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

/*
 * Software stub for ugqm_* — development / CI only. Link the hardware GQM
 * library instead for production; keep the same gqm_common.h contract.
 */

#include <folly/io/async/gqm_common.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct ugqm_queue {
  _Atomic uint64_t head;
  _Atomic uint64_t tail;
  uint64_t capacity;
  uint64_t entries[];
};

uint64_t ugqm_withdata_init(void* gqm, uint32_t length) {
  if (!gqm || length == 0) {
    return (uint64_t)GQM_ERR_INVAL;
  }
  struct ugqm_queue* q = (struct ugqm_queue*)gqm;
  q->capacity = length;
  atomic_store_explicit(&q->head, 0, memory_order_relaxed);
  atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
  memset(q->entries, 0, (size_t)length * sizeof(uint64_t));
  return (uint64_t)GQM_ERR_OK;
}

uint64_t ugqm_deinit(void* gqm) {
  (void)gqm;
  return (uint64_t)GQM_ERR_OK;
}

uint64_t ugqm_push(void* gqm, uint64_t data) {
  struct ugqm_queue* q = (struct ugqm_queue*)gqm;
  uint64_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
  for (;;) {
    uint64_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h - t < q->capacity) {
      break;
    }
  }
  q->entries[h % q->capacity] = data;
  atomic_store_explicit(&q->head, h + 1, memory_order_release);
  return (uint64_t)GQM_ERR_OK;
}

uint64_t ugqm_pop(void* gqm, uint64_t* data) {
  if (!data) {
    return (uint64_t)GQM_ERR_INVAL;
  }
  struct ugqm_queue* q = (struct ugqm_queue*)gqm;
  uint64_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
  uint64_t h = atomic_load_explicit(&q->head, memory_order_acquire);
  if (t >= h) {
    return (uint64_t)GQM_ERR_EMPTY;
  }
  *data = q->entries[t % q->capacity];
  atomic_store_explicit(&q->tail, t + 1, memory_order_release);
  return (uint64_t)GQM_ERR_OK;
}
