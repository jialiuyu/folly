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

#include <folly/io/async/CxlMemPollerGroup.h>

#include <algorithm>
#include <stdexcept>

namespace folly {

template <typename Backend>
CxlMemPollerGroup<Backend>::CxlMemPollerGroup(
    size_t ioShards,
    CxlMemPollerGroupConfig config)
    : config_(config) {
  if (config_.ioPerPoller == 0) {
    throw std::invalid_argument("CxlMemPollerGroup requires ioPerPoller");
  }

  const size_t autoPollers =
      (ioShards + config_.ioPerPoller - 1) / config_.ioPerPoller;
  const size_t pollerCount =
      config_.pollerThreads == 0 ? autoPollers : config_.pollerThreads;

  CxlMemPollerOptions pollerOptions;
  pollerOptions.idleProfile = config_.idleProfile;
  pollers_.reserve(pollerCount);
  for (size_t i = 0; i < pollerCount; ++i) {
    pollers_.push_back(std::make_unique<CxlMemPoller<Backend>>(pollerOptions));
  }
}

template <typename Backend>
size_t CxlMemPollerGroup<Backend>::pollerCount() const {
  return pollers_.size();
}

template <typename Backend>
size_t CxlMemPollerGroup<Backend>::pollerIndexForShard(size_t ioShard) const {
  if (pollers_.empty()) {
    throw std::invalid_argument("CxlMemPollerGroup has no pollers");
  }
  return std::min(ioShard / config_.ioPerPoller, pollers_.size() - 1);
}

template <typename Backend>
CxlMemPoller<Backend>& CxlMemPollerGroup<Backend>::pollerForShard(
    size_t ioShard) {
  return *pollers_.at(pollerIndexForShard(ioShard));
}

template <typename Backend>
const CxlMemPollerGroupConfig& CxlMemPollerGroup<Backend>::config() const {
  return config_;
}

template class CxlMemPollerGroup<CxlMemStubHWQueueBackend>;
template class CxlMemPollerGroup<CxlMemRealHWQueueBackend>;

} // namespace folly
