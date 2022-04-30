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

#ifndef MQTT_IMPL_H_
#define MQTT_IMPL_H_

#include <stdint.h>

_Bool SendConnectMessage(int fd, uint16_t keepalive);
_Bool ReceiveConnectAck(int fd);
_Bool SendSubscribeMessage(int fd);
_Bool ReceiveSubscribeAck(int fd);
_Bool SendPingMessage(int fd);
_Bool SendDisconnectMessage(int fd);
_Bool SendPublishMessage(int fd, const char* topic, uint16_t topic_size,
                         const void* payload, uint32_t payload_size);

#endif  // MQTT_IMPL_H_
