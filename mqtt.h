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

#ifndef MQTTFS_MQTT_H_
#define MQTTFS_MQTT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils.h"

typedef void (*MqttPublishCallback)(void* user, const char* topic,
                                    size_t topic_size, const void* payload,
                                    size_t payload_size);

struct MqttContext {
  MqttPublishCallback publish_callback;
  void* publish_user;
  struct Buffer buffer;
  bool (*handler)(struct MqttContext*, int);
};

bool MqttContextInit(struct MqttContext* context, uint16_t keepalive, int mqtt,
                     MqttPublishCallback publish_callback, void* publish_user);
bool MqttContextPing(struct MqttContext* context, int mqtt);
bool MqttContextPublish(struct MqttContext* context, int mqtt,
                        const char* topic, size_t topic_size,
                        const void* payload, size_t payload_size);
void MqttContextCleanup(struct MqttContext* context);

#endif  // MQTTFS_MQTT_H_
