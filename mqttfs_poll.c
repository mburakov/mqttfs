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
#include <sys/poll.h>
#include <threads.h>

#include "entry.h"
#include "log.h"
#include "mqttfs.h"

// TODO(mburakov): I have no clue what goes on here, it's probably all wrong.

int MqttfsPoll(const char* path, struct fuse_file_info* fi,
               struct fuse_pollhandle* ph, unsigned* reventsp) {
  (void)path;

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }

  struct Entry* entry = (struct Entry*)fi->fh;
  if (ph) {
    // mburakov: Replace currently preserved ph with the new one. This
    // reproduces the behavior from the official poll sample.
    if (entry->ph) {
      fuse_pollhandle_destroy(entry->ph);
    }
    entry->ph = ph;
  }

  // mburakov: This assumes entries are always writable.
  *reventsp |= POLLOUT;
  if (entry->was_updated) {
    *reventsp |= POLLIN;
    entry->was_updated = 0;
  }

  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return 0;
}
