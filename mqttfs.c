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
#include <poll.h>
#include <search.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "utils.h"

static void* g_twalk_closure;

static int CompareNodes(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static struct MqttfsNode* CreateNode(const char* name) {
  struct MqttfsNode* node = calloc(1, sizeof(struct MqttfsNode));
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
  return node;
}

static void DestroyNode(void* node) {
  struct MqttfsNode* real_node = node;
  for (struct MqttfsHandle* it = real_node->handles; it;) {
    struct MqttfsHandle* next = it->next;
    free(it);
    it = next;
  }
  free(real_node->buffer.data);
  tdestroy(real_node->children, DestroyNode);
  free(real_node->name);
  free(real_node);
}

static int IsDirectory(const struct MqttfsNode* node) {
  return node->present_as_dir || node->children;
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

static void CollectDirents(const void* nodep, VISIT which, int depth) {
  (void)depth;
  struct {
    struct MqttfsBuffer* buffer;
    int result;
  }* twalk_closure = g_twalk_closure;

  if (twalk_closure->result == -1 || which == postorder || which == endorder)
    return;

  const struct MqttfsNode* node = *(const void* const*)nodep;
  if (AppendDirent(twalk_closure->buffer, (uint64_t)node,
                   IsDirectory(node) ? S_IFDIR : S_IFREG, node->name) == -1) {
    LOG("Failed to append dirent for %s", node->name);
    twalk_closure->result = -1;
  }
}

static struct MqttfsHandle* CreateHandle(struct MqttfsNode* node) {
  struct MqttfsHandle* handle = calloc(1, sizeof(struct MqttfsHandle));
  if (!handle) {
    LOG("Failed to allocate hande (%s)", strerror(errno));
    return NULL;
  }

  handle->buffer = &node->buffer;
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

static void DestroyHandle(struct MqttfsNode* node,
                          struct MqttfsHandle* handle) {
  if (handle->prev) handle->prev->next = handle->next;
  if (handle->next) handle->next->prev = handle->prev;
  if (node->handles == handle) node->handles = handle->next;
  free(handle);
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

static int WriteFuseNotify(int fuse, int32_t code, const void* data,
                           size_t size) {
  struct fuse_out_header out_header = {
      .len = sizeof(struct fuse_out_header) + (uint32_t)size,
      .error = code,
  };
  struct iovec reply[] = {
      {.iov_base = &out_header, .iov_len = sizeof(out_header)},
      {.iov_base = (void*)(uintptr_t)data, .iov_len = size},
  };
  if (writev(fuse, reply, LENGTH(reply)) != out_header.len) {
    LOG("Failed to write fuse notification (%s)", strerror(errno));
    return -1;
  }
  return 0;
}

static struct fuse_attr GetNodeAttr(struct MqttfsNode* node) {
  struct fuse_attr attr = {
      .ino = (uint64_t)node,
      .size = node->buffer.size,
      .mode = IsDirectory(node) ? (S_IFDIR | 0755) : (S_IFREG | 0644),
  };
  return attr;
}

static struct MqttfsNode* RecurseStore(struct MqttfsNode* node,
                                       const char* topic, size_t topic_size,
                                       const void* payload,
                                       size_t payload_size) {
  if (!topic_size) {
    void* payload_copy = malloc(payload_size);
    if (!payload_copy) {
      LOG("Failed to copy payload");
      return NULL;
    }
    memcpy(payload_copy, payload, payload_size);
    free(node->buffer.data);
    node->buffer.data = payload_copy;
    node->buffer.size = payload_size;
    return node;
  }

  const char* separator = memchr(topic, '/', topic_size);
  size_t name_size = separator ? (size_t)(separator - topic) : topic_size;
  char name_buffer[name_size + 1];
  memcpy(name_buffer, topic, name_size);
  name_buffer[name_size] = 0;

  const char* name = name_buffer;
  void** pchild = tsearch(&name, &node->children, CompareNodes);
  if (!pchild) {
    LOG("Failed to store child node (%s)", strerror(errno));
    return NULL;
  }
  int child_created = *pchild == &name;
  if (child_created) {
    struct MqttfsNode* child = CreateNode(name);
    if (!child) {
      LOG("Failed to create node");
      tdelete(&name, &node->children, CompareNodes);
      return NULL;
    }
    *pchild = child;
  }

  const char* next_topic = separator ? topic + name_size + 1 : NULL;
  size_t next_topic_size = separator ? topic_size - name_size - 1 : 0;
  struct MqttfsNode* result =
      RecurseStore(*pchild, next_topic, next_topic_size, payload, payload_size);
  if (!result && child_created) {
    struct MqttfsNode* child = *pchild;
    tdelete(&name, &node->children, CompareNodes);
    free(child->name);
    free(child);
  }
  return result;
}

int MqttfsNodeUnknown(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  LOG("[%p]->%s(opcode=%d)", (void*)node, __func__, *(const int*)data);
  return WriteFuseStatus(fuse, unique, -ENOSYS);
}

int MqttfsNodeLookup(struct MqttfsNode* node, uint64_t unique, const void* data,
                     int fuse) {
  LOG("[%p]->%s(%s)", (void*)node, __func__, (const char*)data);
  struct MqttfsNode** pnode = tfind(&data, &node->children, CompareNodes);
  if (!pnode) return WriteFuseStatus(fuse, unique, -ENOENT);
  struct fuse_entry_out entry_out = {
      .nodeid = (uint64_t)*pnode,
      .attr = GetNodeAttr(*pnode),
  };
  return WriteFuseReply(fuse, unique, &entry_out, sizeof(entry_out));
}

int MqttfsNodeForget(struct MqttfsNode* node, uint64_t unique, const void* data,
                     int fuse) {
  LOG("[%p]->%s()", (void*)node, __func__);
  (void)unique;
  (void)data;
  (void)fuse;
  // mburakov: This is a noop. Nothing should be touched here, because the node
  // object is already gone, and all the pointers are dangling.
  return 0;
}

int MqttfsNodeGetattr(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  struct fuse_attr_out attr_out = {
      .attr = GetNodeAttr(node),
  };
  return WriteFuseReply(fuse, unique, &attr_out, sizeof(attr_out));
}

int MqttfsNodeMkdir(struct MqttfsNode* node, uint64_t unique, const void* data,
                    int fuse) {
  const struct fuse_mkdir_in* mkdir_in = data;
  const char* name = (const void*)(mkdir_in + 1);
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);
  void** pchild = tsearch(&name, &node->children, CompareNodes);
  if (!pchild) {
    LOG("Failed to store child node (%s)", strerror(errno));
    return WriteFuseStatus(fuse, unique, -ENOMEM);
  }
  if (*pchild != &name) {
    return WriteFuseStatus(fuse, unique, -EEXIST);
  }

  struct MqttfsNode* child = CreateNode(name);
  if (!child) {
    LOG("Failed to create node");
    tdelete(&name, &node->children, CompareNodes);
    return WriteFuseStatus(fuse, unique, -ENOMEM);
  }

  *pchild = child;
  child->present_as_dir = 1;
  struct fuse_entry_out entry_out = {
      .nodeid = (uint64_t)child,
      .attr = GetNodeAttr(child),
  };
  return WriteFuseReply(fuse, unique, &entry_out, sizeof(entry_out));
}

int MqttfsNodeRmdir(struct MqttfsNode* node, uint64_t unique, const void* data,
                    int fuse) {
  const char* name = data;
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);
  void** pchild = tfind(&name, &node->children, CompareNodes);
  if (!pchild) {
    return WriteFuseStatus(fuse, unique, -ENOENT);
  }
  struct MqttfsNode* child = *pchild;
  tdelete(*pchild, &node->children, CompareNodes);
  DestroyNode(child);
  return WriteFuseStatus(fuse, unique, 0);
}

int MqttfsNodeOpen(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  struct MqttfsHandle* handle = CreateHandle(node);
  if (!handle) {
    LOG("Failed to allocate handle (%s)", strerror(errno));
    return WriteFuseStatus(fuse, unique, -ENOMEM);
  }
  struct fuse_open_out open_out = {
      .fh = (uint64_t)handle,
      .open_flags = FOPEN_DIRECT_IO,
  };
  return WriteFuseReply(fuse, unique, &open_out, sizeof(open_out));
}

int MqttfsNodeRead(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse) {
  const struct fuse_read_in* read_in = data;
  const struct MqttfsHandle* handle = (void*)read_in->fh;
  LOG("[%p]->%s(fh=%p, offset=%lu, size=%u)", (void*)node, __func__,
      (const void*)handle, read_in->offset, read_in->size);
  const void* buffer_data = (const char*)handle->buffer->data + read_in->offset;
  size_t buffer_size =
      MIN(read_in->size, handle->buffer->size - read_in->offset);
  return WriteFuseReply(fuse, unique, buffer_data, buffer_size);
}

int MqttfsNodeRelease(struct MqttfsNode* node, uint64_t unique,
                      const void* data, int fuse) {
  const struct fuse_release_in* release_in = data;
  struct MqttfsHandle* handle = (void*)release_in->fh;
  LOG("[%p]->%s(fh=%p)", (void*)node, __func__, (void*)handle);
  DestroyHandle(node, handle);
  return WriteFuseStatus(fuse, unique, 0);
}

int MqttfsNodeInit(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse) {
  (void)data;
  LOG("[%p]->%s()", (void*)node, __func__);
  node->name = NULL;
  node->children = NULL;
  node->buffer.data = NULL;
  node->buffer.size = 0;
  node->handles = NULL;
  struct fuse_init_out init_out = {
      .major = FUSE_KERNEL_VERSION,
      .minor = FUSE_KERNEL_MINOR_VERSION,
  };
  return WriteFuseReply(fuse, unique, &init_out, sizeof(init_out));
}

int MqttfsNodeOpendir(struct MqttfsNode* node, uint64_t unique,
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

  struct {
    struct MqttfsBuffer* buffer;
    int result;
  } twalk_closure = {
      .buffer = buffer,
      .result = 0,
  };

  g_twalk_closure = &twalk_closure;
  twalk(node->children, CollectDirents);
  if (twalk_closure.result == -1) {
    LOG("Failed to collect dirents");
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
  return WriteFuseStatus(fuse, unique, -ENOMEM);
}

int MqttfsNodeReaddir(struct MqttfsNode* node, uint64_t unique,
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

int MqttfsNodeReleasedir(struct MqttfsNode* node, uint64_t unique,
                         const void* data, int fuse) {
  const struct fuse_release_in* release_in = data;
  struct MqttfsBuffer* handle = (void*)release_in->fh;
  LOG("[%p]->%s(fh=%p)", (void*)node, __func__, (void*)handle);
  free(handle->data);
  free(handle);
  return WriteFuseStatus(fuse, unique, 0);
}

int MqttfsNodePoll(struct MqttfsNode* node, uint64_t unique, const void* data,
                   int fuse) {
  const struct fuse_poll_in* poll_in = data;
  struct MqttfsHandle* handle = (void*)poll_in->fh;
  LOG("[%p]->%s(fh=%p, kh=%lu, flags=0x%x, events=0x%x)", (void*)node, __func__,
      (void*)poll_in->fh, poll_in->kh, poll_in->flags, poll_in->events);

  uint32_t revents = poll_in->events & POLLOUT;
  if (poll_in->events & POLLIN) {
    if (poll_in->flags & FUSE_POLL_SCHEDULE_NOTIFY)
      handle->poll_handle = poll_in->kh;
    if (handle->updated) {
      handle->updated = 0;
      revents |= POLLIN;
    }
  }

  struct fuse_poll_out poll_out = {
      .revents = revents,
  };
  return WriteFuseReply(fuse, unique, &poll_out, sizeof(poll_out));
}

void MqttfsNodeCleanup(struct MqttfsNode* node) {
  tdestroy(node->children, DestroyNode);
}

void MqttfsStore(void* publish_user, const char* topic, size_t topic_size,
                 const void* payload, size_t payload_size) {
  LOG("%.*s: %.*s", (int)topic_size, topic, (int)payload_size,
      (const char*)payload);
  struct {
    int fuse;
    struct MqttfsNode* root;
  }* context = publish_user;
  struct MqttfsNode* node =
      RecurseStore(context->root, topic, topic_size, payload, payload_size);
  if (!node) {
    LOG("Failed to handle mqtt publish");
    return;
  }

  for (struct MqttfsHandle* it = node->handles; it; it = it->next) {
    if (!it->poll_handle) continue;
    it->updated = 1;
    LOG("[%p] Notifying poll handle %lu", (void*)node, it->poll_handle);
    struct fuse_notify_poll_wakeup_out notify_poll_wakeup_out = {
        .kh = it->poll_handle,
    };
    WriteFuseNotify(context->fuse, FUSE_NOTIFY_POLL, &notify_poll_wakeup_out,
                    sizeof(notify_poll_wakeup_out));
  }
}
