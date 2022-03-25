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
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"

int MqttfsWrite(const char* path, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
  (void)path;
  (void)offset;

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock root mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  void* data = malloc(size);
  if (!data) {
    LOG(ERR, "failed to preserve file contents");
    result = -EIO;
    goto rollback_mtx_lock;
  }

  struct Node* node = (struct Node*)fi->fh;
  if (!MqttPublish(context->mqtt, &node->as_file.topic, buf, size)) {
    LOG(ERR, "failed to publish topic");
    result = -EIO;
    goto rollback_mtx_lock;
  }

  // mburakov: Write shall return a number of bytes.
  memcpy(data, buf, size);
  free(node->as_file.data);
  node->as_file.data = data;
  node->as_file.size = size;
  NodeTouch(node, 0, 1);
  result = (int)size;

rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
