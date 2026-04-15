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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t ugqm_withdata_init(void* gqm, uint32_t length);
uint64_t ugqm_deinit(void* gqm);
uint64_t ugqm_push(void* gqm, uint64_t data);
uint64_t ugqm_pop(void* gqm, uint64_t* data);

#ifdef __cplusplus
}
#endif

/*
 * Error classification for ugqm_* return values (matches vendor usage):
 *   uint64_t ret = ugqm_pop(q, &data);
 *   if (GQM_RET_ERR(ret) == GQM_ERR_EMPTY) { ... }
 *
 * When building against a vendor SDK, replace this header or prepend its
 * include path so vendor definitions of GQM_RET_ERR / GQM_ERR_* are used.
 */
#ifndef GQM_ERR_OK
#define GQM_ERR_OK 0u
#endif
#ifndef GQM_ERR_EMPTY
#define GQM_ERR_EMPTY 1u
#endif
#ifndef GQM_ERR_INVAL
#define GQM_ERR_INVAL 2u
#endif
#ifndef GQM_RET_ERR
#define GQM_RET_ERR(ret) ((uint32_t)(uint64_t)(ret))
#endif
