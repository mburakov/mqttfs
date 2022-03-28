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

#include "str.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UNCONST(op) ((char*)(uintptr_t)(op))

struct Str StrView(const char* str) {
  struct Str result = {
      .size = strlen(str),
      .data = str,
  };
  return result;
}

_Bool StrCopy(struct Str* to, const struct Str* from) {
  // mburakov: Allocate additional byte on the tail to store a terminating zero.
  // This allows to pass a string as a plain C string where needed.
  struct Str result = {
      .size = from->size,
      .data = malloc(from->size + 1),
  };
  if (!result.data) return 0;
  memcpy(UNCONST(result.data), from->data, from->size);
  *UNCONST(result.data + result.size) = 0;
  *to = result;
  return 1;
}

int StrCompare(const struct Str* a, const struct Str* b) {
  if (a->size != b->size) {
    return a->size < b->size ? -1 : 1;
  }
  if (a->data == b->data) return 0;
  return memcmp(a->data, b->data, a->size);
}

struct Str StrBasePath(const struct Str* path) {
  struct Str result = {
      .data = path->data,
  };
  const char* separator = memrchr(path->data, '/', path->size);
  if (separator) result.size = (size_t)(separator - path->data);
  return result;
}

const char* StrFileName(const struct Str* path) {
  const char* separator = memrchr(path->data, '/', path->size);
  return separator ? separator + 1 : path->data;
}

void StrFree(const struct Str* op) {
  // mburakov: Only call this on strings created by StrCopy.
  free(UNCONST(op->data));
}
