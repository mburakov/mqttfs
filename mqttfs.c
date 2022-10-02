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

#include "mqttfs.h"

#include <errno.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

struct MqttfsHandle* MqttfsHandleCreate(struct MqttfsNode* node) {
  struct MqttfsHandle* handle = malloc(sizeof(struct MqttfsHandle));
  if (!handle) {
    LOG("Failed to allocate hande (%s)", strerror(errno));
    return NULL;
  }

  handle->updated = false;
  handle->poll_handle = 0;
  handle->buffer = &node->buffer;
  handle->prev = NULL;
  handle->next = NULL;
  if (!node->handles) {
    node->handles = handle;
    return handle;
  }

  struct MqttfsHandle* prev = node->handles;
  while (prev->next) prev = prev->next;
  prev->next = handle;
  handle->prev = prev;
  return handle;
}

void MqttfsHandleDestroy(struct MqttfsNode* node, struct MqttfsHandle* handle) {
  if (handle->prev) handle->prev->next = handle->next;
  if (handle->next) handle->next->prev = handle->prev;
  if (node->handles == handle) node->handles = handle->next;
  free(handle);
}

struct MqttfsNode* MqttfsNodeCreate(const char* name) {
  struct MqttfsNode* node = malloc(sizeof(struct MqttfsNode));
  if (!node) {
    LOG("Failed to allocate node (%s)", strerror(errno));
    return NULL;
  }

  node->name = strdup(name);
  if (!node->name) {
    LOG("Failed to copy node name (%s)", strerror(errno));
    free(node);
    return NULL;
  }

  node->children = NULL;
  node->present_as_dir = false;
  BufferInit(&node->buffer);
  node->handles = NULL;
  return node;
}

bool MqttfsNodeIsDirectory(const struct MqttfsNode* node) {
  return node->present_as_dir || node->children;
}

void MqttfsNodeDestroy(void* node) {
  struct MqttfsNode* real_node = node;
  for (struct MqttfsHandle* it = real_node->handles; it;) {
    struct MqttfsHandle* next = it->next;
    free(it);
    it = next;
  }

  BufferCleanup(&real_node->buffer);
  tdestroy(real_node->children, MqttfsNodeDestroy);
  free(real_node->name);
  free(real_node);
}
