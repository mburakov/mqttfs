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

#include "mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "mqtt_impl.h"
#include "mqtt_parser.h"
#include "str.h"

#ifndef LENGTH
#define LENGTH(op) (sizeof(op) / sizeof *(op))
#endif  // LENGTH

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif  // MIN

struct MqttMessage {
  int64_t timestamp;
  struct Str topic;
  void* payload;
  size_t payload_len;
};

struct Mqtt {
  uint16_t keepalive;
  int holdback;
  MqttMessageCallback callback;
  void* user;
  int64_t last_timestamp;
  struct MqttMessage* messages;
  size_t messages_alloc;
  size_t messages_size;
  mtx_t messages_mutex;
  int fd;
  int pipe[2];
  atomic_bool running;
  thrd_t io_thread;
};

static int64_t MillisNow() {
  struct timespec result = {.tv_sec = 0, .tv_nsec = 0};
  clock_gettime(CLOCK_MONOTONIC, &result);
  return result.tv_sec * 1000 + result.tv_nsec / 1000000;
}

static int64_t DrainMessages(struct Mqtt* mqtt, int64_t now) {
  if (mtx_lock(&mqtt->messages_mutex) != thrd_success) {
    LOG(ERR, "failed to lock messages mutex: %s", strerror(errno));
    return -1;
  }
  int64_t result = -1;
  size_t counter = 0;
  for (; counter < mqtt->messages_size &&
         mqtt->messages[counter].timestamp <= now;
       counter++) {
    const struct MqttMessage* iter = mqtt->messages + counter;
    if (!SendPublishMessage(mqtt->fd, iter->topic.data,
                            (uint16_t)iter->topic.size, iter->payload,
                            (uint32_t)iter->payload_len)) {
      LOG(ERR, "failed to write complete publish message: %s", strerror(errno));
      goto rollback_mtx_lock;
    }
  }
  for (struct MqttMessage* iter = mqtt->messages;
       iter < mqtt->messages + counter; iter++) {
    StrFree(&iter->topic);
    free(iter->payload);
  }
  if (counter) {
    if (counter != mqtt->messages_size) {
      memmove(mqtt->messages, mqtt->messages + counter,
              mqtt->messages_size - counter);
    }
    mqtt->last_timestamp = now;
    mqtt->messages_size -= counter;
  }
  result = mqtt->messages_size ? mqtt->messages->timestamp : INT64_MAX;

rollback_mtx_lock:
  mtx_unlock(&mqtt->messages_mutex);
  return result;
}

static int IoThread(void* user) {
  struct Mqtt* mqtt = user;
  uint8_t* buffer = NULL;
  size_t buffer_alloc = 0;
  size_t buffer_size = 0;

  while (atomic_load(&mqtt->running)) {
    int64_t now = MillisNow();
    if (!now) {
      // mburakov: This could only happen if either a) CLOCK_MONOTONIC does not
      // exist on this system, or b) clock value does not fit into timespec. In
      // both cases it does not really make sense to proceed.
      LOG(CRIT, "failed to get monotonic clock: %s", strerror(errno));
      goto leave;
    }

    int64_t next_timestamp = DrainMessages(mqtt, now);
    if (next_timestamp == -1) {
      // mburakov: This could only happen if either a) mutex failed to lock, or
      // b) writing was not fully completed. In both cases it does not really
      // make sense to proceed.
      LOG(CRIT, "failed to drain messages");
      goto leave;
    }

    static const int64_t kPingThreshold = 100;
    int64_t next_ping =
        mqtt->last_timestamp + mqtt->keepalive * 1000 - kPingThreshold;
    if (next_ping <= now) {
      if (!SendPingMessage(mqtt->fd)) {
        // mburakov: Inability to send a ping *will* lead to a server-side
        // disconnect. It does not really make sense to proceed.
        LOG(CRIT, "failed to send complete ping message: %s", strerror(errno));
        goto leave;
      }
      mqtt->last_timestamp = now;
      next_ping = now + mqtt->keepalive * 1000 - kPingThreshold;
    }

    // mburakov: Delay can not be negative or zero, because at this point some
    // message was sent to the server. At the same time delay can not be more
    // than 65535000 (maximum possible keepalive value times 1000), so it will
    // certainly fit into 32-bit int.
    int timeout = (int)(MIN(next_ping, next_timestamp) - now);

    struct pollfd pfds[] = {
        {.fd = mqtt->fd, .events = POLLIN},
        {.fd = mqtt->pipe[0], .events = POLLIN},
    };
    switch (poll(pfds, LENGTH(pfds), timeout)) {
      case -1:
        if (errno != EINTR)
          LOG(WARNING, "failed to complete poll: %s", strerror(errno));
        __attribute__((__fallthrough__));
      case 0:
        continue;
      default:
        break;
    }

    if (pfds[1].revents & POLLIN) {
      char wakeup;
      if (read(mqtt->pipe[0], &wakeup, sizeof(wakeup)) != sizeof(wakeup))
        LOG(WARNING, "failed to read wake token: %s", strerror(errno));
      continue;
    }

    if (~pfds[0].revents & POLLIN) continue;

    int fionread;
    if (ioctl(mqtt->fd, FIONREAD, &fionread) == -1) {
      LOG(WARNING, "failed to ioctl: %s", strerror(errno));
      continue;
    }

    size_t size = (size_t)fionread;
    if (buffer_alloc - buffer_size < size) {
      size_t new_buffer_alloc = buffer_size + size;
      uint8_t* new_buffer = realloc(buffer, new_buffer_alloc);
      if (!new_buffer) {
        LOG(WARNING, "failed to allocate buffer: %s", strerror(errno));
        continue;
      }
      buffer = new_buffer;
      buffer_alloc = new_buffer_alloc;
    }

    ssize_t read_size = read(mqtt->fd, buffer + buffer_size, size);
    switch (read_size) {
      case -1:
        if (errno == EINTR) continue;
        LOG(ERR, "failed to read: %s", strerror(errno));
        __attribute__((__fallthrough__));
      case 0:
        LOG(CRIT, "server closed connection");
        goto leave;
      default:
        buffer_size += (size_t)read_size;
        break;
    }

    const void* tail = buffer;
    size_t tail_size = buffer_size;
    for (;;) {
      struct Str topic_view;
      const void* payload;
      size_t payload_len;
      switch (MqttParseMessage(&tail, &tail_size, &topic_view, &payload,
                               &payload_len)) {
        case kMqttParseStatusSuccess:
          mqtt->callback(mqtt->user, &topic_view, payload, payload_len);
          __attribute__((__fallthrough__));
        case kMqttParseStatusSkipped:
          continue;
        case kMqttParseStatusReadMore:
          break;
        case kMqttParseStatusError:
          LOG(ERR, "failed to parse publish message");
          goto leave;
      }
      memmove(buffer, tail, tail_size);
      buffer_size = tail_size;
      break;
    }
  }

leave:
  atomic_store(&mqtt->running, 0);
  free(buffer);
  return 0;
}

static void WakeIoThread(struct Mqtt* mqtt) {
  char wakeup = 0;
  if (write(mqtt->pipe[1], &wakeup, sizeof(wakeup)) != sizeof(wakeup))
    LOG(WARNING, "failed to write wake token: %s", strerror(errno));
}

struct Mqtt* MqttCreate(const char* host, uint16_t port, uint16_t keepalive,
                        int holdback, MqttMessageCallback callback,
                        void* user) {
  struct Mqtt* result = malloc(sizeof(struct Mqtt));
  if (!result) {
    LOG(ERR, "failed to allocate MQTT client: %s", strerror(errno));
    return NULL;
  }

  result->keepalive = keepalive;
  result->holdback = holdback;
  result->callback = callback;
  result->user = user;

  result->last_timestamp = MillisNow();
  if (!result->last_timestamp) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    goto rollback_malloc;
  }
  result->messages = NULL;
  result->messages_alloc = 0;
  result->messages_size = 0;
  if (mtx_init(&result->messages_mutex, 1) != thrd_success) {
    LOG(ERR, "failed to initialize mutex: %s", strerror(errno));
    goto rollback_malloc;
  }

  result->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (result->fd == -1) {
    LOG(ERR, "failed to create socket: %s", strerror(errno));
    goto rollback_mtx_init;
  }

  if (pipe(result->pipe) == -1) {
    LOG(ERR, "failed to create pipe: %s", strerror(errno));
    goto rollback_socket;
  }

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = inet_addr(host),
  };
  if (connect(result->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    LOG(ERR, "failed to connect socket: %s", strerror(errno));
    goto rollback_pipe;
  }
  if (!SendConnectMessage(result->fd, keepalive)) {
    LOG(ERR, "failed to send complete connect message: %s", strerror(errno));
    goto rollback_pipe;
  }
  if (!ReceiveConnectAck(result->fd)) {
    LOG(ERR, "failed to receive complete connect ack: %s", strerror(errno));
    goto rollback_pipe;
  }

  if (!SendSubscribeMessage(result->fd)) {
    LOG(ERR, "failed to send complete subscribe message: %s", strerror(errno));
    goto rollback_send_connect_message;
  }
  if (!ReceiveSubscribeAck(result->fd)) {
    LOG(ERR, "failed to receive complete subscribe ack: %s", strerror(errno));
    goto rollback_send_connect_message;
  }
  atomic_store(&result->running, 1);
  if (thrd_create(&result->io_thread, &IoThread, result) != thrd_success) {
    LOG(ERR, "failed to create receive thread: %s", strerror(errno));
    goto rollback_send_connect_message;
  }
  return result;

rollback_send_connect_message:
  SendDisconnectMessage(result->fd);
rollback_pipe:
  close(result->pipe[1]);
  close(result->pipe[0]);
rollback_socket:
  close(result->fd);
rollback_mtx_init:
  mtx_destroy(&result->messages_mutex);
rollback_malloc:
  free(result);
  return NULL;
}

_Bool MqttPublish(struct Mqtt* mqtt, const struct Str* topic,
                  const void* payload, size_t payload_len) {
  if (topic->size > UINT16_MAX) {
    LOG(ERR, "invalid topic size");
    return 0;
  }
  if (sizeof(uint16_t) + topic->size + payload_len > 268435455) {
    LOG(ERR, "invalid message length");
    return 0;
  }
  if (!atomic_load(&mqtt->running)) {
    LOG(ERR, "io thread is not running");
    return 0;
  }
  int64_t timestamp = MillisNow();
  if (!timestamp) {
    LOG(ERR, "failed to get monotonic clock: %s", strerror(errno));
    return 0;
  }
  if (mtx_lock(&mqtt->messages_mutex) != thrd_success) {
    LOG(ERR, "failed to lock mutex: %s", strerror(errno));
    return 0;
  }

  if (mqtt->messages_size == mqtt->messages_alloc) {
    size_t messages_alloc = mqtt->messages_alloc + 1;
    struct MqttMessage* messages =
        realloc(mqtt->messages, messages_alloc * sizeof(struct MqttMessage));
    if (!messages) {
      LOG(ERR, "failed to grow messages list: %s", strerror(errno));
      goto rollback_mtx_lock;
    }
    mqtt->messages = messages;
    mqtt->messages_alloc = messages_alloc;
  }
  struct MqttMessage* message = mqtt->messages + mqtt->messages_size;
  message->timestamp = timestamp + mqtt->holdback;
  if (!StrCopy(&message->topic, topic)) {
    LOG(ERR, "failed to copy topic: %s", strerror(errno));
    goto rollback_mtx_lock;
  }

  message->payload = malloc(payload_len);
  if (!message->payload) {
    LOG(ERR, "failed to copy payload: %s", strerror(errno));
    goto rollback_str_copy;
  }
  memcpy(message->payload, payload, payload_len);
  message->payload_len = payload_len;
  mqtt->messages_size++;
  mtx_unlock(&mqtt->messages_mutex);
  WakeIoThread(mqtt);
  return 1;

rollback_str_copy:
  StrFree(&message->topic);
rollback_mtx_lock:
  mtx_unlock(&mqtt->messages_mutex);
  return 0;
}

void MqttCancel(struct Mqtt* mqtt, const struct Str* topic) {
  if (mtx_lock(&mqtt->messages_mutex) != thrd_success) {
    LOG(ERR, "failed to lock mutex: %s", strerror(errno));
    // TODO(mburakov): Cancelling is not supposed to fail (thus returns void).
    // Is it still possible to somehow handle this?
    return;
  }

  for (size_t index = 0; index < mqtt->messages_size;) {
    struct MqttMessage* iter = mqtt->messages + index;
    if (StrCompare(&iter->topic, topic)) {
      index++;
      continue;
    }
    StrFree(&iter->topic);
    free(iter->payload);
    mqtt->messages_size--;
    memmove(iter, iter + 1,
            (mqtt->messages_size - index) * sizeof(struct MqttMessage));
  }

  mtx_unlock(&mqtt->messages_mutex);
}

void MqttDestroy(struct Mqtt* mqtt) {
  atomic_store(&mqtt->running, 0);
  WakeIoThread(mqtt);
  thrd_join(mqtt->io_thread, NULL);
  SendDisconnectMessage(mqtt->fd);
  close(mqtt->pipe[1]);
  close(mqtt->pipe[0]);
  close(mqtt->fd);
  for (struct MqttMessage* iter = mqtt->messages;
       iter < mqtt->messages + mqtt->messages_size; iter++) {
    StrFree(&iter->topic);
    free(iter->payload);
  }
  free(mqtt->messages);
  free(mqtt);
}
