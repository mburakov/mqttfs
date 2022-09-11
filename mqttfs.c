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
#include <linux/fuse.h>
#include <search.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "utils.h"

static int CompareNodes(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static void DestroyNode(void* node) {
  struct MqttfsNode* real_node = node;
  free(real_node->buffer.data);
  tdestroy(real_node->children, DestroyNode);
  free(real_node->name);
  free(real_node);
}

static int WriteFuseStatus(int fuse, uint64_t unique, int status) {
  struct fuse_out_header out_header = {
      .len = sizeof(struct fuse_out_header),
      .error = status,
      .unique = unique,
  };
  if (write(fuse, &out_header, sizeof(out_header)) != sizeof(out_header)) {
    LOG("Failed to write fuse status (%s)", strerror(errno));
    return -1;
  }
  return 0;
}

static int WriteFuseReply(int fuse, uint64_t unique, const void* data,
                          size_t size) {
  struct fuse_out_header out_header = {
      .len = sizeof(struct fuse_out_header) + (uint32_t)size,
      .unique = unique,
  };
  struct iovec reply[] = {
      {.iov_base = &out_header, .iov_len = sizeof(out_header)},
      {.iov_base = (void*)(uintptr_t)data, .iov_len = size},
  };
  if (writev(fuse, reply, LENGTH(reply)) != out_header.len) {
    LOG("Failed to write fuse reply (%s)", strerror(errno));
    return -1;
  }
  return 0;
}

static struct fuse_attr GetNodeAttr(struct MqttfsNode* node) {
  struct fuse_attr attr = {
      .size = node->buffer.size,
      .mode = node->buffer.size ? (S_IFREG | 06444) : (S_IFDIR | 0755),
  };
  return attr;
}

static int AppendDirent(struct MqttfsBuffer* buffer, uint64_t node,
                        uint32_t type, const char* name) {
  size_t namelen = strlen(name);
  size_t size =
      buffer->size + FUSE_DIRENT_ALIGN(sizeof(struct fuse_dirent) + namelen);
  char* data = realloc(buffer->data, size);
  if (!data) {
    LOG("Failed to reallocate buffer (%s)", strerror(errno));
    return -1;
  }
  struct fuse_dirent* dirent = (void*)(data + buffer->size);
  buffer->data = data;
  buffer->size = size;
  dirent->ino = node;
  dirent->off = size;
  dirent->namelen = (uint32_t)namelen;
  dirent->type = type;
  memcpy(dirent->name, name, namelen);
  return 0;
}

#if 0
static struct MqttfsNode* MqttfsNodeCreate(const char* name) {
  struct MqttfsNode* node = calloc(1, sizeof(struct MqttfsNode));
  if (!node) {
    LOG("Failed to allocate node (%s)", strerror(errno));
    return NULL;
  }

  if (name) {
    node->name = strdup(name);
    if (!node->name) {
      LOG("Failed to allocate node name (%s)", strerror(errno));
      goto rollback_node;
    }
  }
  return node;

rollback_node:
  free(node);
  return NULL;
}
#endif

int MqttfsNodeUnknown(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  LOG("[%p]->%s(opcode=%d)", (void*)node, __func__, *(const int*)data);
  return WriteFuseStatus(fuse, unique, -ENOSYS);
}

int MqttfsNodeLookup(struct MqttfsNode* node, uint64_t unique, const void* data,
                     int fuse) {
  LOG("[%p]->%s(%s)", (void*)node, __func__, (const char*)data);
  struct MqttfsNode** pnode = tfind(data, &node->children, CompareNodes);
  if (!pnode) return WriteFuseStatus(fuse, unique, -ENOENT);
  struct fuse_entry_out entry_out = {
      .nodeid = (uint64_t)*pnode,
      .attr = GetNodeAttr(*pnode),
  };
  return WriteFuseReply(fuse, unique, &entry_out, sizeof(entry_out));
}

int MqttfsNodeGetAttr(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  struct fuse_attr_out attr_out = {
      .attr = GetNodeAttr(node),
  };
  return WriteFuseReply(fuse, unique, &attr_out, sizeof(attr_out));
}

int MqttfsNodeInit(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  node->name = NULL;
  node->children = NULL;
  node->buffer.data = NULL;
  node->buffer.size = 0;
  struct fuse_init_out init_out = {
      .major = FUSE_KERNEL_VERSION,
      .minor = FUSE_KERNEL_MINOR_VERSION,
  };
  return WriteFuseReply(fuse, unique, &init_out, sizeof(init_out));
}

int MqttfsNodeOpenDir(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  struct MqttfsBuffer* buffer = calloc(1, sizeof(struct MqttfsBuffer));
  if (!buffer) {
    LOG("Failed to allocate dir handle (%s)", strerror(errno));
    goto report_error;
  }

  if (AppendDirent(buffer, (uint64_t)node, S_IFDIR, ".") == -1 ||
      AppendDirent(buffer, ~0ull, S_IFDIR, "..") == -1) {
    LOG("Failed to append standard dirents");
    goto rollback_handle;
  }

  struct fuse_open_out open_out = {
      .fh = (uint64_t)buffer,
      .open_flags = FOPEN_DIRECT_IO,
  };
  return WriteFuseReply(fuse, unique, &open_out, sizeof(open_out));

rollback_handle:
  free(buffer);
report_error:
  return WriteFuseStatus(fuse, unique, -EIO);
}

int MqttfsNodeReadDir(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  const struct fuse_read_in* read_in = data;
  struct MqttfsBuffer* buffer = (void*)read_in->fh;
  LOG("[%p]->%s(fh=%p, offset=%lu, size=%u)", (void*)node, __func__,
      (void*)buffer, read_in->offset, read_in->size);

  size_t offset = read_in->offset;
  while (offset < buffer->size) {
    struct fuse_dirent* dirent = (void*)((char*)buffer->data + offset);
    size_t next_offset = offset + FUSE_DIRENT_SIZE(dirent);
    if (next_offset - read_in->offset > read_in->size) break;
    offset = next_offset;
  }

  data = (char*)buffer->data + read_in->offset;
  size_t size = offset - read_in->offset;
  return WriteFuseReply(fuse, unique, data, size);
}

int MqttfsNodeReleaseDir(struct MqttfsNode* node, uint64_t unique,
                         const void* data, int fuse) {
  const struct fuse_release_in* release_in = data;
  struct MqttfsBuffer* handle = (void*)release_in->fh;
  LOG("[%p]->%s(fh=%p)", (void*)node, __func__, (void*)handle);
  free(handle->data);
  free(handle);
  return WriteFuseStatus(fuse, unique, 0);
}

void MqttfsNodeCleanup(struct MqttfsNode* node) {
  tdestroy(node->children, DestroyNode);
}
