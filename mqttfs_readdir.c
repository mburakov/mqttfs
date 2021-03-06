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
#include <search.h>
#include <string.h>
#include <sys/types.h>
#include <threads.h>
#include <time.h>

#include "log.h"
#include "mqttfs.h"
#include "node.h"
#include "str.h"

// mburakov: musl does not implement twalk_r.
static void* g_twalk_closure;

static void OnReaddir(const void* nodep, VISIT which, int depth) {
  (void)depth;

  struct {
    void* buf;
    fuse_fill_dir_t filler;
    const struct Str* parent;
  }* closure = g_twalk_closure;

  if (which == postorder || which == endorder) return;
  struct Node* node = *(void* const*)nodep;
  if (!node->path.size) return;

  struct Str base_path = StrBasePath(&node->path);
  if (!StrCompare(closure->parent, &base_path)) {
    // TODO(mburakov): Handle filler errors!
    closure->filler(closure->buf, StrFileName(&node->path), NULL, 0,
                    (enum fuse_fill_dir_flags)0);
  }
}

int MqttfsReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info* fi,
                  enum fuse_readdir_flags flags) {
  (void)path;
  (void)offset;
  (void)flags;

  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    return -EIO;
  }
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock root mutex: %s", strerror(errno));
    return -EIO;
  }

  struct Node* node = (struct Node*)fi->fh;
  filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
  filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);

  struct {
    void* buf;
    fuse_fill_dir_t filler;
    const struct Str* parent;
  } closure = {
      .buf = buf,
      .filler = filler,
      .parent = &node->path,
  };

  g_twalk_closure = &closure;
  twalk(context->root_node, OnReaddir);
  node->atime = now;
  mtx_unlock(&context->root_mutex);
  return 0;
}
