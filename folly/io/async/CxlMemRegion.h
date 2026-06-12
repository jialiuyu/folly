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
#include <string>

namespace folly {

struct CxlMemRegionConfig {
  std::string path;
  size_t size{0};
  bool createIfMissing{false};
  bool cacheCoherentMapping{false};
};

class CxlMemRegion {
 public:
  explicit CxlMemRegion(CxlMemRegionConfig config);
  ~CxlMemRegion();

  CxlMemRegion(const CxlMemRegion&) = delete;
  CxlMemRegion& operator=(const CxlMemRegion&) = delete;

  CxlMemRegion(CxlMemRegion&& other) noexcept;
  CxlMemRegion& operator=(CxlMemRegion&& other) noexcept;

  unsigned char* data() const { return data_; }
  size_t size() const { return size_; }
  const std::string& path() const { return path_; }
  bool cacheCoherentMapping() const { return cacheCoherentMapping_; }

 private:
  void close() noexcept;

  std::string path_;
  unsigned char* data_{nullptr};
  size_t size_{0};
  int fd_{-1};
  bool cacheCoherentMapping_{false};
};

} // namespace folly
