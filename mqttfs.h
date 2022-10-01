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

#include <stddef.h>
#include <stdint.h>

struct MqttfsBuffer {
  void* data;
  size_t size;
};

struct MqttfsHandle {
  int updated;
  uint64_t poll_handle;
  struct MqttfsBuffer* buffer;
  struct MqttfsHandle* prev;
  struct MqttfsHandle* next;
};

struct MqttfsNode {
  char* name;
  void* children;
  int present_as_dir;
  struct MqttfsBuffer buffer;
  struct MqttfsHandle* handles;
};

int MqttfsNodeUnknown(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse);
int MqttfsNodeLookup(struct MqttfsNode* node, uint64_t unique, const void* data,
                     int fuse);
int MqttfsNodeGetattr(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse);
int MqttfsNodeOpen(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse);
int MqttfsNodeRead(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse);
int MqttfsNodeRelease(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse);
int MqttfsNodeInit(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse);
int MqttfsNodeOpendir(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse);
int MqttfsNodeReaddir(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse);
int MqttfsNodeReleasedir(struct MqttfsNode* node, uint64_t unique,
                         const void* data, int fuse);
int MqttfsNodePoll(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse);
void MqttfsNodeCleanup(struct MqttfsNode* node);

void MqttfsStore(void* root_node, const char* topic, size_t topic_size,
                 const void* payload, size_t payload_size);

#endif  // MQTTFS_MQTTFS_H_
