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

#include "utils.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void BufferInit(struct Buffer* buffer) {
  buffer->data = NULL;
  buffer->size = 0;
  buffer->alloc = 0;
}

void* BufferReserve(struct Buffer* buffer, size_t size) {
  size_t alloc = buffer->size + size;
  if (alloc > buffer->alloc) {
    void* data = realloc(buffer->data, alloc);
    if (!data) return NULL;
    buffer->data = data;
    buffer->alloc = alloc;
  }
  uintptr_t base = (uintptr_t)buffer->data;
  return (void*)(base + buffer->size);
}

bool BufferAssign(struct Buffer* buffer, const void* data, size_t size) {
  if (size > buffer->alloc) {
    void* ndata = malloc(size);
    if (!ndata) return false;
    free(buffer->data);
    buffer->data = ndata;
    buffer->alloc = size;
  }
  memcpy(buffer->data, data, size);
  buffer->size = size;
  return true;
}

void BufferCleanup(struct Buffer* buffer) { free(buffer->data); }

void LogImpl(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

unsigned long long MillisNow(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000ull +
         (unsigned long long)ts.tv_nsec / 1000000ull;
}
