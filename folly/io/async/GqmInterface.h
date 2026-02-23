/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <folly/Optional.h>

namespace folly {

/**
 * GQM (Hardware Queue) notification message structure.
 * The message is 64 bits total: 32-bit offset + 32-bit length.
 */
struct GqmNotification {
  uint32_t offset;  // Offset in shared memory where data starts
  uint32_t length;  // Length of data written

  uint64_t toUint64() const {
    return (static_cast<uint64_t>(offset) << 32) | length;
  }

  static GqmNotification fromUint64(uint64_t value) {
    return GqmNotification{
        static_cast<uint32_t>(value >> 32),
        static_cast<uint32_t>(value & 0xFFFFFFFF)};
  }
};

/**
 * GQM (Hardware Queue) interface for notification.
 * This is an abstract interface that should be implemented by the actual
 * hardware queue mechanism.
 *
 * The default implementation uses the external C functions:
 *   - gqm_push(void* msg, size_t len)
 *   - gqm_pop()
 *
 * These functions should be provided by your hardware queue library.
 */
class GqmInterface {
 public:
  virtual ~GqmInterface() = default;

  /**
   * Push a notification to the queue.
   * The message must fit in 64 bits.
   */
  virtual void push(const GqmNotification& notification) = 0;

  /**
   * Pop a notification from the queue.
   * Returns folly::none if no notification is available.
   */
  virtual folly::Optional<GqmNotification> pop() = 0;
};

/**
 * Default GQM implementation using the provided C functions.
 *
 * IMPORTANT: The following C functions must be linked:
 *   void gqm_push(void* msg, size_t len);
 *   void* gqm_pop();
 *
 * The gqm_push function is expected to be atomic and thread-safe.
 * The gqm_pop function returns a pointer to the 64-bit message, or nullptr
 * if no message is available. The message is consumed (read-once).
 */
class DefaultGqmInterface : public GqmInterface {
 public:
  void push(const GqmNotification& notification) override;
  folly::Optional<GqmNotification> pop() override;
};

/**
 * Null GQM implementation for testing or when notifications are not needed.
 * Useful for unit tests where you want to verify behavior without actual
 * hardware queues.
 */
class NullGqmInterface : public GqmInterface {
 public:
  void push(const GqmNotification& notification) override {
    // No-op for testing
    (void)notification;
  }

  folly::Optional<GqmNotification> pop() override {
    // Always return empty for testing
    return folly::none;
  }
};

} // namespace folly
