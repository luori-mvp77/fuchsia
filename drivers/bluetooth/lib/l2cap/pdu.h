// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <endian.h>
#include <mxtl/intrusive_double_list.h>

#include "apps/bluetooth/lib/hci/acl_data_packet.h"
#include "apps/bluetooth/lib/l2cap/l2cap.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/macros.h"

namespace bluetooth {
namespace l2cap {

// Represents a L2CAP PDU. Each PDU contains a complete L2CAP frame that can be transmitted over a
// L2CAP channel. PDUs are the building blocks for SDUs (higher-layer service data units).
//
// A PDU is composed of one or more fragments, each contained in a HCI ACL data packet. A PDU cannot
// be populated directly and must be obtained from a Recombiner or Fragmenter.
//
// A PDU instance is light-weight (it consists of a single unique_ptr via mxtl::DoublyLinkedList)
// and can be passed around by value. As the PDU uniquely owns its chain of fragments, a PDU is
// move-only.
//
// THREAD-SAFETY:
//
// This class is not thread-safe. External locking should be provided if an instance will be
// accessed on multiple threads.
class PDU final {
 public:
  using FragmentList = mxtl::DoublyLinkedList<std::unique_ptr<hci::ACLDataPacket>>;

  PDU();
  ~PDU() = default;

  // Allow move operations.
  PDU(PDU&& other);
  PDU& operator=(PDU&& other);

  // An unpopulated PDU is considered invalid, which is the default-constructed state.
  bool is_valid() const {
    FTL_DCHECK(fragments_.is_empty() && !fragment_count_ ||
               !fragments_.is_empty() && fragment_count_);
    return !fragments_.is_empty();
  }

  // The number of fragments that are currently a part of this PDU.
  size_t fragment_count() const { return fragment_count_; }

  // Returns the number of bytes that are currently contained in this PDU, excluding the Basic L2CAP
  // header.
  uint16_t length() const { return le16toh(basic_header().length); }

  // The L2CAP channel that this packet belongs to.
  ChannelId channel_id() const { return le16toh(basic_header().channel_id); }

  // The connection handle that identifies the logical link this PDU is intended for.
  hci::ConnectionHandle connection_handle() const {
    FTL_DCHECK(is_valid());
    return fragments_.begin()->connection_handle();
  }

  // This method will attempt to read |size| bytes of the basic-frame information payload (i.e.
  // contents of this PDU excludng the basic L2CAP header) starting at offset |pos| and copy the
  // contents into |out_buffer|.
  //
  // The amount read may be smaller then the amount requested if the PDU does not have enough data.
  // |out_buffer| should be sufficiently large.
  //
  // Returns the number of bytes copied into |out_buffer|.
  //
  // NOTE: Use this method wisely as it can be costly. In particular, large values of |pos|
  // will incur a cost (O(pos)) as the underlying fragments need to be traversed to find the initial
  // fragment.
  size_t Read(common::MutableByteBuffer* out_buffer, size_t pos = 0,
              size_t size = std::numeric_limits<std::size_t>::max()) const;

  // Relinquishes ownership of the underlying list of fragments and returns it. Once this is called,
  // the PDU will become invalid.
  void ReleaseFragments(FragmentList* out_list);

 private:
  friend class Fragmenter;
  friend class Recombiner;

  // Methods accessed by friends.
  const BasicHeader& basic_header() const;

  // Takes ownership of |fragment| and adds it to |fragments_|. This method assumes that validity
  // checks on |fragment| have already been performed.
  void AppendFragment(std::unique_ptr<hci::ACLDataPacket> fragment);

  // The number of fragments currently stored in this PDU.
  size_t fragment_count_;

  // ACL data fragments that currently form this PDU. In a complete PDU, it is expected that the sum
  // of the payload sizes of all elements in |fragments_| is equal to the length of the frame
  // (i.e. length() + sizeof(BasicHeader)).
  FragmentList fragments_;

  FTL_DISALLOW_COPY_AND_ASSIGN(PDU);
};

}  // namespace l2cap
}  // namespace bluetooth
