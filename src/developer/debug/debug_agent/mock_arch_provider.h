// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_ARCH_PROVIDER_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_ARCH_PROVIDER_H_

#include <functional>
#include <map>

#include "src/developer/debug/debug_agent/arch.h"

namespace debug_agent {

// This is meant to mock the platform and being able to track what installations
// the code is doing within the tests.
class MockArchProvider : public arch::ArchProvider {
 public:
  zx_status_t ReadDebugState(const zx::thread& handle,
                             zx_thread_state_debug_regs* regs) const override;
  zx_status_t WriteDebugState(const zx::thread& handle,
                              const zx_thread_state_debug_regs& regs) override;
  zx_status_t WriteSingleStep(const zx::thread& thread, bool single_step) override;
  zx_status_t GetInfo(const zx::thread&, zx_object_info_topic_t topic, void* buffer,
                      size_t buffer_size, size_t* actual, size_t* avail) const override;
  void FillExceptionRecord(const zx::thread&, debug_ipc::ExceptionRecord* out) const override;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_MOCK_ARCH_PROVIDER_H_
