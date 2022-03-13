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
#include <fuse.h>
#include <string.h>
#include <sys/types.h>
#include <threads.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"

static void OnReaddir(void* user, const struct Node* entry) {
  struct {
    void* buf;
    fuse_fill_dir_t filler;
  }* context = user;
  // TODO(mburakov): Handle filler errors!
  context->filler(context->buf, entry->name, NULL, 0,
                  (enum fuse_fill_dir_flags)0);
}

int MqttfsReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info* fi,
                  enum fuse_readdir_flags flags) {
  (void)path;
  (void)offset;
  (void)flags;

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex)) {
    LOG(ERR, "failed to lock root mutex: %s", strerror(errno));
    return -EIO;
  }

  const struct Node* node = (const struct Node*)fi->fh;
  filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
  filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
  struct {
    void* buf;
    fuse_fill_dir_t filler;
  } user = {buf, filler};
  NodeForEach(node->as_dir.subs, OnReaddir, &user);

  mtx_unlock(&context->root_mutex);
  return 0;
}
