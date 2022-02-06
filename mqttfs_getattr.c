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

int MqttfsGetattr(const char* path, struct stat* stbuf,
                  struct fuse_file_info* fi) {
  (void)fi;

  int result = -EIO;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return result;
  }

  // mburakov: This might be called for the root as well, but we don't really
  // have one. The first level entries already contains real items. So we add a
  // virtual root here just for the convenience.
  const struct Entry root = {.subs = context->entries};
  const struct Entry* entry =
      strcmp(path, "/") ? EntryFind(&root.subs, path) : &root;
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }

  // mburakov: Client created directory entries are always marked as such, but
  // not the ones received from the broker. The latter are reported as
  // directories when they have any subentries.
  memset(stbuf, 0, sizeof(struct stat));
  if (entry->dir || entry->subs) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
  } else {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = (off_t)entry->size;
  }
  result = 0;

rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return result;
}
