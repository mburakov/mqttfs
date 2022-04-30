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

#include "mqtt_impl.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef UNCONST
#define UNCONST(op) ((void*)(uintptr_t)(op))
#endif  // UNCONST

#ifndef LENGTH
#define LENGTH(op) (sizeof(op) / sizeof *(op))
#endif  // LENGTH

// TODO(mburakov): Implement more robust sending-receiving.

static size_t EncodeLength(uint32_t length, uint8_t digits[4]) {
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

_Bool SendConnectMessage(int fd, uint16_t keepalive) {
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

_Bool ReceiveConnectAck(int fd) {
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

_Bool SendSubscribeMessage(int fd) {
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

_Bool ReceiveSubscribeAck(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
    uint16_t packet_identifier;
    uint8_t return_code;
  } subscribe_ack;
  _Static_assert(sizeof(subscribe_ack) == 5, "Unexpected subscribe ack size");
  return read(fd, &subscribe_ack, sizeof(subscribe_ack)) ==
             sizeof(subscribe_ack) &&
         subscribe_ack.packet_type == 0x90 &&
         subscribe_ack.message_length == 3 &&
         subscribe_ack.packet_identifier == htons(1) &&
         subscribe_ack.return_code == 0;
}

_Bool SendPingMessage(int fd) {
  struct __attribute__((__packed__)) {
    uint8_t packet_type;
    uint8_t message_length;
  } ping_message = {
      .packet_type = 0xd0,
      .message_length = 0,
  };
  _Static_assert(sizeof(ping_message) == 2, "Unexpected ping message size");
  return write(fd, &ping_message, sizeof(ping_message)) == sizeof(ping_message);
}

_Bool SendDisconnectMessage(int fd) {
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

_Bool SendPublishMessage(int fd, const char* topic, uint16_t topic_size,
                         const void* payload, uint32_t payload_size) {
  static const uint8_t kPacketType = 0x30;
  uint8_t length_digits[4];
  size_t length_digits_count = EncodeLength(
      sizeof(topic_size) + topic_size + payload_size, length_digits);
  if (!length_digits_count) return 0;
  uint16_t topic_size_no = htons(topic_size);
  struct iovec iov[] = {
      {.iov_base = UNCONST(&kPacketType), .iov_len = sizeof(kPacketType)},
      {.iov_base = UNCONST(length_digits), .iov_len = length_digits_count},
      {.iov_base = &topic_size_no, .iov_len = sizeof(topic_size_no)},
      {.iov_base = UNCONST(topic), .iov_len = topic_size},
      {.iov_base = UNCONST(payload), .iov_len = payload_size},
  };
  ssize_t write_length = 0;
  for (size_t idx = 0; idx < LENGTH(iov); idx++)
    write_length += iov[idx].iov_len;
  return writev(fd, iov, LENGTH(iov)) == write_length;
}
