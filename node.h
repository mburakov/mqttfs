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

#ifndef MQTTFS_NODE_H_
#define MQTTFS_NODE_H_

#include <stddef.h>
#include <time.h>

#include "str.h"

struct fuse_pollhandle;

struct Node {
  struct Str path;
  struct timespec atime;
  struct timespec mtime;
  void* data;
  size_t size;
  _Bool is_dir;
  _Bool was_updated;
  struct fuse_pollhandle* ph;
};

struct Node* NodeCreate(const struct Str* path, _Bool is_dir);
_Bool NodeUpdate(struct Node* node, const void* data, size_t size);
int NodeCompare(const void* a, const void* b);
void NodeDestroy(struct Node* node);

#endif  // MQTTFS_NODE_H_
