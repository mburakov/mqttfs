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
#include <stdio.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"
#include "str.h"

#ifndef RENAME_NOREPLACE
// mburakov: musl does not define this.
#define RENAME_NOREPLACE (1 << 0)
#endif  // RENAME_NOREPLACE

#ifndef RENAME_EXCHANGE
// mburakov: musl does not define this.
#define RENAME_EXCHANGE (1 << 1)
#endif  // RENAME_EXCHANGE

static int RenameExchange(struct Context* context, struct Node** from_nodep,
                          void** to_nodep) {
  // mburakov: Check below reproduces behavior described in man 2 rename.
  struct Node* from_node = *from_nodep;
  struct Node* to_node = *to_nodep;
  if (from_node->is_dir != to_node->is_dir)
    return from_node->is_dir ? -ENOTDIR : -EISDIR;

  // mburakov: Publish payload with the updated topic.
  if (!from_node->is_dir) {
    if (!MqttPublish(context->mqtt, &to_node->path, from_node->data,
                     from_node->size)) {
      LOG(ERR, "failed to publish topic");
      return -EIO;
    }
    // mburakov: Delivery might not be done yet, try to cancel.
    MqttCancel(context->mqtt, &from_node->path);
  }

  // mburakov: Exchange names first.
  struct Str temp = from_node->path;
  from_node->path = to_node->path;
  to_node->path = temp;

  // mburakov: Exchange nodes afterwards.
  *from_nodep = to_node;
  *to_nodep = from_node;
  return 0;
}

static int RenameNoreplace(struct Context* context, struct Node** from_nodep,
                           void** to_nodep, struct Str* to_view) {
  if (!to_nodep) {
    LOG(ERR, "failed to search node: %s", strerror(errno));
    return -EIO;
  }
  if (*to_nodep != to_view) return -EEXIST;

  // TODO(mburakov): Should recursive creation be allowed?
  // TODO(mburakov): Should parent node type be verified?

  int result;
  struct Str to_copy;
  if (!StrCopy(&to_copy, to_view)) {
    LOG(ERR, "failed to copy string: %s", strerror(errno));
    result = -EIO;
    goto rollback_tsearch;
  }

  // mburakov: Publish payload with the updated topic.
  struct Node* from_node = *from_nodep;
  if (!from_node->is_dir) {
    if (!MqttPublish(context->mqtt, to_view, from_node->data,
                     from_node->size)) {
      LOG(ERR, "failed to publish topic");
      result = -EIO;
      goto rollback_str_copy;
    }
    // mburakov: Delivery might not be done yet, try to cancel.
    MqttCancel(context->mqtt, &from_node->path);
  }

  // mburakov: First, remove the node.
  tdelete(from_node, &context->root_node, NodeCompare);

  // mburakov: Second, update the node path.
  StrFree(&from_node->path);
  from_node->path = to_copy;

  // mburakov: Finally, put the node back.
  *to_nodep = from_node;
  return 0;

rollback_str_copy:
  StrFree(&to_copy);
rollback_tsearch:
  tdelete(to_view, &context->root_node, NodeCompare);
  return result;
}

static int RenameNormal(struct Context* context, struct Node** from_nodep,
                        void** to_nodep, struct Str* to_view) {
  if (!to_nodep) {
    LOG(ERR, "failed to search node: %s", strerror(errno));
    return -EIO;
  }

  if (*to_nodep == to_view)
    return RenameNoreplace(context, from_nodep, to_nodep, to_view);

  int result = RenameExchange(context, from_nodep, to_nodep);
  if (result) return result;

  // TODO(mburakov): This cleans up original remains of target node. But what if
  // it is open or polled? I have no idea how is this supposed to work...

  struct Node* from_node = *from_nodep;
  tdelete(from_node, &context->root_node, NodeCompare);
  MqttCancel(context->mqtt, &from_node->path);
  NodeDestroy(from_node);
  return 0;
}

int MqttfsRename(const char* from, const char* to, unsigned int flags) {
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return -EIO;
  }

  int result;
  struct Str from_view = StrView(from + 1);
  struct Node** from_nodep =
      tfind(&from_view, &context->root_node, NodeCompare);
  if (!from_nodep) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }

  void** to_nodep;
  struct Str to_view = StrView(to + 1);
  switch (flags) {
    case 0:
      to_nodep = tsearch(&to_view, &context->root_node, NodeCompare);
      result = RenameNormal(context, from_nodep, to_nodep, &to_view);
      break;

    case RENAME_EXCHANGE:
      to_nodep = tfind(&to_view, &context->root_node, NodeCompare);
      result = RenameExchange(context, from_nodep, to_nodep);
      break;

    case RENAME_NOREPLACE:
      to_nodep = tsearch(&to_view, &context->root_node, NodeCompare);
      result = RenameNoreplace(context, from_nodep, to_nodep, &to_view);
      break;

    default:
      result = -EINVAL;
      break;
  }

rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
  return result;
}
