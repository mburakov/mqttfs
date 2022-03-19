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

struct fuse_pollhandle;

struct Node {
  const char* const name;
  const _Bool is_dir;
  struct timespec atime;
  struct timespec mtime;
  union {
    struct {
      void* subs;
    } as_dir;
    struct {
      char* topic;
      void* data;
      size_t size;
      _Bool was_updated;
      struct fuse_pollhandle* ph;
    } as_file;
  };
};

typedef void (*NodeCallback)(void* user, const struct Node* node);

struct Node* NodeCreate(const char* name, _Bool is_dir);
struct Node* NodeFind(struct Node* node, char* path);
struct Node* NodeGet(struct Node* node, const char* name);
_Bool NodeTouch(struct Node* node, _Bool atime, _Bool mtime);
_Bool NodeInsert(struct Node* parent, const struct Node* node);
void NodeForEach(struct Node* node, NodeCallback callback, void* user);
void NodeDestroy(struct Node* node);

#endif  // MQTTFS_NODE_H_
