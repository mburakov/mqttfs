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

#include <arpa/inet.h>
#include <errno.h>
#include <fuse.h>
#include <netinet/in.h>
#include <search.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"
#include "str.h"

static struct Options ParseOptions() {
  struct Options options = {
      .host = "127.0.0.1",
      .port = 1883,
      .keepalive = 60,
      .holdback = 0,
  };
  const char* maybe_host = getenv("MQTT_HOST");
  if (maybe_host) {
    if (inet_addr(maybe_host) == INADDR_NONE) {
      LOG(ERR, "invalid host value provided");
      exit(EINVAL);
    }
    options.host = maybe_host;
  }
  const char* maybe_port = getenv("MQTT_PORT");
  if (maybe_port) {
    int port = atoi(maybe_port);
    if (port <= 0 || UINT16_MAX < port) {
      LOG(ERR, "invalid port value provided");
      exit(EINVAL);
    }
    options.port = (uint16_t)port;
  }
  const char* maybe_keepalive = getenv("MQTT_KEEPALIVE");
  if (maybe_keepalive) {
    int keepalive = atoi(maybe_keepalive);
    if (keepalive <= 0 || UINT16_MAX < keepalive) {
      LOG(ERR, "invalid keepalive value provided");
      exit(EINVAL);
    }
    options.keepalive = (uint16_t)keepalive;
  }
  const char* maybe_holdback = getenv("MQTT_HOLDBACK");
  if (maybe_holdback) {
    int holdback = atoi(maybe_holdback);
    if (holdback < 0) {
      LOG(ERR, "invalid holdback value provided");
      exit(EINVAL);
    }
    options.holdback = holdback;
  }
  return options;
}

static void* CreateRootNode() {
  struct Str path = StrView("");
  struct Node* node = NodeCreate(&path, 1);
  if (!node) {
    LOG(ERR, "failed to create root node");
    return NULL;
  }

  void* root_node = NULL;
  if (!tsearch(node, &root_node, NodeCompare)) {
    LOG(ERR, "failed to search root node: %s", strerror(errno));
    goto rollback_node_create;
  }
  return root_node;

rollback_node_create:
  NodeDestroy(node);
  return NULL;
}

static void OnMqttMessage(void* user, const struct Str* topic,
                          const void* payload, size_t payload_len) {
  // TODO(mburakov): Check that topic is in canonical form.

  struct Context* context = user;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return;
  }

  // mburakov: Either search, or create a node.
  void** nodep = tsearch(topic, &context->root_node, NodeCompare);
  if (!nodep) {
    LOG(ERR, "failed to search node: %s", strerror(errno));
    goto rollback_mtx_lock;
  }

  if (*nodep != topic) {
    // mburakov: Node exists, just update it.
    struct Node* node = *nodep;
    if (node->is_dir) {
      LOG(ERR, "node is a directory");
      goto rollback_mtx_lock;
    }
    if (!NodeUpdate(node, payload, payload_len)) {
      LOG(ERR, "failed to update node");
      goto rollback_mtx_lock;
    }
    mtx_unlock(&context->root_mutex);
    return;
  }

  // mburakov: Node does not exist, and has to be created and updated.
  struct Node* node = NodeCreate(topic, 0);
  if (!node) {
    LOG(ERR, "failed to create node");
    goto rollback_tsearch;
  }
  if (!NodeUpdate(node, payload, payload_len)) {
    LOG(ERR, "failed to update node");
    goto rollback_node_create;
  }
  *nodep = node;

  // mburakov: Some parent directory nodes might also be missing.
  struct Str base_path = StrBasePath(topic);
  for (; base_path.size; base_path = StrBasePath(&base_path)) {
    nodep = tsearch(&base_path, &context->root_node, NodeCompare);
    if (!nodep) {
      LOG(ERR, "failed to search parent node: %s", strerror(errno));
      goto rollback_recurse;
    }

    if (*nodep != &base_path) {
      // mburakov: Reached first existing parent node.
      struct Node* parent = *nodep;
      if (!parent->is_dir) {
        LOG(ERR, "parent node is not a directory");
        goto rollback_recurse;
      }
      // mburakov: The first existing parent node is a directory. Reaching it
      // means that the full base path is available now.
      break;
    }

    // mburakov: Parent node does not exist, and has to be created.
    struct Node* parent = NodeCreate(&base_path, 1);
    if (!parent) {
      LOG(ERR, "failed to create parent node");
      tdelete(&base_path, &context->root_node, NodeCompare);
      goto rollback_recurse;
    }
    *nodep = parent;
  }

  mtx_unlock(&context->root_mutex);
  return;

rollback_recurse:
  for (struct Str rollback_path = StrBasePath(topic);
       rollback_path.size != base_path.size;
       rollback_path = StrBasePath(&rollback_path)) {
    nodep = tfind(&rollback_path, &context->root_node, NodeCompare);
    // mburakov: The node has to be in the tree.
    NodeDestroy(*nodep);
    tdelete(&rollback_path, &context->root_node, NodeCompare);
  }
rollback_node_create:
  NodeDestroy(node);
rollback_tsearch:
  tdelete(topic, &context->root_node, 0);
rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
}

static void* MqttfsInit(struct fuse_conn_info* conn, struct fuse_config* cfg) {
  (void)conn;
  // TODO(mburakov): Implement lazy connecting.
  struct Context* context = fuse_get_context()->private_data;
  context->mqtt = MqttCreate(context->options.host, context->options.port,
                             context->options.keepalive,
                             context->options.holdback, OnMqttMessage, context);
  cfg->direct_io = 1;
  cfg->nullpath_ok = 1;
  return context;
}

static int MqttfsChmod(const char* path, mode_t mode,
                       struct fuse_file_info* fi) {
  // mburakov: This operation is unsupported, but required by NGINX.
  (void)path;
  (void)mode;
  (void)fi;
  return 0;
}

static void NodeDestroyWrapper(void* node) {
  // mburakov: This is needed to avoid casting function types.
  NodeDestroy(node);
}

int main(int argc, char* argv[]) {
  struct Context context = {
      .options = ParseOptions(),
      .root_node = CreateRootNode(),
  };
  if (!context.root_node) {
    LOG(ERR, "failed to create root node");
    exit(EIO);
  }
  if (mtx_init(&context.root_mutex, mtx_plain) != thrd_success) {
    LOG(ERR, "failed to initialize mutex: %s", strerror(errno));
    tdestroy(context.root_node, NodeDestroyWrapper);
    exit(errno);
  }
  static const struct fuse_operations kFuseOperations = {
      .getattr = MqttfsGetattr,
      .mkdir = MqttfsMkdir,
      .unlink = MqttfsUnlink,
      .rmdir = MqttfsUnlink,
      .rename = MqttfsRename,
      .chmod = MqttfsChmod,
      .open = MqttfsOpen,
      .read = MqttfsRead,
      .write = MqttfsWrite,
      .opendir = MqttfsOpendir,
      .readdir = MqttfsReaddir,
      .init = MqttfsInit,
      .create = MqttfsCreate,
      .utimens = MqttfsUtimens,
      .poll = MqttfsPoll,
  };
  int result = fuse_main(argc, argv, &kFuseOperations, &context);
  if (context.mqtt) MqttDestroy(context.mqtt);
  tdestroy(context.root_node, NodeDestroyWrapper);
  mtx_destroy(&context.root_mutex);
  return result;
}
