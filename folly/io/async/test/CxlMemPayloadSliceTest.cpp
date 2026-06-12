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

#include <folly/io/async/CxlMemPayloadSlice.h>

#include <folly/portability/GTest.h>

#include <cstdint>
#include <stdexcept>

namespace folly {

TEST(CxlMemPayloadSlice, reserveCommitReducesFreeSpace) {
  CxlMemPayloadSlice slice(CxlMemPayloadSliceConfig{1000, 16});

  uint64_t offset = 0;
  ASSERT_TRUE(slice.reserve(4, &offset));
  EXPECT_EQ(1000, offset);
  slice.commit(offset, 4);
  EXPECT_EQ(12, slice.freeBytes());

  ASSERT_TRUE(slice.reserve(6, &offset));
  EXPECT_EQ(1004, offset);
  slice.commit(offset, 6);
  EXPECT_EQ(6, slice.freeBytes());
}

TEST(CxlMemPayloadSlice, releaseThroughRestoresFreeSpace) {
  CxlMemPayloadSlice slice(CxlMemPayloadSliceConfig{0, 16});

  uint64_t offset = 0;
  ASSERT_TRUE(slice.reserve(8, &offset));
  slice.commit(offset, 8);
  EXPECT_EQ(8, slice.freeBytes());

  slice.releaseThrough(8);
  EXPECT_EQ(16, slice.freeBytes());
}

TEST(CxlMemPayloadSlice, reserveWrapsWhenTailSpaceIsTooSmall) {
  CxlMemPayloadSlice slice(CxlMemPayloadSliceConfig{200, 8});

  uint64_t offset = 0;
  ASSERT_TRUE(slice.reserve(6, &offset));
  EXPECT_EQ(200, offset);
  slice.commit(offset, 6);
  EXPECT_EQ(2, slice.freeBytes());

  slice.releaseThrough(4);
  EXPECT_EQ(6, slice.freeBytes());

  ASSERT_TRUE(slice.reserve(4, &offset));
  EXPECT_EQ(200, offset);
  slice.commit(offset, 4);
  EXPECT_EQ(0, slice.freeBytes());

  slice.releaseThrough(12);
  EXPECT_EQ(8, slice.freeBytes());
}

TEST(CxlMemPayloadSlice, reserveFailureDoesNotAdvanceCursorOrChangeOffset) {
  CxlMemPayloadSlice slice(CxlMemPayloadSliceConfig{300, 8});

  uint64_t offset = 0;
  ASSERT_TRUE(slice.reserve(8, &offset));
  EXPECT_EQ(300, offset);
  slice.commit(offset, 8);
  EXPECT_EQ(0, slice.freeBytes());

  offset = 12345;
  EXPECT_FALSE(slice.reserve(1, &offset));
  EXPECT_EQ(12345, offset);

  slice.releaseThrough(8);
  ASSERT_TRUE(slice.reserve(8, &offset));
  EXPECT_EQ(300, offset);
}

TEST(CxlMemPayloadSlice, rejectsInvalidConfigurationAndCommit) {
  EXPECT_THROW(
      CxlMemPayloadSlice(CxlMemPayloadSliceConfig{0, 0}), std::invalid_argument);

  CxlMemPayloadSlice slice(CxlMemPayloadSliceConfig{0, 8});
  uint64_t offset = 0;
  ASSERT_TRUE(slice.reserve(4, &offset));
  EXPECT_THROW(slice.commit(offset + 1, 4), std::invalid_argument);
}

} // namespace folly
