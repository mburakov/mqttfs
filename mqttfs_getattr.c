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
#include <sys/stat.h>
#include <threads.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"

int MqttfsGetattr(const char* path, struct stat* stbuf,
                  struct fuse_file_info* fi) {
  const struct Node* node = NULL;
  if (fi) {
    node = (const struct Node*)fi->fh;
    if (node->is_dir) {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
      return 0;
    }
  }

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex)) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  if (!node) {
    char* path_copy = strdup(path);
    if (!path_copy) {
      LOG(ERR, "failed to copy path: %s", strerror(errno));
      result = -EIO;
      goto rollback_mtx_lock;
    }

    node = NodeFind(context->root_node, path_copy);
    free(path_copy);
    if (!node) {
      result = -ENOENT;
      goto rollback_mtx_lock;
    }
  }

  memset(stbuf, 0, sizeof(struct stat));
  if (node->is_dir) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
  } else {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = (off_t)node->as_file.size;
  }
  result = 0;

rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
