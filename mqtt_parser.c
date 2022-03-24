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

#include "mqtt_parser.h"

#include <setjmp.h>
#include <stdint.h>

static uint8_t ReadByte(const uint8_t** buffer, size_t* size, jmp_buf jmpbuf) {
  if (!*size) longjmp(jmpbuf, kMqttParseStatusReadMore);
  uint8_t result = **buffer;
  (*buffer)++;
  (*size)--;
  return result;
}

static size_t ReadRemainingLength(const uint8_t** buffer, size_t* size,
                                  jmp_buf jmpbuf) {
  size_t result = 0;
  for (size_t counter = 0; counter < 4; counter++) {
    uint8_t byte = ReadByte(buffer, size, jmpbuf);
    result |= (byte & 0x7full) << (7ull * counter);
    if (~byte & 0x80) return result;
  }
  longjmp(jmpbuf, kMqttParseStatusError);
}

enum MqttParseStatus MqttParseMessage(const void** buffer, size_t* size,
                                      const char** topic, size_t* topic_len,
                                      const void** payload,
                                      size_t* payload_len) {
  jmp_buf jmpbuf;
  int result = setjmp(jmpbuf);
  if (result) return (enum MqttParseStatus)result;
  const uint8_t* buffer_copy = *buffer;
  size_t size_copy = *size;

  uint8_t packet_type = ReadByte(&buffer_copy, &size_copy, jmpbuf);
  size_t remaining_length =
      ReadRemainingLength(&buffer_copy, &size_copy, jmpbuf);
  if (remaining_length > size_copy) return kMqttParseStatusReadMore;

  if ((packet_type & 0xf0) != 0x30) {
    *buffer = buffer_copy + remaining_length;
    *size = size_copy - remaining_length;
    return kMqttParseStatusSkipped;
  }

  size_t topic_len_copy = (buffer_copy[0] << 8 | buffer_copy[1]) & 0xffff;
  if (topic_len_copy > remaining_length) return kMqttParseStatusError;

  *buffer = buffer_copy + remaining_length;
  *size = size_copy - remaining_length;
  *topic = (const char*)buffer_copy + sizeof(uint16_t);
  *topic_len = topic_len_copy;
  *payload = *topic + topic_len_copy;
  *payload_len = remaining_length - sizeof(uint16_t) - *topic_len;
  return kMqttParseStatusSuccess;
}
