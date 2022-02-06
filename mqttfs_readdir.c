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

struct ReaddirWalkContext {
  void* buf;
  fuse_fill_dir_t filler;
};

static void OnReaddirWalk(void* user, const struct Entry* entry) {
  const struct ReaddirWalkContext* context = user;
  // TODO(mburakov): Handle filler errors!
  context->filler(context->buf, entry->name, NULL, 0,
                  (enum fuse_fill_dir_flags)0);
}

int MqttfsReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info* fi,
                  enum fuse_readdir_flags flags) {
  (void)offset;
  (void)fi;
  (void)flags;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;

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

  // TODO(mburakov): Handle filler errors.
  filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
  filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
  struct ReaddirWalkContext user = {.buf = buf, .filler = filler};
  EntryWalk(entry->subs, OnReaddirWalk, &user);

rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
    LOG(CRIT, "failed to unlock entries mutex");
  }
  return result;
}
