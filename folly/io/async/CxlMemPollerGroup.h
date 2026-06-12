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

#include <folly/io/async/CxlMemPoller.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace folly {

struct CxlMemPollerGroupConfig {
  size_t pollerThreads{0};
  size_t ioPerPoller{4};
  size_t hwQueuesPerDoorbell{2};
  CxlMemPollerIdleProfile idleProfile{CxlMemPollerIdleProfile::ADAPTIVE};
  size_t evbDrainMaxItems{64};
  std::chrono::microseconds evbDrainMaxDuration{50};
};

template <typename Backend>
class CxlMemPollerGroup {
 public:
  explicit CxlMemPollerGroup(
      size_t ioShards,
      CxlMemPollerGroupConfig config = {});

  size_t pollerCount() const;
  size_t pollerIndexForShard(size_t ioShard) const;
  CxlMemPoller<Backend>& pollerForShard(size_t ioShard);
  const CxlMemPollerGroupConfig& config() const;

 private:
  CxlMemPollerGroupConfig config_;
  std::vector<std::unique_ptr<CxlMemPoller<Backend>>> pollers_;
};

extern template class CxlMemPollerGroup<CxlMemStubHWQueueBackend>;
extern template class CxlMemPollerGroup<CxlMemRealHWQueueBackend>;

} // namespace folly
