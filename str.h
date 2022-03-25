/*
 * Copyright (C) 2022 Mikhail Burakov. This file is part of mqttfs.
 *
 * mqttfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mqttfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mqttfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MQTTFS_STR_H_
#define MQTTFS_STR_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef UNCONST
#define UNCONST(op) ((void*)(uintptr_t)(op))
#endif  // UNCONST

struct Str {
  uint8_t short_size;
  uint16_t long_size;
  const char* data;
};

inline uint16_t StrSize(const struct Str* str) {
  return str->short_size ? str->short_size : str->long_size;
}

inline const char* StrData(const struct Str* str) {
  return str->short_size ? (const char*)&str->long_size : str->data;
}

inline int StrCompare(const struct Str* a, const struct Str* b) {
  uint16_t size_a = StrSize(a);
  uint16_t size_b = StrSize(b);
  int delta = size_a - size_b;
  if (delta) return delta;
  const char* data_a = StrData(a);
  const char* data_b = StrData(b);
  return memcmp(data_a, data_b, size_a);
}

inline struct Str StrView(const char* data, uint16_t size) {
  struct Str result = {
      .long_size = size,
      .data = data,
  };
  return result;
}

inline _Bool StrCopy(struct Str* to, const struct Str* from) {
  uint16_t size = StrSize(from);
  const char* data = StrData(from);
  if (size <= sizeof(struct Str) - offsetof(struct Str, long_size)) {
    to->short_size = (uint8_t)size;
    memcpy(&to->long_size, data, size);
    return 1;
  }
  struct Str result = {
      .long_size = size,
      .data = malloc(size),
  };
  if (!result.data) return 0;
  memcpy(UNCONST(result.data), data, size);
  *to = result;
  return 1;
}

inline void StrFree(const struct Str* op) {
  if (op->short_size) return;
  free(UNCONST(op->data));
}

#endif  // MQTTFS_STR_H_
