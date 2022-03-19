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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"

// TODO(mburakov): It's absolutely unclear how this supposed to works with i.e.
// poll function. Pretty much possible everything here is totally wrong.

#ifndef RENAME_NOREPLACE
// mburakov: musl does not define this.
#define RENAME_NOREPLACE (1 << 0)
#endif  // RENAME_NOREPLACE

#ifndef RENAME_EXCHANGE
// mburakov: musl does not define this.
#define RENAME_EXCHANGE (1 << 1)
#endif  // RENAME_EXCHANGE

#define MAKE_SWAP_FUNCTION(name, type) \
  static void name(type* a, type* b) { \
    type temp = *a;                    \
    *a = *b;                           \
    *b = temp;                         \
  }

#define SWAP(a, b) \
  _Generic((a), void* : SwapPointers, size_t : SwapSizes)(&a, &b)

MAKE_SWAP_FUNCTION(SwapPointers, void*)
MAKE_SWAP_FUNCTION(SwapSizes, size_t)

static int RenameExchange(struct Context* context, struct Node* from_node,
                          struct Node* to_node) {
  if (!to_node) return -ENOENT;

  // mburakov: Check below reproduces behavior described in man 2 rename.
  if (from_node->is_dir != to_node->is_dir)
    return from_node->is_dir ? -ENOTDIR : -EISDIR;

  if (!from_node->is_dir &&
      !MqttPublish(context->mqtt, to_node->as_file.topic,
                   from_node->as_file.data, from_node->as_file.size)) {
    LOG(ERR, "failed to publish topic");
    return -EIO;
  }

  if (from_node->is_dir) {
    SWAP(from_node->as_dir.subs, to_node->as_dir.subs);
  } else {
    // mburakov: Update flags, poll handle and other attributes stay
    // unmodified. It's only file contents that are swapped.
    SWAP(from_node->as_file.data, to_node->as_file.data);
    SWAP(from_node->as_file.size, to_node->as_file.size);
  }
  return 0;
}

static int RenameNoreplace(struct Context* context, struct Node* from_parent,
                           struct Node** from_node, struct Node* to_parent,
                           struct Node** to_node, const char* topic,
                           const char* to_filename) {
  if (*to_node) return -EEXIST;

  if (!(*from_node)->is_dir && (*from_node)->as_file.ph) {
    // TODO(mburakov): It's unclear how is this supposed to work, so just
    // prohibit this instead.
    LOG(ERR, "will not move polled file");
    return -EPERM;
  }

  struct Node* node = NodeCreate(to_filename, (*from_node)->is_dir);
  if (!node) {
    LOG(ERR, "failed to allocate target node");
    return -EIO;
  }

  if (!node->is_dir) {
    node->as_file.topic = strdup(topic);
    if (!node->as_file.topic) {
      LOG(ERR, "failed to copy topic");
      goto rollback_node_create;
    }
  }
  if (!NodeInsert(to_parent, node)) {
    LOG(ERR, "failed to insert node");
    goto rollback_node_create;
  }

  if (RenameExchange(context, *from_node, node) != 0) {
    // mburakov: The only thing that can fail at this point is MQTT publish.
    // Respective return value would be -EIO.
    goto rollback_node_insert;
  }

  NodeRemove(from_parent, *from_node);
  NodeDestroy(*from_node);
  *from_node = NULL;
  *to_node = node;
  return 0;

rollback_node_insert:
  NodeRemove(to_parent, node);
rollback_node_create:
  NodeDestroy(node);
  return -EIO;
}

static int RenameNormal(struct Context* context, struct Node* from_parent,
                        struct Node** from_node, struct Node* to_parent,
                        struct Node** to_node, const char* topic,
                        const char* to_filename) {
  if (!*to_node) {
    return RenameNoreplace(context, from_parent, from_node, to_parent, to_node,
                           topic, to_filename);
  }
  if (!(*from_node)->is_dir && (*from_node)->as_file.ph) {
    // TODO(mburakov): It's unclear how is this supposed to work, so just
    // prohibit this instead.
    LOG(ERR, "will not move polled file");
    return -EPERM;
  }
  int result = RenameExchange(context, *from_node, *to_node);
  if (result != 0) return result;
  NodeRemove(from_parent, *from_node);
  NodeDestroy(*from_node);
  *from_node = NULL;
  return 0;
}

int MqttfsRename(const char* from, const char* to, unsigned int flags) {
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  char* from_copy = strdup(from);
  if (!from_copy) {
    LOG(ERR, "failed to copy from path: %s", strerror(errno));
    result = -EIO;
    goto rollback_mtx_lock;
  }

  // mburakov: FUSE is expected to provide a valid normalized path here.
  char* from_filename = strrchr(from_copy, '/');
  *from_filename++ = 0;

  char* to_copy = strdup(to);
  if (!to_copy) {
    LOG(ERR, "failed to copy to path: %s", strerror(errno));
    result = -EIO;
    goto rollback_strdup_from;
  }

  // mburakov: FUSE is expected to provide a valid normalized path here.
  char* to_filename = strrchr(to_copy, '/');
  *to_filename++ = 0;

  struct Node* from_parent = NodeFind(context->root_node, from_copy);
  struct Node* to_parent = NodeFind(context->root_node, to_copy);
  if (!from_parent || !to_parent) {
    result = -ENOENT;
    goto rollback_strdup_to;
  }

  if (!from_parent->is_dir || !to_parent->is_dir) {
    result = -ENOTDIR;
    goto rollback_strdup_to;
  }

  struct Node* from_node = NodeGet(from_parent, from_filename);
  if (!from_node) {
    result = -ENOENT;
    goto rollback_strdup_to;
  }

  struct Node* to_node = NodeGet(to_parent, to_filename);
  switch (flags) {
    case 0:
      result = RenameNormal(context, from_parent, &from_node, to_parent,
                            &to_node, to + 1, to_filename);
      break;
    case RENAME_EXCHANGE:
      result = RenameExchange(context, from_node, to_node);
      break;
    case RENAME_NOREPLACE:
      result = RenameNoreplace(context, from_parent, &from_node, to_parent,
                               &to_node, to + 1, to_filename);
      break;
    default:
      // mburakov: This should not be reacheable.
      result = -EINVAL;
      goto rollback_strdup_to;
  }
  if (result == 0) {
    NodeTouch(to_node, 0, 1);
    if (from_node) from_node->mtime = to_node->mtime;
    from_parent->mtime = to_node->mtime;
    to_parent->mtime = to_node->mtime;
  }

rollback_strdup_to:
  free(to_copy);
rollback_strdup_from:
  free(from_copy);
rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
