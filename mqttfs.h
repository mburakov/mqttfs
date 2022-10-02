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

#ifndef MQTTFS_MQTTFS_H_
#define MQTTFS_MQTTFS_H_

#include <stdbool.h>
#include <stdint.h>

#include "utils.h"

struct MqttfsHandle {
  bool updated;
  uint64_t poll_handle;
  struct Buffer* buffer;
  struct MqttfsHandle* prev;
  struct MqttfsHandle* next;
};

struct MqttfsNode {
  char* name;
  void* children;
  bool present_as_dir;
  struct Buffer buffer;
  struct MqttfsHandle* handles;
};

struct MqttfsHandle* MqttfsHandleCreate(struct MqttfsNode* node);
void MqttfsHandleDestroy(struct MqttfsNode* node, struct MqttfsHandle* handle);

struct MqttfsNode* MqttfsNodeCreate(const char* name);
bool MqttfsNodeIsDirectory(const struct MqttfsNode* node);
void MqttfsNodeDestroy(void* node);

#endif  // MQTTFS_MQTTFS_H_
