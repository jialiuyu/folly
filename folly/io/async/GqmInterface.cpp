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

#include <folly/io/async/GqmInterface.h>

#include <cstring>

namespace folly {

// External GQM functions declaration
// These functions should be provided by your hardware queue library
extern "C" {
void gqm_push(void* msg, size_t len);
void* gqm_pop();
}

void DefaultGqmInterface::push(const GqmNotification& notification) {
  uint64_t msg = notification.toUint64();
  gqm_push(&msg, sizeof(msg));
}

folly::Optional<GqmNotification> DefaultGqmInterface::pop() {
  void* result = gqm_pop();
  if (result == nullptr) {
    return folly::none;
  }

  uint64_t msg;
  std::memcpy(&msg, result, sizeof(msg));
  return GqmNotification::fromUint64(msg);
}

} // namespace folly
