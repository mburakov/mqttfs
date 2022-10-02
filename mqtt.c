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

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "utils.h"

enum PublishParseResult {
  kPublishParseResultSuccess = 0,
  kPublishParseResultReadMore,
  kPublishParseResultError,
};

static uint8_t ReadByte(const struct MqttContext* context, size_t* offset,
                        jmp_buf jmpbuf) {
  if (*offset == context->buffer.size)
    longjmp(jmpbuf, kPublishParseResultReadMore);
  uint8_t* buffer_data = context->buffer.data;
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
  if (remaining_length > context->buffer.size)
    return kPublishParseResultReadMore;

  if ((packet_type & 0xf0) == 0x30) {
    const uint8_t* varheader = (uint8_t*)context->buffer.data + offset;
    uint16_t topic_size = (varheader[0] << 8 | varheader[1]) & 0xffff;
    const char* topic = (const void*)(varheader + 2);
    const char* payload = topic + topic_size;
    size_t payload_size = remaining_length - topic_size - sizeof(topic_size);
    context->publish_callback(context->publish_user, topic, topic_size, payload,
                              payload_size);
  }

  size_t leftovers = context->buffer.size - remaining_length - offset;
  void* from = (char*)context->buffer.data + remaining_length + offset;
  memmove(context->buffer.data, from, leftovers);
  context->buffer.size = leftovers;
  return kPublishParseResultSuccess;
}

static bool ReadPublish(struct MqttContext* context, int mqtt) {
  int avail;
  if (ioctl(mqtt, FIONREAD, &avail) == -1) {
    LOG("Failed to get available data size (%s)", strerror(errno));
    return false;
  }
  if (!avail) {
    LOG("Broker closed connection");
    return false;
  }

  size_t size = (size_t)avail;
  void* buffer = BufferReserve(&context->buffer, size);
  if (!buffer) {
    LOG("Failed to reserve buffer (%s)", strerror(errno));
    return false;
  }

  for (;;) {
    ssize_t len = read(mqtt, buffer, size);
    switch (len) {
      case -1:
        if (errno == EINTR) continue;
        LOG("Failed to read mqtt (%s)", strerror(errno));
        __attribute__((fallthrough));
      case 0:
        return false;
      default:
        break;
    }
    context->buffer.size += (size_t)len;
    break;
  }

  for (;;) {
    switch (ParsePublish(context)) {
      case kPublishParseResultSuccess:
        continue;
      case kPublishParseResultReadMore:
        return true;
      case kPublishParseResultError:
        return false;
      default:
        __builtin_unreachable();
    }
  }
}

static bool ReadSubscribeAck(struct MqttContext* context, int mqtt) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t packet_identifier;
    uint8_t return_code;
  } subscribe_ack;
  static_assert(sizeof(subscribe_ack) == 5, "Unexpected subscribe ack size");
  if (read(mqtt, &subscribe_ack, sizeof(subscribe_ack)) !=
      sizeof(subscribe_ack)) {
    LOG("Failed to read mqtt (%s)", strerror(errno));
    return false;
  }
  if (subscribe_ack.packet_type != 0x90 || subscribe_ack.message_length != 3 ||
      subscribe_ack.packet_identifier != htons(1) ||
      subscribe_ack.return_code != 0) {
    LOG("Unexpected subscribe ack from mqtt broker");
    return false;
  }
  context->handler = ReadPublish;
  return true;
}

static bool WriteSubscribe(struct MqttContext* context, int mqtt) {
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
  static_assert(sizeof(subscribe_message) == 10,
                "Unexpected subscribe message size");
  if (write(mqtt, &subscribe_message, sizeof(subscribe_message)) !=
      sizeof(subscribe_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return false;
  }
  context->handler = ReadSubscribeAck;
  return true;
}

static bool ReadConnectAck(struct MqttContext* context, int mqtt) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint8_t connack_flags;
    uint8_t return_code;
  } connect_ack;
  static_assert(sizeof(connect_ack) == 4, "Unexpected connect ack size");
  if (read(mqtt, &connect_ack, sizeof(connect_ack)) != sizeof(connect_ack)) {
    LOG("Failed to read mqtt (%s)", strerror(errno));
    return false;
  }
  if (connect_ack.packet_type != 0x20 || connect_ack.message_length != 2 ||
      connect_ack.connack_flags != 0x00 || connect_ack.return_code != 0) {
    LOG("Unexpected connect ack from mqtt broker");
    return false;
  }
  if (!WriteSubscribe(context, mqtt)) {
    LOG("Failed to subscribe to mqtt broker");
    return false;
  }
  return true;
}

static bool WriteConnect(struct MqttContext* context, uint16_t keepalive,
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
  static_assert(sizeof(connect_message) == 14,
                "Unexpected connect message size");
  if (write(mqtt, &connect_message, sizeof(connect_message)) !=
      sizeof(connect_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return false;
  }
  context->handler = ReadConnectAck;
  return true;
}

static bool WritePing(struct MqttContext* context, int mqtt) {
  (void)context;
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
  } ping_message = {
      .packet_type = 0xd0,
      .message_length = 0,
  };
  static_assert(sizeof(ping_message) == 2, "Unexpected ping message size");
  if (write(mqtt, &ping_message, sizeof(ping_message)) !=
      sizeof(ping_message)) {
    LOG("Failed to write mqtt (%s)", strerror(errno));
    return false;
  }
  return true;
}

bool MqttContextInit(struct MqttContext* context, uint16_t keepalive, int mqtt,
                     MqttPublishCallback publish_callback, void* publish_user) {
  struct MqttContext init = {
      .publish_callback = publish_callback,
      .publish_user = publish_user,
  };
  *context = init;
  if (!WriteConnect(context, keepalive, mqtt)) {
    LOG("Failed to connect to mqtt broker");
    return false;
  }
  return true;
}

bool MqttContextPing(struct MqttContext* context, int mqtt) {
  if (!WritePing(context, mqtt)) {
    LOG("Failed to ping mqtt broker");
    return false;
  }
  return true;
}

bool MqttContextPublish(struct MqttContext* context, int mqtt,
                        const char* topic, size_t topic_size,
                        const void* payload, size_t payload_size) {
  (void)context;
  (void)mqtt;
  (void)topic;
  (void)topic_size;
  (void)payload;
  (void)payload_size;
  // TODO(mburakov): Implement me!
  return true;
}

void MqttContextCleanup(struct MqttContext* context) {
  BufferCleanup(&context->buffer);
}
