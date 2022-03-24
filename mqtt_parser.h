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

#ifndef MQTTFS_MQTT_PARSER_H_
#define MQTTFS_MQTT_PARSER_H_

#include <stddef.h>

enum MqttParseStatus {
  kMqttParseStatusSuccess = 0,
  kMqttParseStatusSkipped,
  kMqttParseStatusReadMore,
  kMqttParseStatusError
};

enum MqttParseStatus MqttParseMessage(const void** buffer, size_t* size,
                                      const char** topic, size_t* topic_len,
                                      const void** payload,
                                      size_t* payload_len);

#endif  // MQTTFS_MQTT_PARSER_H_
