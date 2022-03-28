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

#include "node.h"

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "str.h"

struct Node* NodeCreate(const struct Str* path, _Bool is_dir) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    return NULL;
  }
  struct Node* result = calloc(1, sizeof(struct Node));
  if (!result) {
    LOG(ERR, "failed to allocate node: %s", strerror(errno));
    return NULL;
  }

  if (!StrCopy(&result->path, path)) {
    LOG(ERR, "failed to copy path: %s", strerror(errno));
    goto rollback_calloc;
  }

  result->atime = now;
  result->mtime = now;
  result->is_dir = is_dir;
  return result;

rollback_calloc:
  free(result);
  return NULL;
}

_Bool NodeUpdate(struct Node* node, const void* data, size_t size) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    return 0;
  }
  void* data_copy = malloc(size);
  if (!data_copy) {
    LOG(ERR, "failed to copy data: %s", strerror(errno));
    return 0;
  }

  if (node->ph) {
    // mburakov: There's a blocked poll call on this entry.
    int result = fuse_notify_poll(node->ph);
    if (result) {
      LOG(ERR, "failed to notify poll: %s", strerror(-result));
      goto rollback_malloc;
    }
    fuse_pollhandle_destroy(node->ph);
    node->was_updated = 1;
    node->ph = NULL;
  }

  memcpy(data_copy, data, size);
  free(node->data);
  node->mtime = now;
  node->data = data_copy;
  node->size = size;
  return 1;

rollback_malloc:
  free(data_copy);
  return 0;
}

int NodeCompare(const void* a, const void* b) {
  _Static_assert(!offsetof(struct Node, path),
                 "Something is wrong with your compiler");
  return StrCompare(a, b);
}

void NodeDestroy(struct Node* node) {
  StrFree(&node->path);
  free(node->data);
  free(node);
}
