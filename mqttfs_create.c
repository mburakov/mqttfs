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

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"

int MqttfsCreate(const char* path, mode_t mode, struct fuse_file_info* fi) {
  (void)mode;

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex)) {
    LOG(ERR, "failed to lock root mutex");
    return -EIO;
  }

  int result;
  char* path_copy = strdup(path);
  if (!path_copy) {
    LOG(ERR, "failed to copy path: %s", strerror(errno));
    result = -EIO;
    goto rollback_mtx_lock;
  }

  // mburakov: FUSE is expected to provide a valid normalized path here.
  char* filename = strrchr(path_copy, '/');
  *filename++ = 0;

  struct Node* parent = NodeFind(context->root_node, path_copy);
  if (!parent) {
    result = -ENOENT;
    goto rollback_strdup;
  }
  if (!parent->is_dir) {
    result = -ENOTDIR;
    goto rollback_strdup;
  }

  struct Node* node = NodeCreate(filename, 0);
  if (!node) {
    LOG(ERR, "failed to create node");
    result = -EIO;
    goto rollback_strdup;
  }
  if (!NodeInsert(parent, node)) {
    LOG(ERR, "failed to insert node");
    NodeDestroy(node);
    result = -EIO;
    goto rollback_strdup;
  }
  fi->fh = (uint64_t)node;
  result = 0;

rollback_strdup:
  free(path_copy);
rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
