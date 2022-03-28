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
#include <search.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"
#include "str.h"

int MqttfsOpen(const char* path, struct fuse_file_info* fi) {
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  struct Str path_view = StrView(path + 1);
  struct Node** nodep = tfind(&path_view, &context->root_node, NodeCompare);
  if (!nodep) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }
  struct Node* node = *nodep;
  if (node->is_dir) {
    result = -EISDIR;
    goto rollback_mtx_lock;
  }

  fi->fh = (uint64_t)node;
  mtx_unlock(&context->root_mutex);
  return 0;

rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
