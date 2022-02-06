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

int MqttfsRead(const char* path, char* buf, size_t size, off_t offset,
               struct fuse_file_info* fi) {
  (void)fi;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result;

  // mburakov: FUSE is expected to perform basic sanity checks, i.e. it won't
  // allow to read a directory, including root.
  const struct Entry* entry = EntryFind(&context->entries, path);
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }

  // mburakov: Read shall return a number of bytes.
  size_t read_end = (size_t)offset + size;
  if (read_end > entry->size) read_end = entry->size;
  size_t read_size = read_end - (size_t)offset;
  const char* read_from = (const char*)entry->data + offset;
  memcpy(buf, read_from, read_size);
  result = (int)read_size;

rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return result;
}
