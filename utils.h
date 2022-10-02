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

#ifndef MQTTFS_UTILS_H_
#define MQTTFS_UTILS_H_

#include <stdbool.h>
#include <stddef.h>

#define STR_IMPL(op) #op
#define STR(op) STR_IMPL(op)
#define LOG(fmt, ...) \
  LogImpl(__FILE__ ":" STR(__LINE__) " " fmt "\n", ##__VA_ARGS__)
#define LENGTH(op) (sizeof(op) / sizeof *(op))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct Buffer {
  void* data;
  size_t size;
  size_t alloc;
};

void BufferInit(struct Buffer* buffer);
void* BufferReserve(struct Buffer* buffer, size_t size);
bool BufferAssign(struct Buffer* buffer, const void* data, size_t size);
void BufferCleanup(struct Buffer* buffer);

void LogImpl(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
unsigned long long MillisNow(void);

#endif  // MQTTFS_UTILS_H_
