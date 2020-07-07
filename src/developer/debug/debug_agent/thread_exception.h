// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_THREAD_EXCEPTION_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_THREAD_EXCEPTION_H_

#include <lib/fitx/result.h>

#include "src/lib/fxl/macros.h"

namespace debug_agent {

// ThreadException abstracts zx::exception, allowing for a more straightforward
// implementation in tests in overrides of this class.
class ThreadException {
 public:
  ThreadException() = default;
  virtual ~ThreadException() = default;

  // Returns the koid of the exception's thread or ZX_KOID_INVALID if the
  // exception is invalid.
  virtual fitx::result<zx_status_t, zx_koid_t> GetThreadKoid() const = 0;

  // Returns the associated ZX_EXCEPTION_STATE_* constant characterizing
  // the state of the exception.
  virtual fitx::result<zx_status_t, uint32_t> GetState() const = 0;

  // Given a ZX_EXCEPTION_STATE_* constant, sets the state of the exception.
  virtual zx_status_t SetState(uint32_t state) = 0;

  // Returns the associated ZX_EXCEPTION_STRATEGY_* constant characterizing
  // the exception handling strategy.
  virtual fitx::result<zx_status_t, uint32_t> GetStrategy() const = 0;

  // Given a ZX_EXCEPTION_STRAGEY_* constant, sets the handling strategy.
  virtual zx_status_t SetStrategy(uint32_t strategy) = 0;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(ThreadException);
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_THREAD_EXCEPTION_H_