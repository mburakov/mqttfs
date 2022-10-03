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

#include "fuse.h"

#include <errno.h>
#include <linux/fuse.h>
#include <poll.h>
#include <search.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "mqttfs.h"
#include "utils.h"

struct FuseRequest {
  struct fuse_in_header header;
  union {
    struct {
      char name;
    } lookup, unlink;
    struct {
      struct fuse_mkdir_in header;
      char name;
    } mkdir;
    struct {
      struct fuse_read_in header;
    } read, readdir;
    struct {
      struct fuse_release_in header;
    } release, releasedir;
    struct {
      struct fuse_create_in header;
      char name;
    } create;
    struct {
      struct fuse_poll_in header;
    } poll;
  };
};

static void* g_twalk_closure;

static int CompareNodes(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static bool AppendDirent(struct Buffer* buffer, uint64_t node, uint32_t type,
                         const char* name) {
  size_t namelen = strlen(name);
  size_t size = FUSE_DIRENT_ALIGN(sizeof(struct fuse_dirent) + namelen);
  struct fuse_dirent* dirent = BufferReserve(buffer, size);
  if (!dirent) {
    LOG("Failed to reserve dirent (%s)", strerror(errno));
    return false;
  }

  buffer->size += size;
  dirent->ino = node;
  dirent->off = buffer->size;
  dirent->namelen = (uint32_t)namelen;
  dirent->type = type;
  memcpy(dirent->name, name, namelen);
  return true;
}

static void CollectDirents(const void* nodep, VISIT which, int depth) {
  (void)depth;
  struct {
    struct Buffer* buffer;
    bool result;
  }* twalk_closure = g_twalk_closure;
  if (!twalk_closure->result || which == postorder || which == endorder) return;

  const struct MqttfsNode* node = *(const void* const*)nodep;
  if (!AppendDirent(twalk_closure->buffer, (uint64_t)node,
                    MqttfsNodeIsDirectory(node) ? S_IFDIR : S_IFREG,
                    node->name)) {
    LOG("Failed to append dirent for %s", node->name);
    twalk_closure->result = false;
  }
}

static bool WriteFuseStatus(int fuse, uint64_t unique, int status) {
  struct fuse_out_header out_header = {
      .len = sizeof(struct fuse_out_header),
      .error = status,
      .unique = unique,
  };
  if (write(fuse, &out_header, sizeof(out_header)) != sizeof(out_header)) {
    LOG("Failed to write fuse status (%s)", strerror(errno));
    return false;
  }
  return true;
}

static bool WriteFuseReply(int fuse, uint64_t unique, const void* data,
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
    return false;
  }
  return true;
}

static bool WriteFuseNotify(int fuse, int32_t code, const void* data,
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
    return false;
  }
  return true;
}

static struct fuse_attr GetNodeAttr(const struct MqttfsNode* node) {
  struct fuse_attr attr = {
      .ino = (uint64_t)node,
      .size = node->buffer.size,
      .mode = MqttfsNodeIsDirectory(node) ? (S_IFDIR | 0755) : (S_IFREG | 0644),
  };
  return attr;
}

static struct MqttfsNode* RecurseStore(struct MqttfsNode* node,
                                       const char* pathname,
                                       size_t pathname_size, const void* data,
                                       size_t data_size) {
  if (!pathname_size) {
    bool result = BufferAssign(&node->buffer, data, data_size);
    if (!result) LOG("Failed to copy payload");
    return result ? node : NULL;
  }

  const char* separator = memchr(pathname, '/', pathname_size);
  size_t name_size = separator ? (size_t)(separator - pathname) : pathname_size;
  char name_buffer[name_size + 1];
  memcpy(name_buffer, pathname, name_size);
  name_buffer[name_size] = 0;

  const char* name = name_buffer;
  void** pchild = tsearch(&name, &node->children, CompareNodes);
  if (!pchild) {
    LOG("Failed to store child node (%s)", strerror(errno));
    return NULL;
  }
  int child_created = *pchild == &name;
  if (child_created) {
    struct MqttfsNode* child = MqttfsNodeCreate(name);
    if (!child) {
      LOG("Failed to create node");
      tdelete(&name, &node->children, CompareNodes);
      return NULL;
    }
    *pchild = child;
  }

  const char* next_topic = separator ? pathname + name_size + 1 : NULL;
  size_t next_topic_size = separator ? pathname_size - name_size - 1 : 0;
  struct MqttfsNode* result =
      RecurseStore(*pchild, next_topic, next_topic_size, data, data_size);
  if (!result && child_created) {
    struct MqttfsNode* child = *pchild;
    tdelete(&name, &node->children, CompareNodes);
    MqttfsNodeDestroy(child);
  }
  return result;
}

static bool CreateChildNode(struct MqttfsNode* node, uint64_t unique,
                            const char* name, bool present_as_dir, int fuse) {
  void** pchild = tsearch(&name, &node->children, CompareNodes);
  if (!pchild) {
    LOG("Failed to store child node (%s)", strerror(errno));
    goto report_error;
  }
  if (*pchild != &name) {
    return WriteFuseStatus(fuse, unique, -EEXIST);
  }

  struct MqttfsNode* child = MqttfsNodeCreate(name);
  if (!child) {
    LOG("Failed to create node");
    goto rollback_pchild;
  }

  *pchild = child;
  child->present_as_dir = present_as_dir;
  struct fuse_entry_out entry_out = {
      .nodeid = (uint64_t)child,
      .attr = GetNodeAttr(child),
  };
  if (present_as_dir)
    return WriteFuseReply(fuse, unique, &entry_out, sizeof(entry_out));

  struct MqttfsHandle* handle = MqttfsHandleCreate(child);
  if (!handle) {
    LOG("Failed to allocate handle (%s)", strerror(errno));
    goto rollback_child;
  }
  struct {
    struct fuse_entry_out entry_out;
    struct fuse_open_out open_out;
  } reply = {
      .entry_out = entry_out,
      .open_out.fh = (uint64_t)handle,
      .open_out.open_flags = FOPEN_DIRECT_IO,
  };
  return WriteFuseReply(fuse, unique, &reply, sizeof(reply));

rollback_child:
  MqttfsNodeDestroy(child);
rollback_pchild:
  tdelete(&name, &node->children, CompareNodes);
report_error:
  return WriteFuseStatus(fuse, unique, -ENOMEM);
}

static bool OnFuseUnknown(const struct FuseRequest* request,
                          struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s(opcode=%u)", (void*)node, __func__, request->header.opcode);
  return WriteFuseStatus(fuse, request->header.unique, -ENOSYS);
}

static bool OnFuseLookup(const struct FuseRequest* request,
                         struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  const char* name = &request->lookup.name;
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);

  uint64_t unique = request->header.unique;
  void** pchild = tfind(&name, &node->children, CompareNodes);
  if (!pchild) return WriteFuseStatus(fuse, unique, -ENOENT);
  struct fuse_entry_out entry_out = {
      .nodeid = (uint64_t)*pchild,
      .attr = GetNodeAttr(*pchild),
  };
  return WriteFuseReply(fuse, unique, &entry_out, sizeof(entry_out));
}

static bool OnFuseForget(const struct FuseRequest* request,
                         struct FuseContext* context, int fuse) {
  (void)context;
  (void)fuse;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s()", (void*)node, __func__);
  // mburakov: This is a noop. Nothing should be touched here, because the node
  // object is already gone, and all the pointers are dangling.
  return 0;
}

static bool OnFuseGetattr(const struct FuseRequest* request,
                          struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s()", (void*)node, __func__);

  uint64_t unique = request->header.unique;
  struct fuse_attr_out attr_out = {
      .attr = GetNodeAttr(node),
  };
  return WriteFuseReply(fuse, unique, &attr_out, sizeof(attr_out));
}

static bool OnFuseMkdir(const struct FuseRequest* request,
                        struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  const char* name = &request->mkdir.name;
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);
  uint64_t unique = request->header.unique;
  return CreateChildNode(node, unique, name, true, fuse);
}

static bool OnFuseUnlink(const struct FuseRequest* request,
                         struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  const char* name = &request->unlink.name;
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);

  uint64_t unique = request->header.unique;
  void** pchild = tfind(&name, &node->children, CompareNodes);
  if (!pchild) {
    return WriteFuseStatus(fuse, unique, -ENOENT);
  }

  struct MqttfsNode* child = *pchild;
  tdelete(*pchild, &node->children, CompareNodes);
  MqttfsNodeDestroy(child);
  return WriteFuseStatus(fuse, unique, 0);
}

static bool OnFuseOpen(const struct FuseRequest* request,
                       struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s()", (void*)node, __func__);

  uint64_t unique = request->header.unique;
  struct MqttfsHandle* handle = MqttfsHandleCreate(node);
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

static bool OnFuseRead(const struct FuseRequest* request,
                       struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  struct MqttfsHandle* handle = (void*)request->read.header.fh;
  uint64_t offset = request->read.header.offset;
  uint32_t size = request->read.header.size;
  LOG("[%p]->%s(fh=%p, offset=%lu, size=%u)", (void*)node, __func__,
      (void*)handle, offset, size);

  uint64_t unique = request->header.unique;
  uintptr_t base = (uintptr_t)handle->buffer->data;
  const void* buffer_data = (void*)(base + offset);
  size_t buffer_size = MIN(size, handle->buffer->size - offset);
  return WriteFuseReply(fuse, unique, buffer_data, buffer_size);
}

static bool OnFuseRelease(const struct FuseRequest* request,
                          struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  struct MqttfsHandle* handle = (void*)request->read.header.fh;
  LOG("[%p]->%s(fh=%p)", (void*)node, __func__, (void*)handle);

  uint64_t unique = request->header.unique;
  MqttfsHandleDestroy(node, handle);
  return WriteFuseStatus(fuse, unique, 0);
}

static bool OnFuseInit(const struct FuseRequest* request,
                       struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s()", (void*)node, __func__);

  uint64_t unique = request->header.unique;
  struct fuse_init_out init_out = {
      .major = FUSE_KERNEL_VERSION,
      .minor = FUSE_KERNEL_MINOR_VERSION,
  };
  return WriteFuseReply(fuse, unique, &init_out, sizeof(init_out));
}

static bool OnFuseOpendir(const struct FuseRequest* request,
                          struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  LOG("[%p]->%s()", (void*)node, __func__);

  uint64_t unique = request->header.unique;
  struct Buffer* buffer = malloc(sizeof(struct Buffer));
  if (!buffer) {
    LOG("Failed to allocate dir handle (%s)", strerror(errno));
    goto report_error;
  }

  BufferInit(buffer);
  if (!AppendDirent(buffer, (uint64_t)node, S_IFDIR, ".") ||
      !AppendDirent(buffer, ~0ull, S_IFDIR, "..")) {
    LOG("Failed to append standard dirents");
    goto rollback_handle;
  }

  struct {
    struct Buffer* buffer;
    bool result;
  } twalk_closure = {
      .buffer = buffer,
      .result = true,
  };

  g_twalk_closure = &twalk_closure;
  twalk(node->children, CollectDirents);
  if (!twalk_closure.result) {
    LOG("Failed to collect dirents");
    goto rollback_handle;
  }

  struct fuse_open_out open_out = {
      .fh = (uint64_t)buffer,
      .open_flags = FOPEN_DIRECT_IO,
  };
  return WriteFuseReply(fuse, unique, &open_out, sizeof(open_out));

rollback_handle:
  BufferCleanup(buffer);
  free(buffer);
report_error:
  return WriteFuseStatus(fuse, unique, -ENOMEM);
}

static bool OnFuseReaddir(const struct FuseRequest* request,
                          struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  struct Buffer* buffer = (void*)request->readdir.header.fh;
  uint64_t offset = request->readdir.header.offset;
  uint64_t size = request->readdir.header.size;
  LOG("[%p]->%s(fh=%p, offset=%lu, size=%lu)", (void*)node, __func__,
      (void*)buffer, offset, size);

  while (offset < buffer->size) {
    uintptr_t base = (uintptr_t)buffer->data;
    struct fuse_dirent* dirent = (void*)(base + offset);
    size_t next_offset = offset + FUSE_DIRENT_SIZE(dirent);
    if (next_offset - offset > size) break;
    offset = next_offset;
  }

  uint64_t unique = request->header.unique;
  uintptr_t base = (uintptr_t)buffer->data;
  void* data = (void*)(base + request->readdir.header.offset);
  size = offset - request->readdir.header.offset;
  return WriteFuseReply(fuse, unique, data, size);
}

static bool OnFuseReleasedir(const struct FuseRequest* request,
                             struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  struct Buffer* buffer = (void*)request->releasedir.header.fh;
  LOG("[%p]->%s(fh=%p)", (void*)node, __func__, (void*)buffer);
  BufferCleanup(buffer);
  free(buffer);
  uint64_t unique = request->header.unique;
  return WriteFuseStatus(fuse, unique, 0);
}

static bool OnFuseCreate(const struct FuseRequest* request,
                         struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  const char* name = &request->create.name;
  LOG("[%p]->%s(%s)", (void*)node, __func__, name);
  uint64_t unique = request->header.unique;
  return CreateChildNode(node, unique, name, 0, fuse);
}

static bool OnFusePoll(const struct FuseRequest* request,
                       struct FuseContext* context, int fuse) {
  (void)context;
  struct MqttfsNode* node = (void*)request->header.nodeid;
  struct MqttfsHandle* handle = (void*)request->poll.header.fh;
  uint64_t poll_handle = request->poll.header.kh;
  uint32_t flags = request->poll.header.flags;
  uint32_t events = request->poll.header.events;
  LOG("[%p]->%s(fh=%p, kh=%lu, flags=0x%x, events=0x%x)", (void*)node, __func__,
      (void*)handle, poll_handle, flags, events);

  uint32_t revents = events & POLLOUT;
  if (events & POLLIN) {
    if (flags & FUSE_POLL_SCHEDULE_NOTIFY) {
      handle->poll_handle = poll_handle;
    }
    if (handle->updated) {
      handle->updated = false;
      revents |= POLLIN;
    }
  }

  uint64_t unique = request->header.unique;
  struct fuse_poll_out poll_out = {
      .revents = revents,
  };
  return WriteFuseReply(fuse, unique, &poll_out, sizeof(poll_out));
}

void FuseContextInit(struct FuseContext* context,
                     FuseWriteCallback write_callback, void* write_user) {
  context->write_callback = write_callback;
  context->write_user = write_user;
  context->root.name = NULL;
  context->root.children = NULL;
  context->root.present_as_dir = true;
  BufferInit(&context->root.buffer);
  context->root.handles = NULL;
}

bool FuseContextHandle(struct FuseContext* context, int fuse) {
  static char buffer[FUSE_MIN_READ_BUFFER];
  ssize_t size = read(fuse, buffer, sizeof(buffer));
  if (size == -1) {
    LOG("Failed to read fuse (%s)", strerror(errno));
    return false;
  }

  struct FuseRequest* request = (void*)buffer;
  if (request->header.nodeid == FUSE_ROOT_ID)
    request->header.nodeid = (uint64_t)&context->root;

  typedef bool (*FuseRequestHandler)(const struct FuseRequest*,
                                     struct FuseContext*, int);
  static const FuseRequestHandler fuse_request_handlers[] = {
      [FUSE_LOOKUP] = OnFuseLookup,
      [FUSE_FORGET] = OnFuseForget,
      [FUSE_GETATTR] = OnFuseGetattr,
      [FUSE_MKDIR] = OnFuseMkdir,
      [FUSE_UNLINK] = OnFuseUnlink,
      [FUSE_RMDIR] = OnFuseUnlink,
      [FUSE_OPEN] = OnFuseOpen,
      [FUSE_READ] = OnFuseRead,
      [FUSE_RELEASE] = OnFuseRelease,
      [FUSE_INIT] = OnFuseInit,
      [FUSE_OPENDIR] = OnFuseOpendir,
      [FUSE_READDIR] = OnFuseReaddir,
      [FUSE_RELEASEDIR] = OnFuseReleasedir,
      [FUSE_CREATE] = OnFuseCreate,
      [FUSE_POLL] = OnFusePoll,
  };

  FuseRequestHandler request_handler = OnFuseUnknown;
  if (request->header.opcode < LENGTH(fuse_request_handlers) &&
      fuse_request_handlers[request->header.opcode]) {
    request_handler = fuse_request_handlers[request->header.opcode];
  }

  bool result = request_handler(request, context, fuse);
  if (!result) LOG("Failed to handle fuse request");
  return result;
}

bool FuseContextWrite(struct FuseContext* context, int fuse,
                      const char* pathname, size_t pathname_size,
                      const void* data, size_t data_size) {
  LOG("%.*s: %.*s", (int)pathname_size, pathname, (int)data_size,
      (const char*)data);

  struct MqttfsNode* node =
      RecurseStore(&context->root, pathname, pathname_size, data, data_size);
  if (!node) {
    LOG("Failed to handle external write");
    return false;
  }

  for (struct MqttfsHandle* it = node->handles; it; it = it->next) {
    if (!it->poll_handle) continue;
    it->updated = true;
    LOG("[%p] Notifying poll handle %lu", (void*)node, it->poll_handle);
    struct fuse_notify_poll_wakeup_out notify_poll_wakeup_out = {
        .kh = it->poll_handle,
    };
    WriteFuseNotify(fuse, FUSE_NOTIFY_POLL, &notify_poll_wakeup_out,
                    sizeof(notify_poll_wakeup_out));
  }
  return true;
}

void FuseContextCleanup(struct FuseContext* context) {
  tdestroy(context->root.children, MqttfsNodeDestroy);
}
