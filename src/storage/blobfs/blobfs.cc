// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blobfs.h"

#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <inttypes.h>
#include <lib/async/cpp/task.h>
#include <lib/cksum.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/debug.h>
#include <lib/zx/event.h>
#include <lib/zx/status.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <limits>
#include <memory>
#include <utility>

#include <blobfs/compression-settings.h>
#include <blobfs/fsck.h>
#include <block-client/cpp/pass-through-read-only-device.h>
#include <block-client/cpp/remote-block-device.h>
#include <cobalt-client/cpp/collector.h>
#include <digest/digest.h>
#include <digest/merkle-tree.h>
#include <fbl/auto_call.h>
#include <fbl/ref_ptr.h>
#include <fs/journal/journal.h>
#include <fs/journal/replay.h>
#include <fs/journal/superblock.h>
#include <fs/pseudo_dir.h>
#include <fs/ticker.h>
#include <fs/vfs_types.h>
#include <fvm/client.h>

#include "allocator/extent-reserver.h"
#include "allocator/node-reserver.h"
#include "blob-loader.h"
#include "blob.h"
#include "blobfs-checker.h"
#include "blobfs/format.h"
#include "compression/compressor.h"
#include "iterator/allocated-node-iterator.h"
#include "iterator/block-iterator.h"
#include "pager/transfer-buffer.h"
#include "pager/user-pager-info.h"

namespace blobfs {
namespace {

using ::digest::Digest;
using ::fs::Journal;
using ::fs::JournalSuperblock;
using ::id_allocator::IdAllocator;
using ::storage::BlockingRingBuffer;
using ::storage::VmoidRegistry;

struct DirectoryCookie {
  size_t index;       // Index into node map
  uint64_t reserved;  // Unused
};

// Writeback enabled, journaling enabled.
zx::status<std::unique_ptr<Journal>> InitializeJournal(
    fs::TransactionHandler* transaction_handler, VmoidRegistry* registry, uint64_t journal_start,
    uint64_t journal_length, JournalSuperblock journal_superblock,
    std::shared_ptr<fs::MetricsTrait> journal_metrics) {
  const uint64_t journal_entry_blocks = journal_length - fs::kJournalMetadataBlocks;

  std::unique_ptr<BlockingRingBuffer> journal_buffer;
  zx_status_t status = BlockingRingBuffer::Create(registry, journal_entry_blocks, kBlobfsBlockSize,
                                                  "journal-writeback-buffer", &journal_buffer);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot create journal buffer: %s\n", zx_status_get_string(status));
    return zx::error(status);
  }

  std::unique_ptr<BlockingRingBuffer> writeback_buffer;
  status = BlockingRingBuffer::Create(registry, WriteBufferSize(), kBlobfsBlockSize,
                                      "data-writeback-buffer", &writeback_buffer);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Cannot create writeback buffer: %s\n", zx_status_get_string(status));
    return zx::error(status);
  }

  auto options = Journal::Options();
  options.metrics = journal_metrics;
  return zx::ok(std::make_unique<Journal>(transaction_handler, std::move(journal_superblock),
                                          std::move(journal_buffer), std::move(writeback_buffer),
                                          journal_start, options));
}

const char* CachePolicyToString(CachePolicy policy) {
  switch (policy) {
    case CachePolicy::NeverEvict:
      return "NEVER_EVICT";
    case CachePolicy::EvictImmediately:
      return "EVICT_IMMEDIATELY";
  }
}

}  // namespace

// static.
zx_status_t Blobfs::Create(async_dispatcher_t* dispatcher, std::unique_ptr<BlockDevice> device,
                           const MountOptions& options, zx::resource vmex_resource,
                           std::unique_ptr<Blobfs>* out) {
  TRACE_DURATION("blobfs", "Blobfs::Create");
  char block[kBlobfsBlockSize];
  zx_status_t status = device->ReadBlock(0, kBlobfsBlockSize, block);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: could not read info block\n");
    return status;
  }
  const Superblock* superblock = reinterpret_cast<Superblock*>(&block[0]);

  fuchsia_hardware_block_BlockInfo block_info;
  status = device->BlockGetInfo(&block_info);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: cannot acquire block info: %d\n", status);
    return status;
  }
  uint64_t blocks = (block_info.block_size * block_info.block_count) / kBlobfsBlockSize;

  Writability writability = options.writability;
  if (block_info.flags & BLOCK_FLAG_READONLY) {
    FS_TRACE_WARN("blobfs: Mounting as read-only. WARNING: Journal will not be applied\n");
    writability = blobfs::Writability::ReadOnlyDisk;
  }
  if (kBlobfsBlockSize % block_info.block_size != 0) {
    FS_TRACE_ERROR("blobfs: Blobfs block size (%u) not divisible by device block size (%u)\n",
                   kBlobfsBlockSize, block_info.block_size);
    return ZX_ERR_IO;
  }

  // Perform superblock validations which should succeed prior to journal replay.
  const uint64_t total_blocks = TotalBlocks(*superblock);
  if (blocks < total_blocks) {
    FS_TRACE_ERROR("blobfs: Block size mismatch: (superblock: %zu) vs (actual: %zu)\n",
                   total_blocks, blocks);
    return ZX_ERR_BAD_STATE;
  }
  status = CheckSuperblock(superblock, total_blocks);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Check Superblock failure\n");
    return status;
  }

  // Construct the Blobfs object, without intensive validation, since it
  // may require upgrades / journal replays to become valid.
  auto fs = std::unique_ptr<Blobfs>(new Blobfs(
      dispatcher, std::move(device), superblock, writability, options.compression_settings,
      std::move(vmex_resource), options.pager_backed_cache_policy));
  fs->block_info_ = block_info;

  auto fs_ptr = fs.get();
  auto status_or_buffer = pager::StorageBackedTransferBuffer::Create(
      pager::kTransferBufferSize, fs_ptr, fs_ptr, fs_ptr->Metrics());
  if (!status_or_buffer.is_ok()) {
    FS_TRACE_ERROR("blobfs: Could not initialize pager transfer buffer\n");
    return status_or_buffer.status_value();
  }
  auto status_or_pager =
      pager::UserPager::Create(std::move(status_or_buffer).value(), fs_ptr->Metrics());
  if (!status_or_pager.is_ok()) {
    FS_TRACE_ERROR("blobfs: Could not initialize user pager\n");
    return status_or_pager.status_value();
  }
  fs->pager_ = std::move(status_or_pager).value();
  FS_TRACE_INFO("blobfs: Initialized user pager\n");

  if (options.metrics) {
    fs->metrics_->Collect();
  }

  if (writability == blobfs::Writability::ReadOnlyDisk) {
    FS_TRACE_ERROR("blobfs: Replaying the journal requires a writable disk\n");
    return ZX_ERR_ACCESS_DENIED;
  }
  FS_TRACE_INFO("blobfs: Replaying journal\n");
  auto journal_superblock_or = fs::ReplayJournal(fs.get(), fs.get(), JournalStartBlock(fs->info_),
                                                 JournalBlocks(fs->info_), kBlobfsBlockSize);
  if (journal_superblock_or.is_error()) {
    FS_TRACE_ERROR("blobfs: Failed to replay journal\n");
    return journal_superblock_or.error_value();
  }
  JournalSuperblock journal_superblock = std::move(journal_superblock_or.value());
  FS_TRACE_DEBUG("blobfs: Journal replayed\n");

  switch (writability) {
    case blobfs::Writability::Writable: {
      FS_TRACE_DEBUG("blobfs: Initializing journal for writeback\n");
      auto journal_or =
          InitializeJournal(fs.get(), fs.get(), JournalStartBlock(fs->info_),
                            JournalBlocks(fs->info_), std::move(journal_superblock), fs->metrics_);
      if (journal_or.is_error()) {
        FS_TRACE_ERROR("blobfs: Failed to initialize journal\n");
        return journal_or.error_value();
      }
      fs->journal_ = std::move(journal_or.value());
      if (zx_status_t status = fs->ReloadSuperblock(); status != ZX_OK) {
        FS_TRACE_ERROR("blobfs: Failed to re-load superblock\n");
        return status;
      }
      {
        // Change to true to enable fsck at the end of every transaction, which is useful to check
        // that every transaction leaves the file system in a consistent state.
        bool fsck_at_end_of_every_transaction = false;
        if (fsck_at_end_of_every_transaction) {
          fs->journal_->set_write_metadata_callback(
              fit::bind_member(fs.get(), &Blobfs::FsckAtEndOfTransaction));
        }
      }
      break;
    }
    case blobfs::Writability::ReadOnlyFilesystem:
      // Journal uninitialized.
      break;
    default:
      FS_TRACE_ERROR("blobfs: Unexpected writability option for journaling\n");
      return ZX_ERR_NOT_SUPPORTED;
  }

  // Validate the FVM after replaying the journal.
  status = CheckFvmConsistency(&fs->info_, fs->Device(),
                               /*repair=*/writability != blobfs::Writability::ReadOnlyDisk);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: FVM info check failed\n");
    return status;
  }

  FS_TRACE_INFO("blobfs: Using eviction policy %s\n", CachePolicyToString(options.cache_policy));
  if (options.pager_backed_cache_policy) {
    FS_TRACE_INFO("blobfs: Using overridden pager eviction policy %s\n",
                  CachePolicyToString(*options.pager_backed_cache_policy));
  }
  fs->Cache().SetCachePolicy(options.cache_policy);

  RawBitmap block_map;
  // Keep the block_map aligned to a block multiple
  if ((status = block_map.Reset(BlockMapBlocks(fs->info_) * kBlobfsBlockBits)) < 0) {
    FS_TRACE_ERROR("blobfs: Could not reset block bitmap\n");
    return status;
  }
  if ((status = block_map.Shrink(fs->info_.data_block_count)) < 0) {
    FS_TRACE_ERROR("blobfs: Could not shrink block bitmap\n");
    return status;
  }
  fzl::ResizeableVmoMapper node_map;

  size_t nodemap_size = kBlobfsInodeSize * fs->info_.inode_count;
  ZX_DEBUG_ASSERT(fbl::round_up(nodemap_size, kBlobfsBlockSize) == nodemap_size);
  ZX_DEBUG_ASSERT(nodemap_size / kBlobfsBlockSize == NodeMapBlocks(fs->info_));
  if ((status = node_map.CreateAndMap(nodemap_size, "nodemap")) != ZX_OK) {
    return status;
  }
  std::unique_ptr<IdAllocator> nodes_bitmap = {};
  if ((status = IdAllocator::Create(fs->info_.inode_count, &nodes_bitmap) != ZX_OK)) {
    FS_TRACE_ERROR("blobfs: Failed to allocate bitmap for inodes\n");
    return status;
  }

  fs->allocator_ = std::make_unique<Allocator>(fs.get(), std::move(block_map), std::move(node_map),
                                               std::move(nodes_bitmap));
  if ((status = fs->allocator_->ResetFromStorage(fs::ReadTxn(fs.get()))) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to load bitmaps: %d\n", status);
    return status;
  }
  if ((status = fs->info_mapping_.CreateAndMap(kBlobfsBlockSize, "blobfs-superblock")) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to create info vmo: %d\n", status);
    return status;
  }
  if ((status = fs->BlockAttachVmo(fs->info_mapping_.vmo(), &fs->info_vmoid_)) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to attach info vmo: %d\n", status);
    return status;
  }
  if ((status = fs->CreateFsId()) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to create fs_id: %d\n", status);
    return status;
  }
  if ((status = fs->InitializeVnodes()) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to initialize Vnodes\n");
    return status;
  }
  zx::status<BlobLoader> loader =
      BlobLoader::Create(fs_ptr, fs_ptr, fs->GetNodeFinder(), fs->pager_.get(), fs->Metrics());
  if (!loader.is_ok()) {
    FS_TRACE_ERROR("blobfs: Failed to initialize loader: %s\n", loader.status_string());
    return loader.status_value();
  }
  fs->loader_ = std::move(loader.value());

  // At this point, the filesystem is loaded and validated. No errors should be returned after this
  // point.

  // On a read-write filesystem, since we can now serve writes, we need to unset the kBlobFlagClean
  // flag to indicate that the filesystem may not be in a "clean" state anymore. This helps to make
  // sure we are unmounted cleanly i.e the kBlobFlagClean flag is set back on clean unmount.
  //
  // Additionally, we can now update the oldest_revision field if it needs to be updated.
  FS_TRACE_INFO("blobfs: detected oldest_revision %" PRIu64 ", current revision %" PRIu64 "\n",
                fs->info_.oldest_revision, kBlobfsCurrentRevision);
  if (writability == blobfs::Writability::Writable) {
    BlobTransaction transaction;
    fs->info_.flags &= ~kBlobFlagClean;
    if (fs->info_.oldest_revision > kBlobfsCurrentRevision) {
      FS_TRACE_INFO("Setting oldest_revision to %" PRIu64 "\n", kBlobfsCurrentRevision);
      fs->info_.oldest_revision = kBlobfsCurrentRevision;
    }
    fs->WriteInfo(transaction);
    transaction.Commit(*fs->journal());
  }

  FS_TRACE_INFO(
      "blobfs: Using compression %s\n",
      CompressionAlgorithmToString(fs->write_compression_settings_.compression_algorithm));
  if (fs->write_compression_settings_.compression_level) {
    FS_TRACE_INFO("blobfs: Using overridden compression level %d\n",
                  *(fs->write_compression_settings_.compression_level));
  }

  status = BlobCorruptionNotifier::Create(&(fs->blob_corruption_notifier_));

  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Failed to initialize corruption notifier: %s\n",
                   zx_status_get_string(status));
  }

  *out = std::move(fs);
  return ZX_OK;
}

// static.
std::unique_ptr<BlockDevice> Blobfs::Destroy(std::unique_ptr<Blobfs> blobfs) {
  return blobfs->Reset();
}

Blobfs::~Blobfs() { Reset(); }

zx_status_t Blobfs::LoadAndVerifyBlob(uint32_t node_index) {
  return Blob::LoadAndVerifyBlob(this, node_index);
}

void Blobfs::PersistBlocks(const ReservedExtent& reserved_extent, BlobTransaction& transaction) {
  TRACE_DURATION("blobfs", "Blobfs::PersistBlocks");

  allocator_->MarkBlocksAllocated(reserved_extent);

  const Extent& extent = reserved_extent.extent();
  info_.alloc_block_count += extent.Length();
  // Write out to disk.
  WriteBitmap(extent.Length(), extent.Start(), transaction);
  WriteInfo(transaction);
}

// Frees blocks from reserved and allocated maps, updates disk in the latter case.
void Blobfs::FreeExtent(const Extent& extent, BlobTransaction& transaction) {
  size_t start = extent.Start();
  size_t num_blocks = extent.Length();
  size_t end = start + num_blocks;

  TRACE_DURATION("blobfs", "Blobfs::FreeExtent", "nblocks", num_blocks, "blkno", start);

  // Check if blocks were allocated on disk.
  if (allocator_->CheckBlocksAllocated(start, end)) {
    transaction.AddReservedExtent(allocator_->FreeBlocks(extent));
    info_.alloc_block_count -= num_blocks;
    WriteBitmap(num_blocks, start, transaction);
    WriteInfo(transaction);
    DeleteExtent(DataStartBlock(info_) + start, num_blocks, transaction);
  }
}

void Blobfs::FreeNode(uint32_t node_index, BlobTransaction& transaction) {
  allocator_->FreeNode(node_index);
  info_.alloc_inode_count--;
  WriteNode(node_index, transaction);
}

void Blobfs::FreeInode(uint32_t node_index, BlobTransaction& transaction) {
  TRACE_DURATION("blobfs", "Blobfs::FreeInode", "node_index", node_index);
  InodePtr mapped_inode = GetNode(node_index);

  if (mapped_inode->header.IsAllocated()) {
    // Always write back the first node.
    FreeNode(node_index, transaction);

    AllocatedExtentIterator extent_iter(allocator_.get(), node_index);
    while (!extent_iter.Done()) {
      // If we're observing a new node, free it.
      if (extent_iter.NodeIndex() != node_index) {
        node_index = extent_iter.NodeIndex();
        FreeNode(node_index, transaction);
      }

      const Extent* extent;
      ZX_ASSERT(extent_iter.Next(&extent) == ZX_OK);

      // Free the extent.
      FreeExtent(*extent, transaction);
    }
    WriteInfo(transaction);
  }
}

void Blobfs::PersistNode(uint32_t node_index, BlobTransaction& transaction) {
  TRACE_DURATION("blobfs", "Blobfs::PersistNode");
  info_.alloc_inode_count++;
  WriteNode(node_index, transaction);
  WriteInfo(transaction);
}

size_t Blobfs::WritebackCapacity() const { return WriteBufferSize(); }

void Blobfs::WriteBitmap(uint64_t nblocks, uint64_t start_block, BlobTransaction& transaction) {
  TRACE_DURATION("blobfs", "Blobfs::WriteBitmap", "nblocks", nblocks, "start_block", start_block);
  uint64_t bbm_start_block = start_block / kBlobfsBlockBits;
  uint64_t bbm_end_block =
      fbl::round_up(start_block + nblocks, kBlobfsBlockBits) / kBlobfsBlockBits;

  // Write back the block allocation bitmap
  transaction.AddOperation({.vmo = zx::unowned_vmo(allocator_->GetBlockMapVmo().get()),
                            {
                                .type = storage::OperationType::kWrite,
                                .vmo_offset = bbm_start_block,
                                .dev_offset = BlockMapStartBlock(info_) + bbm_start_block,
                                .length = bbm_end_block - bbm_start_block,
                            }});
}

void Blobfs::WriteNode(uint32_t map_index, BlobTransaction& transaction) {
  TRACE_DURATION("blobfs", "Blobfs::WriteNode", "map_index", map_index);
  uint64_t block = (map_index * sizeof(Inode)) / kBlobfsBlockSize;
  transaction.AddOperation({.vmo = zx::unowned_vmo(allocator_->GetNodeMapVmo().get()),
                            {
                                .type = storage::OperationType::kWrite,
                                .vmo_offset = block,
                                .dev_offset = NodeMapStartBlock(info_) + block,
                                .length = 1,
                            }});
}

void Blobfs::WriteInfo(BlobTransaction& transaction) {
  memcpy(info_mapping_.start(), &info_, sizeof(info_));
  transaction.AddOperation({
      .vmo = zx::unowned_vmo(info_mapping_.vmo().get()),
      {
          .type = storage::OperationType::kWrite,
          .vmo_offset = 0,
          .dev_offset = 0,
          .length = 1,
      },
  });
}

void Blobfs::DeleteExtent(uint64_t start_block, uint64_t num_blocks,
                          BlobTransaction& transaction) const {
  if (block_info_.flags & fuchsia_hardware_block_FLAG_TRIM_SUPPORT) {
    TRACE_DURATION("blobfs", "Blobfs::DeleteExtent", "num_blocks", num_blocks, "start_block",
                   start_block);
    storage::BufferedOperation operation = {};
    operation.op.type = storage::OperationType::kTrim;
    operation.op.dev_offset = start_block;
    operation.op.length = num_blocks;
    transaction.AddTrimOperation(operation);
  }
}

zx_status_t Blobfs::CreateFsId() {
  ZX_DEBUG_ASSERT(!fs_id_legacy_);
  ZX_DEBUG_ASSERT(!fs_id_.is_valid());
  zx::event event;
  zx_status_t status = zx::event::create(0, &event);
  if (status != ZX_OK) {
    return status;
  }
  zx_info_handle_basic_t info;
  status = event.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    return status;
  }

  fs_id_ = std::move(event);
  fs_id_legacy_ = info.koid;
  return ZX_OK;
}

zx_status_t Blobfs::GetFsId(zx::event* out_fs_id) const {
  ZX_DEBUG_ASSERT(fs_id_.is_valid());
  return fs_id_.duplicate(ZX_RIGHTS_BASIC, out_fs_id);
}

static_assert(sizeof(DirectoryCookie) <= sizeof(fs::vdircookie_t),
              "Blobfs dircookie too large to fit in IO state");

zx_status_t Blobfs::Readdir(fs::vdircookie_t* cookie, void* dirents, size_t len,
                            size_t* out_actual) {
  TRACE_DURATION("blobfs", "Blobfs::Readdir", "len", len);
  fs::DirentFiller df(dirents, len);
  DirectoryCookie* c = reinterpret_cast<DirectoryCookie*>(cookie);

  for (size_t i = c->index; i < info_.inode_count; ++i) {
    ZX_DEBUG_ASSERT(i < std::numeric_limits<uint32_t>::max());
    uint32_t node_index = static_cast<uint32_t>(i);
    if (GetNode(node_index)->header.IsAllocated() &&
        !GetNode(node_index)->header.IsExtentContainer()) {
      Digest digest(GetNode(node_index)->merkle_root_hash);
      auto name = digest.ToString();
      uint64_t ino = ::llcpp::fuchsia::io::INO_UNKNOWN;
      if (df.Next(name.ToStringPiece(), VTYPE_TO_DTYPE(V_TYPE_FILE), ino) != ZX_OK) {
        break;
      }
      c->index = i + 1;
    }
  }

  *out_actual = df.BytesFilled();
  return ZX_OK;
}

zx_status_t Blobfs::BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out) {
  zx_status_t status = Device()->BlockAttachVmo(vmo, out);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Failed to attach blob VMO: %s\n", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t Blobfs::BlockDetachVmo(storage::Vmoid vmoid) {
  return Device()->BlockDetachVmo(std::move(vmoid));
}

zx_status_t Blobfs::AddInodes(Allocator* allocator) {
  TRACE_DURATION("blobfs", "Blobfs::AddInodes");

  if (!(info_.flags & kBlobFlagFVM)) {
    return ZX_ERR_NO_SPACE;
  }

  const size_t blocks_per_slice = info_.slice_size / kBlobfsBlockSize;
  uint64_t offset = (kFVMNodeMapStart / blocks_per_slice) + info_.ino_slices;
  uint64_t length = 1;
  zx_status_t status = Device()->VolumeExtend(offset, length);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Blobfs::AddInodes fvm_extend failure: %s", zx_status_get_string(status));
    return status;
  }

  const uint32_t kInodesPerSlice = static_cast<uint32_t>(info_.slice_size / kBlobfsInodeSize);
  uint64_t inodes64 = (info_.ino_slices + static_cast<uint32_t>(length)) * kInodesPerSlice;
  ZX_DEBUG_ASSERT(inodes64 <= std::numeric_limits<uint32_t>::max());
  uint32_t inodes = static_cast<uint32_t>(inodes64);
  uint32_t inoblks = (inodes + kBlobfsInodesPerBlock - 1) / kBlobfsInodesPerBlock;
  ZX_DEBUG_ASSERT(info_.inode_count <= std::numeric_limits<uint32_t>::max());
  uint32_t inoblks_old = (static_cast<uint32_t>(info_.inode_count) + kBlobfsInodesPerBlock - 1) /
                         kBlobfsInodesPerBlock;
  ZX_DEBUG_ASSERT(inoblks_old <= inoblks);

  if (allocator->GrowNodeMap(inoblks * kBlobfsBlockSize) != ZX_OK) {
    return ZX_ERR_NO_SPACE;
  }

  info_.ino_slices += static_cast<uint32_t>(length);
  info_.inode_count = inodes;

  // Reset new inodes to 0, and update the info block.
  uint64_t zeroed_nodes_blocks = inoblks - inoblks_old;
  // Use GetNode to get a pointer to the first node we need to zero and also to keep the map locked
  // whilst we zero them.
  InodePtr new_nodes = allocator->GetNode(inoblks_old * kBlobfsInodesPerBlock);
  memset(&*new_nodes, 0, kBlobfsBlockSize * zeroed_nodes_blocks);

  BlobTransaction transaction;
  WriteInfo(transaction);
  if (zeroed_nodes_blocks > 0) {
    transaction.AddOperation({
        .vmo = zx::unowned_vmo(allocator->GetNodeMapVmo().get()),
        {
            .type = storage::OperationType::kWrite,
            .vmo_offset = inoblks_old,
            .dev_offset = NodeMapStartBlock(info_) + inoblks_old,
            .length = zeroed_nodes_blocks,
        },
    });
  }
  transaction.Commit(*journal_);
  return ZX_OK;
}

zx_status_t Blobfs::AddBlocks(size_t nblocks, RawBitmap* block_map) {
  TRACE_DURATION("blobfs", "Blobfs::AddBlocks", "nblocks", nblocks);

  if (!(info_.flags & kBlobFlagFVM)) {
    return ZX_ERR_NO_SPACE;
  }

  const size_t blocks_per_slice = info_.slice_size / kBlobfsBlockSize;
  // Number of slices required to add nblocks
  uint64_t offset = (kFVMDataStart / blocks_per_slice) + info_.dat_slices;
  uint64_t length = (nblocks + blocks_per_slice - 1) / blocks_per_slice;

  uint64_t blocks64 = (info_.dat_slices + length) * blocks_per_slice;
  ZX_DEBUG_ASSERT(blocks64 <= std::numeric_limits<uint32_t>::max());
  uint32_t blocks = static_cast<uint32_t>(blocks64);
  uint32_t abmblks = (blocks + kBlobfsBlockBits - 1) / kBlobfsBlockBits;
  uint64_t abmblks_old = (info_.data_block_count + kBlobfsBlockBits - 1) / kBlobfsBlockBits;
  ZX_DEBUG_ASSERT(abmblks_old <= abmblks);

  if (abmblks > blocks_per_slice) {
    // TODO(planders): Allocate more slices for the block bitmap.
    FS_TRACE_ERROR("Blobfs::AddBlocks needs to increase block bitmap size\n");
    return ZX_ERR_NO_SPACE;
  }

  zx_status_t status = Device()->VolumeExtend(offset, length);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("Blobfs::AddBlocks FVM Extend failure: %s\n", zx_status_get_string(status));
    return status;
  }

  // Grow the block bitmap to hold new number of blocks
  if (block_map->Grow(fbl::round_up(blocks, kBlobfsBlockBits)) != ZX_OK) {
    return ZX_ERR_NO_SPACE;
  }
  // Grow before shrinking to ensure the underlying storage is a multiple
  // of kBlobfsBlockSize.
  block_map->Shrink(blocks);

  info_.dat_slices += static_cast<uint32_t>(length);
  info_.data_block_count = blocks;

  BlobTransaction transaction;
  WriteInfo(transaction);
  uint64_t zeroed_bitmap_blocks = abmblks - abmblks_old;
  // Since we are extending the bitmap, we need to fill the expanded
  // portion of the allocation block bitmap with zeroes.
  if (zeroed_bitmap_blocks > 0) {
    storage::UnbufferedOperation operation = {
        .vmo = zx::unowned_vmo(block_map->StorageUnsafe()->GetVmo().get()),
        {
            .type = storage::OperationType::kWrite,
            .vmo_offset = abmblks_old,
            .dev_offset = BlockMapStartBlock(info_) + abmblks_old,
            .length = zeroed_bitmap_blocks,
        },
    };
    transaction.AddOperation(operation);
  }
  transaction.Commit(*journal_);
  return ZX_OK;
}

constexpr const char kFsName[] = "blobfs";
void Blobfs::GetFilesystemInfo(FilesystemInfo* info) const {
  static_assert(fbl::constexpr_strlen(kFsName) + 1 < ::llcpp::fuchsia::io::MAX_FS_NAME_BUFFER,
                "Blobfs name too long");

  *info = {};
  info->block_size = kBlobfsBlockSize;
  info->max_filename_size = digest::kSha256HexLength;
  info->fs_type = VFS_TYPE_BLOBFS;
  info->fs_id = GetFsIdLegacy();
  info->total_bytes = Info().data_block_count * Info().block_size;
  info->used_bytes = Info().alloc_block_count * Info().block_size;
  info->total_nodes = Info().inode_count;
  info->used_nodes = Info().alloc_inode_count;
  strlcpy(reinterpret_cast<char*>(info->name.data()), kFsName,
          ::llcpp::fuchsia::io::MAX_FS_NAME_BUFFER);
}

BlockIterator Blobfs::BlockIteratorByNodeIndex(uint32_t node_index) {
  return BlockIterator(std::make_unique<AllocatedExtentIterator>(GetAllocator(), node_index));
}

void Blobfs::Sync(SyncCallback cb) {
  TRACE_DURATION("blobfs", "Blobfs::Sync");
  if (journal_ == nullptr) {
    return cb(ZX_OK);
  }

  auto trace_id = TRACE_NONCE();
  TRACE_FLOW_BEGIN("blobfs", "Blobfs.sync", trace_id);

  journal_->schedule_task(journal_->Sync().then(
      [trace_id, cb = std::move(cb)](fit::result<void, zx_status_t>& result) mutable {
        TRACE_DURATION("blobfs", "Blobfs::Sync::callback");

        if (result.is_ok()) {
          cb(ZX_OK);
        } else {
          cb(result.error());
        }

        TRACE_FLOW_END("blobfs", "Blobfs.sync", trace_id);
      }));
}

Blobfs::Blobfs(async_dispatcher_t* dispatcher, std::unique_ptr<BlockDevice> device,
               const Superblock* info, Writability writable,
               CompressionSettings write_compression_settings, zx::resource vmex_resource,
               std::optional<CachePolicy> pager_backed_cache_policy)
    : info_(*info),
      dispatcher_(dispatcher),
      block_device_(std::move(device)),
      writability_(writable),
      write_compression_settings_(write_compression_settings),
      vmex_resource_(std::move(vmex_resource)),
      pager_backed_cache_policy_(pager_backed_cache_policy) {}

std::unique_ptr<BlockDevice> Blobfs::Reset() {
  // XXX This function relies on very subtle orderings and assumptions about the state of the
  // filesystem. Proceed with caution whenever making changes to Blobfs::Reset(), and consult the
  // blame history for the graveyard of bugs past.
  // TODO(fxbug.dev/56464): simplify the teardown path.
  if (!block_device_) {
    return nullptr;
  }

  FS_TRACE_INFO("blobfs: Shutting down\n");

  // Shutdown all internal connections to blobfs.
  Cache().ForAllOpenNodes([](fbl::RefPtr<CacheNode> cache_node) {
    auto vnode = fbl::RefPtr<Blob>::Downcast(std::move(cache_node));
    vnode->CloneWatcherTeardown();
  });

  // Reset |loader_| now, since it has internally allocated buffers attached to the FIFO it needs to
  // detach.
  loader_.Reset();

  // Write the clean bit.
  if (writability_ == Writability::Writable) {
    // TODO(fxbug.dev/42174): If blobfs initialization failed, it is possible that the
    // info_mapping_ vmo that we use to send writes to the underlying block device
    // has not been initialized yet. Change Blobfs::Create ordering to try and get
    // the object into a valid state as soon as possible and reassess what is needed
    // in the destructor.
    if (info_mapping_.start() == nullptr) {
      FS_TRACE_ERROR("blobfs: Cannot write journal clean bit\n");
    } else {
      BlobTransaction transaction;
      info_.flags |= kBlobFlagClean;
      WriteInfo(transaction);
      transaction.Commit(*journal_);
    }
  }
  // Waits for all pending writeback operations to complete or fail.
  journal_.reset();

  // Reset |pager_| which owns a VMO that is attached to the block FIFO.
  pager_ = nullptr;

  // Flushes the underlying block device.
  fs::WriteTxn sync_txn(this);
  sync_txn.EnqueueFlush();
  sync_txn.Transact();

  BlockDetachVmo(std::move(info_vmoid_));

  return std::move(block_device_);
}

zx_status_t Blobfs::InitializeVnodes() {
  Cache().Reset();
  uint32_t total_allocated = 0;

  for (uint32_t node_index = 0; node_index < info_.inode_count; node_index++) {
    const InodePtr inode = GetNode(node_index);
    // We are not interested in free nodes.
    if (!inode->header.IsAllocated()) {
      continue;
    }
    total_allocated++;

    allocator_->MarkNodeAllocated(node_index);

    // Nothing much to do here if this is not an Inode
    if (inode->header.IsExtentContainer()) {
      continue;
    }

    zx_status_t validation_status =
        AllocatedExtentIterator::VerifyIteration(GetNodeFinder(), inode.get());
    if (validation_status != ZX_OK) {
      // Whatever the more differentiated error is here, the real root issue is
      // the integrity of the data that was just mirrored from the disk.
      return ZX_ERR_IO_DATA_INTEGRITY;
    }

    fbl::RefPtr<Blob> vnode = fbl::MakeRefCounted<Blob>(this, node_index, *inode);

    // This blob is added to the cache, where it will quickly be relocated into the "closed
    // set" once we drop our reference to |vnode|. Although we delay reading any of the
    // contents of the blob from disk until requested, this pre-caching scheme allows us to
    // quickly verify or deny the presence of a blob during blob lookup and creation.
    zx_status_t status = Cache().Add(vnode);
    if (status != ZX_OK) {
      Digest digest(vnode->GetNode().merkle_root_hash);
      FS_TRACE_ERROR("blobfs: CORRUPTED FILESYSTEM: Duplicate node: %s @ index %u\n",
                     digest.ToString().c_str(), node_index - 1);
      return status;
    }
    metrics_->IncrementCompressionFormatMetric(*inode);
  }

  if (total_allocated != info_.alloc_inode_count) {
    FS_TRACE_ERROR(
        "blobfs: CORRUPTED FILESYSTEM: Allocated nodes mismatch. Expected:%lu. Found: %u\n",
        info_.alloc_inode_count, total_allocated);
    return ZX_ERR_IO_OVERRUN;
  }

  return ZX_OK;
}

zx_status_t Blobfs::ReloadSuperblock() {
  TRACE_DURATION("blobfs", "Blobfs::ReloadSuperblock");

  // Re-read the info block from disk.
  char block[kBlobfsBlockSize];
  zx_status_t status = Device()->ReadBlock(0, kBlobfsBlockSize, block);
  if (status != ZX_OK) {
    FS_TRACE_ERROR("blobfs: could not read info block\n");
    return status;
  }

  Superblock* info = reinterpret_cast<Superblock*>(&block[0]);
  if ((status = CheckSuperblock(info, TotalBlocks(*info))) != ZX_OK) {
    FS_TRACE_ERROR("blobfs: Check info failure\n");
    return status;
  }

  // Once it has been verified, overwrite the current info.
  memcpy(&info_, info, sizeof(Superblock));
  return ZX_OK;
}

zx_status_t Blobfs::OpenRootNode(fbl::RefPtr<fs::Vnode>* out) {
  fbl::RefPtr<Directory> vn = fbl::AdoptRef(new Directory(this));

  auto validated_options = vn->ValidateOptions(fs::VnodeConnectionOptions());
  if (validated_options.is_error()) {
    return validated_options.error();
  }
  zx_status_t status = vn->Open(validated_options.value(), nullptr);
  if (status != ZX_OK) {
    return status;
  }

  *out = std::move(vn);
  return ZX_OK;
}

Journal* Blobfs::journal() { return journal_.get(); }

void Blobfs::FsckAtEndOfTransaction(zx_status_t status) {
  if (status == ZX_OK) {
    std::scoped_lock lock(fsck_at_end_of_transaction_mutex_);
    auto device =
        std::make_unique<block_client::PassThroughReadOnlyBlockDevice>(block_device_.get());
    MountOptions options;
    options.writability = Writability::ReadOnlyDisk;
    ZX_ASSERT(Fsck(std::move(device), options) == ZX_OK);
  }
}

zx_status_t Blobfs::RunRequests(const std::vector<storage::BufferedOperation>& operations) {
  std::shared_lock lock(fsck_at_end_of_transaction_mutex_);
  return TransactionManager::RunRequests(operations);
}

std::shared_ptr<BlobfsMetrics> Blobfs::CreateMetrics() {
  bool enable_page_in_metrics = false;
#ifdef BLOBFS_ENABLE_PAGE_IN_METRICS
  enable_page_in_metrics = true;
#endif
  return std::make_shared<BlobfsMetrics>(enable_page_in_metrics);
}

}  // namespace blobfs
