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

#ifndef MQTTFS_FUSE_H_
#define MQTTFS_FUSE_H_

#include <stdbool.h>
#include <stddef.h>

#include "mqttfs.h"

typedef void (*FuseWriteCallback)(void* user, const char* pathname,
                                  size_t pathname_size, const void* data,
                                  size_t data_size);

struct FuseContext {
  FuseWriteCallback write_callback;
  void* write_user;
  struct MqttfsNode root;
};

void FuseContextInit(struct FuseContext* context,
                     FuseWriteCallback write_callback, void* write_user);
bool FuseContextHandle(struct FuseContext* context, int fuse);
bool FuseContextWrite(struct FuseContext* context, int fuse,
                      const char* pathname, size_t pathname_size,
                      const void* data, size_t data_size);
void FuseContextCleanup(struct FuseContext* context);

#endif  // MQTTFS_FUSE_H_
