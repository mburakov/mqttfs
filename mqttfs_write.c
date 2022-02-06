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

#include "entry.h"
#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"

static void SwapPointers(void** a, void** b) {
  void* c = *a;
  *a = *b;
  *b = c;
}

int MqttfsWrite(const char* path, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
  (void)offset;

  int result = -EIO;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return result;
  }

  void* data = malloc(size);
  if (!data) {
    LOG(ERR, "failed to preserve file contents");
    goto rollback_mtx_lock;
  }

  if (!MqttPublish(context->mqtt, path + 1, buf, size)) {
    LOG(WARNING, "failed to publish topic");
    goto rollback_malloc;
  }

  // mburakov: Write shall return a number of bytes.
  memcpy(data, buf, size);
  struct Entry* entry = (struct Entry*)fi->fh;
  SwapPointers(&entry->data, &data);
  entry->size = size;
  result = (int)size;

rollback_malloc:
  free(data);
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return result;
}
