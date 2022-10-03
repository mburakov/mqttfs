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
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include "fuse.h"
#include "mqtt.h"
#include "utils.h"

struct Context {
  int mqtt_fd;
  int fuse_fd;
  struct MqttContext mqtt;
  struct FuseContext fuse;
};

static volatile sig_atomic_t g_signal;

static void SignalHandler(int signal) { g_signal = signal; }

static bool ParseAddress(const char* arg, struct sockaddr_in* addr) {
  char ip[sizeof("xxx.xxx.xxx.xxx")];
  uint16_t port = 1883;
  if (sscanf(arg, "%[0-9.]:%hd", ip, &port) < 1) return false;
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = inet_addr(ip);
  return true;
}

static bool DoMount(int fuse, const char* mountpoint) {
  char options[256];
  snprintf(options, sizeof(options),
           "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other", fuse);
  return mount("mqttfs", mountpoint, "fuse.mqttfs", MS_NOSUID | MS_NODEV,
               options) == 0;
}

static void OnMqttPublish(void* user, const char* topic, size_t topic_size,
                          const void* payload, size_t payload_size) {
  struct Context* context = user;
  if (!FuseContextWrite(&context->fuse, context->fuse_fd, topic, topic_size,
                        payload, payload_size)) {
    LOG("Failed to write to fuse");
  }
}

static void OnFuseWrite(void* user, const char* pathname, size_t pathname_size,
                        const void* data, size_t data_size) {
  struct Context* context = user;
  if (!MqttContextPublish(&context->mqtt, context->mqtt_fd, pathname,
                          pathname_size, data, data_size)) {
    LOG("Failed to publish to mqtt");
  }
}

int main(int argc, char* argv[]) {
  struct Context context;

  if (argc < 3) {
    LOG("Usage: %s [address[:port]] [mountpoint]", argv[0]);
    return EXIT_FAILURE;
  }
  struct sockaddr_in addr;
  if (!ParseAddress(argv[1], &addr)) {
    LOG("Failed to parse address argument");
    return EXIT_FAILURE;
  }
  context.mqtt_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (context.mqtt_fd == -1) {
    LOG("Failed to create mqtt socket (%s)", strerror(errno));
    return EXIT_FAILURE;
  }

  int status = EXIT_FAILURE;
  if (connect(context.mqtt_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    LOG("Faield to connect mqtt socket (%s)", strerror(errno));
    goto rollback_mqtt_fd;
  }
  context.fuse_fd = open("/dev/fuse", O_RDWR);
  if (context.fuse_fd == -1) {
    LOG("Failed to open fuse device (%s)", strerror(errno));
    goto rollback_mqtt_fd;
  }

  if (!DoMount(context.fuse_fd, argv[2])) {
    LOG("Failed to mount fuse (%s)", strerror(errno));
    goto rollback_fuse_fd;
  }

  if (signal(SIGINT, SignalHandler) == SIG_ERR ||
      signal(SIGTERM, SignalHandler) == SIG_ERR) {
    LOG("Failed to set signal handlers (%s)", strerror(errno));
    goto rollback_mount;
  }

  uint64_t now = MillisNow();
  static const uint16_t kMqttKeepalive = UINT16_MAX;
  if (!MqttContextInit(&context.mqtt, kMqttKeepalive, context.mqtt_fd,
                       OnMqttPublish, &context)) {
    LOG("Failed to init mqtt context");
    goto rollback_mount;
  }

  FuseContextInit(&context.fuse, OnFuseWrite, &context);
  while (!g_signal) {
    struct pollfd pfds[] = {
        {.fd = context.mqtt_fd, .events = POLLIN},
        {.fd = context.fuse_fd, .events = POLLIN},
    };
    uint64_t timeout = now + kMqttKeepalive * 1000ull - MillisNow();
    int result = poll(pfds, LENGTH(pfds), (int)timeout);
    switch (result) {
      case -1:
        if (errno == EINTR) continue;
        LOG("Failed to poll (%s)", strerror(errno));
        goto rollback_context;
      case 0:
        now = MillisNow();
        if (!MqttContextPing(&context.mqtt, context.mqtt_fd)) continue;
        LOG("Failed to ping mqtt broker");
        goto rollback_context;
      default:
        break;
    }
    if (pfds[0].revents &&
        !context.mqtt.handler(&context.mqtt, context.mqtt_fd)) {
      LOG("Failed to handle mqtt io event");
      goto rollback_context;
    }
    if (pfds[1].revents && !FuseContextHandle(&context.fuse, context.fuse_fd)) {
      LOG("Failed to handle fuse io event");
      goto rollback_context;
    }
  }
  status = EXIT_SUCCESS;

rollback_context:
  FuseContextCleanup(&context.fuse);
  MqttContextCleanup(&context.mqtt, context.mqtt_fd);
rollback_mount:
  umount(argv[2]);
rollback_fuse_fd:
  close(context.fuse_fd);
rollback_mqtt_fd:
  close(context.mqtt_fd);
  return status;
}
