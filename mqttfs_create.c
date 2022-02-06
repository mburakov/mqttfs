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
#include <threads.h>

#include "entry.h"
#include "log.h"
#include "mqttfs.h"

int MqttfsCreate(const char* path, mode_t mode, struct fuse_file_info* fi) {
  (void)mode;
  (void)fi;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(WARNING, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;

  // mburakov: FUSE is expected to check the path to the directory, so in case
  // entries creation fails, there would be no leftovers. It is also expected
  // that it performs basic sanity checks, i.e. it won't allow to create a file
  // that already exists or file with the same name as existing directory (i.e.
  // a root directory), etc.
  struct Entry* entry = EntrySearch(&context->entries, path);
  if (!entry) {
    LOG(WARNING, "failed to create entry");
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
