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
  if (!mtx_lock(&context->root_mutex)) {
    LOG(ERR, "failed to lock nodes mutex: %s", strerror(errno));
    return;
  }

  // TODO(mburakov): Create a copy of a filesystem and work on the copy. When
  // all the modifications succeeded, copy it over the old one, and release the
  // latter.

  (void)topic;
  (void)payload;
  (void)payload_len;

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
  cfg->nullpath_ok = 1;
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
  mtx_destroy(&context.root_mutex);
  NodeDestroy(context.root_node);
  return result;
}
