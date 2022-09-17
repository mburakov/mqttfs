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

#include <errno.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "utils.h"

enum PublishParseResult {
  kPublishParseResultSuccess = 0,
  kPublishParseResultReadMore,
  kPublishParseResultError,
};

static void* GetBuffer(struct MqttContext* context, size_t size) {
  size_t buffer_alloc = context->buffer_size + size;
  if (buffer_alloc > context->buffer_alloc) {
    void* buffer_data = realloc(context->buffer_data, buffer_alloc);
    if (!buffer_data) {
      LOG("Failed to reallocate buffer (%s)", strerror(errno));
      return NULL;
    }
    context->buffer_data = buffer_data;
    context->buffer_alloc = buffer_alloc;
  }
  return ((char*)context->buffer_data) + context->buffer_size;
}

static uint8_t ReadByte(const struct MqttContext* context, size_t* offset,
                        jmp_buf jmpbuf) {
  if (*offset == context->buffer_size)
    longjmp(jmpbuf, kPublishParseResultReadMore);
  uint8_t* buffer_data = context->buffer_data;
  return buffer_data[(*offset)++];
}

static size_t ReadVarint(const struct MqttContext* context, size_t* offset,
                         jmp_buf jmpbuf) {
  size_t result = 0;
  for (size_t counter = 0; counter < 4; counter++) {
    uint8_t byte = ReadByte(context, offset, jmpbuf);
    result |= (byte & 0x7full) << (7ull * counter);
    if (~byte & 0x80) return result;
  }
  LOG("Failed to parse varint");
  longjmp(jmpbuf, kPublishParseResultError);
}

static enum PublishParseResult ParsePublish(struct MqttContext* context) {
  jmp_buf jmpbuf;
  int result = setjmp(jmpbuf);
  if (result != 0) {
    return (enum PublishParseResult)result;
  }

  size_t offset = 0;
  uint8_t packet_type = ReadByte(context, &offset, jmpbuf);
  size_t remaining_length = ReadVarint(context, &offset, jmpbuf);
  if (remaining_length > context->buffer_size)
    return kPublishParseResultReadMore;

  if ((packet_type & 0xf0) == 0x30) {
    const uint8_t* varheader = (uint8_t*)context->buffer_data + offset;
    uint16_t topic_size = (varheader[0] << 8 | varheader[1]) & 0xffff;
    const char* topic = (const void*)(varheader + 2);
    const char* payload = topic + topic_size;
    size_t payload_size = remaining_length - topic_size - sizeof(topic_size);
    context->publish_callback(context->publish_user, topic, topic_size, payload,
                              payload_size);
  }

  size_t leftovers = context->buffer_size - remaining_length - offset;
  void* from = (char*)context->buffer_data + remaining_length + offset;
  memmove(context->buffer_data, from, leftovers);
  context->buffer_size = leftovers;
  return kPublishParseResultSuccess;
}

static int ReadPublish(struct MqttContext* context, int mqtt) {
  int avail;
  if (ioctl(mqtt, FIONREAD, &avail) == -1) {
    LOG("Failed to get available data size (%s)", strerror(errno));
    return -1;
  }

  size_t size = (size_t)avail;
  void* buffer = GetBuffer(context, size);
  if (!buffer) return -1;

  for (;;) {
    ssize_t len = read(mqtt, buffer, size);
    switch (len) {
      case -1:
        if (errno == EINTR) continue;
        LOG("Failed to read mqtt (%s)", strerror(errno));
        __attribute__((fallthrough));
      case 0:
        return -1;
      default:
        break;
    }
    context->buffer_size += (size_t)len;
    break;
  }

  for (;;) {
    switch (ParsePublish(context)) {
      case kPublishParseResultSuccess:
        continue;
      case kPublishParseResultReadMore:
        return 0;
      case kPublishParseResultError:
        return -1;
      default:
        __builtin_unreachable();
    }
  }
}

static int ReadSubscribeAck(struct MqttContext* context, int mqtt) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t packet_identifier;
    uint8_t return_code;
  } subscribe_ack;
  _Static_assert(sizeof(subscribe_ack) == 5, "Unexpected subscribe ack size");
  if (read(mqtt, &subscribe_ack, sizeof(subscribe_ack)) !=
      sizeof(subscribe_ack)) {
    LOG("Failed to read mqtt (%s)", strerror(errno));
    return -1;
  }
  if (subscribe_ack.packet_type != 0x90 || subscribe_ack.message_length != 3 ||
      subscribe_ack.packet_identifier != htons(1) ||
      subscribe_ack.return_code != 0) {
    LOG("Unexpected subscribe ack from mqtt broker");
    return -1;
  }
  context->handler = ReadPublish;
  return 0;
}

static int WriteSubscribe(struct MqttContext* context, int mqtt) {
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
  if (write(mqtt, &subscribe_message, sizeof(subscribe_message)) !=
      sizeof(subscribe_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return -1;
  }
  context->handler = ReadSubscribeAck;
  return 0;
}

static int ReadConnectAck(struct MqttContext* context, int mqtt) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint8_t connack_flags;
    uint8_t return_code;
  } connect_ack;
  _Static_assert(sizeof(connect_ack) == 4, "Unexpected connect ack size");
  if (read(mqtt, &connect_ack, sizeof(connect_ack)) != sizeof(connect_ack)) {
    LOG("Failed to read mqtt (%s)", strerror(errno));
    return -1;
  }
  if (connect_ack.packet_type != 0x20 || connect_ack.message_length != 2 ||
      connect_ack.connack_flags != 0x00 || connect_ack.return_code != 0) {
    LOG("Unexpected connect ack from mqtt broker");
    return -1;
  }
  if (WriteSubscribe(context, mqtt) == -1) {
    LOG("Failed to subscribe to mqtt broker");
    return -1;
  }
  return 0;
}

static int WriteConnect(struct MqttContext* context, uint16_t keepalive,
                        int mqtt) {
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
  if (write(mqtt, &connect_message, sizeof(connect_message)) !=
      sizeof(connect_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return -1;
  }
  context->handler = ReadConnectAck;
  return 0;
}

static int WritePing(struct MqttContext* context, int mqtt) {
  (void)context;
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
  } ping_message = {
      .packet_type = 0xd0,
      .message_length = 0,
  };
  _Static_assert(sizeof(ping_message) == 2, "Unexpected ping message size");
  if (write(mqtt, &ping_message, sizeof(ping_message)) !=
      sizeof(ping_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return -1;
  }
  return 0;
}

int MqttContextInit(struct MqttContext* context, uint16_t keepalive, int mqtt,
                    MqttPublishCallback publish_callback, void* publish_user) {
  struct MqttContext init = {
      .publish_callback = publish_callback,
      .publish_user = publish_user,
  };
  *context = init;
  if (WriteConnect(context, keepalive, mqtt) == -1) {
    LOG("Failed to connect to mqtt broker");
    return -1;
  }
  return 0;
}

int MqttContextPing(struct MqttContext* context, int mqtt) {
  if (WritePing(context, mqtt) == -1) {
    LOG("Failed to ping mqtt broker");
    return -1;
  }
  return 0;
}

int MqttContextPublish(struct MqttContext* context, int mqtt) {
  (void)context;
  (void)mqtt;
  // TODO(mburakov): Implement me!
  return 0;
}

void MqttContextCleanup(struct MqttContext* context) {
  free(context->buffer_data);
}
