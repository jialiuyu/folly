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

#include <folly/io/async/CxlMemRegion.h>

#include <folly/portability/GTest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace folly {
namespace {

class TempPath {
 public:
  TempPath() {
    char path[] = "/tmp/folly_cxl_mem_region_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
      throw std::runtime_error(
          std::string("mkstemp failed: ") + std::strerror(errno));
    }
    ::close(fd);
    ::unlink(path);
    path_ = path;
  }

  ~TempPath() { ::unlink(path_.c_str()); }

  const std::string& get() const { return path_; }

 private:
  std::string path_;
};

off_t fileSize(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) == -1) {
    throw std::runtime_error(
        std::string("stat failed: ") + std::strerror(errno));
  }
  return st.st_size;
}

} // namespace

TEST(CxlMemRegion, createsAndMapsWritableFile) {
  TempPath path;

  CxlMemRegion region(CxlMemRegionConfig{
      path.get(),
      4096,
      true,
      true,
  });

  EXPECT_EQ(path.get(), region.path());
  EXPECT_EQ(4096, region.size());
  EXPECT_TRUE(region.cacheCoherentMapping());
  ASSERT_NE(nullptr, region.data());
  region.data()[0] = 0x7f;
  region.data()[4095] = 0x55;
  EXPECT_EQ(4096, fileSize(path.get()));
}

TEST(CxlMemRegion, openingMissingFileWithoutCreateThrows) {
  TempPath path;

  EXPECT_THROW(
      CxlMemRegion(CxlMemRegionConfig{
          path.get(),
          4096,
          false,
          false,
      }),
      std::system_error);
}

TEST(CxlMemRegion, rejectsZeroSizeMapping) {
  TempPath path;

  EXPECT_THROW(
      CxlMemRegion(CxlMemRegionConfig{
          path.get(),
          0,
          true,
          false,
      }),
      std::invalid_argument);
}

TEST(CxlMemRegion, rejectsExistingFileThatIsTooSmallWithoutCreate) {
  TempPath path;
  int fd = ::open(path.get().c_str(), O_CREAT | O_RDWR, 0600);
  ASSERT_NE(-1, fd);
  ASSERT_EQ(0, ::ftruncate(fd, 128));
  ::close(fd);

  EXPECT_THROW(
      CxlMemRegion(CxlMemRegionConfig{
          path.get(),
          4096,
          false,
          false,
      }),
      std::invalid_argument);
}

} // namespace folly
