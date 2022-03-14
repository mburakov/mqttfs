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

#include "log.h"
#include "mqttfs.h"
#include "node.h"

// TODO(mburakov): I have no clue what goes on here, it's probably all wrong.

int MqttfsPoll(const char* path, struct fuse_file_info* fi,
               struct fuse_pollhandle* ph, unsigned* reventsp) {
  (void)path;

  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock root mutex: %s", strerror(errno));
    return -EIO;
  }

  struct Node* node = (struct Node*)fi->fh;
  if (ph) {
    // mburakov: Replace currently preserved ph with the new one. This
    // reproduces the behavior from the official poll sample.
    if (node->as_file.ph) {
      fuse_pollhandle_destroy(node->as_file.ph);
    }
    node->as_file.ph = ph;
  }

  // mburakov: This assumes entries are always writable.
  *reventsp |= POLLOUT;
  if (node->as_file.was_updated) {
    *reventsp |= POLLIN;
    node->as_file.was_updated = 0;
  }

  mtx_unlock(&context->root_mutex);
  return 0;
}
