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
#include <time.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"

int MqttfsUtimens(const char* path, const struct timespec tv[2],
                  struct fuse_file_info* fi) {
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  struct Node* node;
  if (fi) {
    node = (struct Node*)fi->fh;
  } else {
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

  struct timespec now = {.tv_sec = 0, .tv_nsec = 0};
  if ((tv[0].tv_nsec == UTIME_NOW || tv[1].tv_nsec == UTIME_NOW) &&
      clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    result = -EIO;
    goto rollback_mtx_lock;
  }

  if (tv[0].tv_nsec != UTIME_OMIT) {
    node->atime = tv[0].tv_nsec == UTIME_NOW ? now : tv[0];
  }
  if (tv[1].tv_nsec != UTIME_OMIT) {
    node->mtime = tv[1].tv_nsec == UTIME_NOW ? now : tv[1];
  }
  result = 0;

rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}