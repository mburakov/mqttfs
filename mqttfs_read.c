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
#include <string.h>
#include <threads.h>

#include "entry.h"
#include "log.h"
#include "mqttfs.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

int MqttfsRead(const char* path, char* buf, size_t size, off_t offset,
               struct fuse_file_info* fi) {
  (void)path;

  if (offset) return 0;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }

  // mburakov: Read shall return a number of bytes.
  const struct Entry* entry = (const struct Entry*)fi->fh;
  size = MIN(size, entry->size);
  memcpy(buf, entry->data, size);

  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return (int)size;
}
