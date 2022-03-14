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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"
#include "node.h"

static struct Options ParseOptions() {
  struct Options options = {
      .host = "127.0.0.1",
      .port = 1883,
      .keepalive = 60,
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
  return options;
}

static void OnMqttMessage(void* user, const char* topic, const void* payload,
                          size_t payload_len) {
  struct Context* context = user;
  if (mtx_lock(&context->root_mutex) != thrd_success) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return;
  }

  char* path_copy = strdup(topic);
  if (!path_copy) {
    LOG(ERR, "failed to copy path: %s", strerror(errno));
    goto rollback_mtx_lock;
  }

  void* payload_copy = malloc(payload_len);
  if (!payload_copy) {
    LOG(ERR, "failed to copy payload: %s", strerror(errno));
    goto rollback_strdup;
  }

  memcpy(payload_copy, payload, payload_len);
  char* token = strtok(path_copy, "/");
  if (!token) {
    LOG(WARNING, "Invalid topic provided");
    free(payload_copy);
    goto rollback_strdup;
  }

  struct Node* node = context->root_node;
  struct Node* next_node = NULL;
  for (; token; token = strtok(NULL, "/")) {
    next_node = NodeGet(node, token);
    if (!next_node) break;
    node = next_node;
  }

  if (!token) {
    // mburakov: We are at the end of path, node points to the exact item.
    if (node->is_dir) {
      LOG(WARNING, "Ignoring write to a directory node");
      free(payload_copy);
      goto rollback_strdup;
    }
    free(node->as_file.data);
    node->as_file.data = payload_copy;
    node->as_file.size = payload_len;
    if (!node->as_file.ph) goto rollback_strdup;

    // mburakov: There's a blocked poll call on this entry.
    node->as_file.was_updated = 1;
    int result = fuse_notify_poll(node->as_file.ph);
    fuse_pollhandle_destroy(node->as_file.ph);
    node->as_file.ph = NULL;
    if (result) {
      // mburakov: Entry is preserved with its data, but poll will not wake.
      LOG(ERR, "failed to wake poll: %s", strerror(result));
    }
    goto rollback_strdup;
  }

  // mburakov: Got to the last available node, but it's not the end of the path.
  if (!node->is_dir) {
    LOG(WARNING, "Ignoring descend into a non-directory node");
    free(payload_copy);
    goto rollback_strdup;
  }

  // mburakov: The next created node would be a detached root node.
  struct Node* parent_node = node;
  struct Node* detached_root_node = NULL;
  while (token) {
    const char* name = token;
    token = strtok(NULL, "/");
    next_node = NodeCreate(name, token != NULL);
    if (!next_node) {
      LOG(ERR, "Failed to create node");
      free(payload_copy);
      if (detached_root_node) NodeDestroy(detached_root_node);
      goto rollback_strdup;
    }
    if (!detached_root_node) {
      detached_root_node = next_node;
      node = next_node;
      continue;
    }
    if (!NodeInsert(node, next_node)) {
      LOG(ERR, "Failed to insert node");
      free(payload_copy);
      NodeDestroy(detached_root_node);
      goto rollback_strdup;
    }
    node = next_node;
  }

  // mburakov: We are at the end of the path.
  next_node->as_file.data = payload_copy;
  next_node->as_file.size = payload_len;
  if (!NodeInsert(parent_node, detached_root_node)) {
    LOG(ERR, "Failed to insert detached root node");
    NodeDestroy(detached_root_node);
    goto rollback_strdup;
  }

rollback_strdup:
  free(path_copy);
rollback_mtx_lock:
  mtx_unlock(&context->root_mutex);
}

static void* MqttfsInit(struct fuse_conn_info* conn, struct fuse_config* cfg) {
  (void)conn;
  // TODO(mburakov): Implement lazy connecting.
  struct Context* context = fuse_get_context()->private_data;
  context->mqtt =
      MqttCreate(context->options.host, context->options.port,
                 context->options.keepalive, OnMqttMessage, context);
  cfg->direct_io = 1;
  // TODO(mburakov): This breaks write call.
  // cfg->nullpath_ok = 1;
  return context;
}

int main(int argc, char* argv[]) {
  struct Context context = {
      .options = ParseOptions(),
      .root_node = NodeCreate("/", 1),
  };
  if (mtx_init(&context.root_mutex, mtx_plain) != thrd_success) {
    LOG(ERR, "failed to initialize mutex: %s", strerror(errno));
    exit(errno);
  }
  static const struct fuse_operations kFuseOperations = {
      .getattr = MqttfsGetattr,
      .mkdir = MqttfsMkdir,
      .open = MqttfsOpen,
      .read = MqttfsRead,
      .write = MqttfsWrite,
      .opendir = MqttfsOpendir,
      .readdir = MqttfsReaddir,
      .init = MqttfsInit,
      .create = MqttfsCreate,
      .poll = MqttfsPoll,
  };
  int result = fuse_main(argc, argv, &kFuseOperations, &context);
  if (context.mqtt) MqttDestroy(context.mqtt);
  mtx_destroy(&context.root_mutex);
  NodeDestroy(context.root_node);
  return result;
}
