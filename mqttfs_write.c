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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <threads.h>
#include <time.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"

int MqttfsWrite(const char* path, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
  (void)path;
  (void)offset;

  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    return -EIO;
  }
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock root mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  void* data = malloc(size);
  if (!data) {
    LOG(ERR, "failed to allocate file contents");
    result = -EIO;
    goto rollback_mtx_lock;
  }

  struct Node* node = (struct Node*)fi->fh;
  if (!MqttPublish(context->mqtt, &node->path, buf, size)) {
    LOG(ERR, "failed to publish topic");
    result = -EIO;
    goto rollback_malloc;
  }

  // mburakov: Write shall return a number of bytes.
  memcpy(data, buf, size);
  free(node->data);
  node->mtime = now;
  node->data = data;
  node->size = size;
  mtx_unlock(&context->root_mutex);
  return (int)size;

rollback_malloc:
  free(data);
rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
