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
#include <fcntl.h>
#include <linux/fuse.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include "mqtt.h"
#include "mqttfs.h"
#include "utils.h"

static volatile sig_atomic_t g_signal;

typedef int (*FuseHandler)(struct MqttfsNode*, uint64_t, const void*, int);
static const FuseHandler g_fuse_handlers[] = {
    [FUSE_LOOKUP] = MqttfsNodeLookup,
    [FUSE_GETATTR] = MqttfsNodeGetAttr,
    [FUSE_INIT] = MqttfsNodeInit,
    [FUSE_OPENDIR] = MqttfsNodeOpenDir,
    [FUSE_READDIR] = MqttfsNodeReadDir,
    [FUSE_RELEASEDIR] = MqttfsNodeReleaseDir,
};

static void SignalHandler(int signal) { g_signal = signal; }

static int HandleFuse(int fuse, struct MqttfsNode* root) {
  static char buffer[FUSE_MIN_READ_BUFFER];
  ssize_t size = read(fuse, buffer, sizeof(buffer));
  if (size == -1) {
    LOG("Failed to read fuse (%s)", strerror(errno));
    return -1;
  }
  struct fuse_in_header* in_header = (void*)buffer;
  struct MqttfsNode* node = root;
  if (in_header->nodeid && in_header->nodeid != FUSE_ROOT_ID)
    node = (struct MqttfsNode*)in_header->nodeid;
  FuseHandler handler = MqttfsNodeUnknown;
  void* data = &in_header->opcode;
  if (in_header->opcode < LENGTH(g_fuse_handlers) &&
      g_fuse_handlers[in_header->opcode]) {
    handler = g_fuse_handlers[in_header->opcode];
    data = buffer + sizeof(struct fuse_in_header);
  }
  if (handler(node, in_header->unique, data, fuse) == -1) {
    LOG("Failed to handle fuse request");
    return -1;
  }
  return 0;
}

static void HandlePublish(void* user, const char* topic, size_t topic_size,
                          const void* payload, size_t payload_size) {
  struct MqttfsNode* root = user;
  (void)root;
  LOG("%.*s: %.*s", (int)topic_size, topic, (int)payload_size,
      (const char*)payload);
}

static int ParseAddress(const char* arg, struct sockaddr_in* addr) {
  char ip[sizeof("xxx.xxx.xxx.xxx")];
  uint16_t port = 1883;
  if (sscanf(arg, "%[0-9.]:%hd", ip, &port) < 1) return -1;
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = inet_addr(ip);
  return 0;
}

static int DoMount(int fuse, const char* mountpoint) {
  char options[256];
  snprintf(options, sizeof(options),
           "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other", fuse);
  return mount("mqttfs", mountpoint, "fuse.mqttfs", MS_NOSUID | MS_NODEV,
               options);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    LOG("Usage: %s [address[:port]] [mountpoint]", argv[0]);
    return EXIT_FAILURE;
  }
  struct sockaddr_in addr;
  if (ParseAddress(argv[1], &addr) == -1) {
    LOG("Failed to parse address argument");
    return EXIT_FAILURE;
  }
  int mqtt = socket(AF_INET, SOCK_STREAM, 0);
  if (mqtt == -1) {
    LOG("Failed to create mqtt socket (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  int status = EXIT_FAILURE;
  if (connect(mqtt, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    LOG("Faield to connect mqtt socket (%s)", strerror(errno));
    goto rollback_mqtt;
  }
  int fuse = open("/dev/fuse", O_RDWR);
  if (fuse == -1) {
    LOG("Failed to open fuse device (%s)", strerror(errno));
    goto rollback_mqtt;
  }

  if (DoMount(fuse, argv[2]) == -1) {
    LOG("Failed to mount fuse (%s)", strerror(errno));
    goto rollback_fuse;
  }

  if (signal(SIGINT, SignalHandler) == SIG_ERR ||
      signal(SIGTERM, SignalHandler) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    goto rollback_mount;
  }
  struct MqttfsNode root;
  struct MqttContext context;
  if (MqttContextInit(&context, 65535, mqtt, HandlePublish, &root)) {
    LOG("Failed to init mqtt context");
    goto rollback_mount;
  }

  while (!g_signal) {
    struct pollfd pfds[] = {
        {.fd = fuse, .events = POLLIN},
        {.fd = mqtt, .events = POLLIN},
    };
    int result = poll(pfds, LENGTH(pfds), -1);
    if (result == -1) {
      if (errno == EINTR) continue;
      LOG("Failed to poll (%s)", strerror(errno));
      goto rollback_root;
    }
    if (pfds[0].revents && HandleFuse(fuse, &root) == -1) {
      LOG("Failed to handle fuse io event");
      goto rollback_root;
    }
    if (pfds[1].revents && context.handler(&context, mqtt) == -1) {
      LOG("Failed to handle mqtt io event");
      goto rollback_root;
    }
  }
  status = EXIT_SUCCESS;

rollback_root:
  MqttfsNodeCleanup(&root);
  MqttContextCleanup(&context);
rollback_mount:
  umount(argv[2]);
rollback_fuse:
  close(fuse);
rollback_mqtt:
  close(mqtt);
  return status;
}
