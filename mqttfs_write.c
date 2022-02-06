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

int MqttfsWrite(const char* path, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi) {
  (void)fi;
  if (offset != 0) return -EINVAL;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = (int)size;

  // mburakov: FUSE is expected to perform basic sanity checks, i.e. it won't
  // allow to write a directory including root.
  struct Entry* entry = EntrySearch(&context->entries, path);
  if (!entry) {
    LOG(WARNING, "failed to create entry");
    result = -EIO;
    goto rollback_mtx_lock;
  }

  void* data = malloc(size);
  if (!data) {
    // mburakov: Entry is preserved, but it will be empty.
    LOG(WARNING, "failed to preserve payload");
    result = -EIO;
    goto rollback_mtx_lock;
  }
  memcpy(data, buf, size);
  free(entry->data);
  entry->data = data;
  entry->size = size;

  if (!MqttPublish(context->mqtt, path + 1, data, size)) {
    // mburakov: Entry is preserved, but the broker would not be aware.
    LOG(WARNING, "failed to publish topic");
    result = -EIO;
    goto rollback_mtx_lock;
  }

rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return result;
}
