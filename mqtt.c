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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <threads.h>
#include <unistd.h>

#include "log.h"
#include "mqtt_parser.h"
#include "str.h"

#ifndef UNCONST
#define UNCONST(op) ((void*)(ptrdiff_t)(op))
#endif  // UNCONST

#ifndef LENGTH
#define LENGTH(op) (sizeof(op) / sizeof *(op))
#endif  // LENGTH

// TODO(mburakov): Implement more robust sending-receiving.

struct Mqtt {
  int holdback;
  MqttMessageCallback callback;
  void* user;
  int fd;
  int pipe[2];
  thrd_t receive_thread;
};

static _Bool SendConnectMessage(int fd, uint16_t keepalive) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t protocol_name_length;
    char protocol_name[4];
    uint8_t protocol_level;
    uint8_t connect_flags;
    uint16_t keepalive;
    uint16_t client_id_length;
  } connect_message = {
      .packet_type = 0x10,
      .message_length = 12,
      .protocol_name_length = htons(4),
      .protocol_name = {'M', 'Q', 'T', 'T'},
      .protocol_level = 4,
      .connect_flags = 0x02,
      .keepalive = htons(keepalive),
      .client_id_length = 0,
  };
  _Static_assert(sizeof(connect_message) == 14,
                 "Unexpected connect message size");
  return write(fd, &connect_message, sizeof(connect_message)) ==
         sizeof(connect_message);
}

static _Bool ReceiveConnectAck(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint8_t connack_flags;
    uint8_t return_code;
  } connect_ack;
  _Static_assert(sizeof(connect_ack) == 4, "Unexpected connect ack size");
  return read(fd, &connect_ack, sizeof(connect_ack)) == sizeof(connect_ack) &&
         connect_ack.packet_type == 0x20 && connect_ack.message_length == 2 &&
         connect_ack.connack_flags == 0x00 && connect_ack.return_code == 0;
}

static _Bool SendSubscribeMessage(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t packet_identifier;
    uint16_t topic_length;
    char topic[3];
    uint8_t qos;
  } subscribe_message = {
      .packet_type = 0x82,
      .message_length = 8,
      .packet_identifier = htons(1),
      .topic_length = htons(3),
      .topic = {'+', '/', '#'},
      .qos = 0x00,
  };
  _Static_assert(sizeof(subscribe_message) == 10,
                 "Unexpected subscribe message size");
  return write(fd, &subscribe_message, sizeof(subscribe_message)) ==
         sizeof(subscribe_message);
}

static _Bool ReceiveSubscribeAck(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t packet_identifier;
    uint8_t return_code;
  } subscribe_ack;
  _Static_assert(sizeof(subscribe_ack) == 5, "Unexpected subscribe ack length");
  return read(fd, &subscribe_ack, sizeof(subscribe_ack)) ==
             sizeof(subscribe_ack) &&
         subscribe_ack.packet_type == 0x90 &&
         subscribe_ack.message_length == 3 &&
         subscribe_ack.packet_identifier == htons(1) &&
         subscribe_ack.return_code == 0;
}

static _Bool SendDisconnectMessage(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
  } disconnect_message = {
      .packet_type = 0xe0,
      .message_length = 0,
  };
  _Static_assert(sizeof(disconnect_message) == 2,
                 "Unexpected disconnect message size");
  return write(fd, &disconnect_message, sizeof(disconnect_message)) ==
         sizeof(disconnect_message);
}

static size_t EncodeLength(size_t length, uint8_t digits[4]) {
  if (length > 268435455) return 0;
  size_t result = 0;
  for (;;) {
    digits[result] = length & 0x7f;
    length = length >> 7;
    if (length) {
      digits[result] |= 0x80;
      result++;
    } else {
      return result + 1;
    }
  }
}

static _Bool SendPublishMessage(int fd, const uint8_t* length_digits,
                                size_t length_digits_count,
                                const struct Str* topic, const void* payload,
                                size_t payload_size) {
  uint16_t topic_size = htons((uint16_t)topic->size);
  struct iovec iov[] = {
      {.iov_base = "\x30", .iov_len = 1},
      {.iov_base = UNCONST(length_digits), .iov_len = length_digits_count},
      {.iov_base = &topic_size, .iov_len = sizeof(topic_size)},
      {.iov_base = UNCONST(topic->data), .iov_len = topic->size},
      {.iov_base = UNCONST(payload), .iov_len = payload_size},
  };
  ssize_t write_length = 0;
  for (size_t idx = 0; idx < LENGTH(iov); idx++)
    write_length += iov[idx].iov_len;
  return writev(fd, iov, LENGTH(iov)) == write_length;
}

static int ReceiveThread(void* user) {
  struct Mqtt* mqtt = user;
  uint8_t* buffer = NULL;
  size_t buffer_alloc = 0;
  size_t buffer_size = 0;

  for (;;) {
    struct pollfd pfds[] = {
        {.fd = mqtt->fd, .events = POLLIN},
        {.fd = mqtt->pipe[0], .events = POLLIN},
    };
    switch (poll(pfds, LENGTH(pfds), -1)) {
      case -1:
        if (errno != EINTR) LOG(WARNING, "failed to poll: %s", strerror(errno));
        __attribute__((__fallthrough__));
      case 0:
        continue;
      default:
        break;
    }
    if (pfds[1].revents & POLLIN) {
      free(buffer);
      return 0;
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
        free(buffer);
        return 0;
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
          free(buffer);
          return 0;
      }
      memmove(buffer, tail, tail_size);
      buffer_size = tail_size;
      break;
    }
  }
}

struct Mqtt* MqttCreate(const char* host, uint16_t port, uint16_t keepalive,
                        int holdback, MqttMessageCallback callback,
                        void* user) {
  struct Mqtt* result = malloc(sizeof(struct Mqtt));
  if (!result) {
    LOG(ERR, "failed to allocate MQTT client: %s", strerror(errno));
    return NULL;
  }

  result->holdback = holdback;
  result->callback = callback;
  result->user = user;
  result->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (result->fd == -1) {
    LOG(ERR, "failed to create socket: %s", strerror(errno));
    goto rollback_malloc;
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
  if (thrd_create(&result->receive_thread, ReceiveThread, result) !=
      thrd_success) {
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
  uint8_t length_digits[4];
  size_t length_digits_count =
      EncodeLength(sizeof(uint16_t) + topic->size + payload_len, length_digits);
  if (!length_digits_count) {
    LOG(ERR, "invalid payload size");
    return 0;
  }
  if (!SendPublishMessage(mqtt->fd, length_digits, length_digits_count, topic,
                          payload, payload_len)) {
    LOG(ERR, "failed to write complete publish message: %s", strerror(errno));
    return 0;
  }
  return 1;
}

void MqttCancel(struct Mqtt* mqtt, const struct Str* topic) {
  // TODO(mburakov): Implement me!
  (void)mqtt;
  (void)topic;
}

void MqttDestroy(struct Mqtt* mqtt) {
  char wakeup = 0;
  if (write(mqtt->pipe[1], &wakeup, sizeof(wakeup)) != sizeof(wakeup))
    LOG(CRIT, "failed to wake receive thread: %s", strerror(errno));
  thrd_join(mqtt->receive_thread, NULL);
  SendDisconnectMessage(mqtt->fd);
  close(mqtt->pipe[1]);
  close(mqtt->pipe[0]);
  close(mqtt->fd);
  free(mqtt);
}
