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

#include <errno.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>

#include "entry.h"
#include "log.h"
#include "mqtt.h"

struct Context {
  void* entries;
  mtx_t entries_mutex;
  struct Mqtt* mqtt;
};

static struct Options {
  const char* host;
  int port;
  int keepalive;
  int show_help;
} g_options;

static const struct fuse_opt g_options_spec[] = {
    {"--host=%s", offsetof(struct Options, host), 0},
    {"--port=%d", offsetof(struct Options, port), 0},
    {"--keepalive=%d", offsetof(struct Options, keepalive), 0},
    {"-h", offsetof(struct Options, show_help), 1},
    {"--help", offsetof(struct Options, show_help), 1},
    FUSE_OPT_END};

static void ShowHelp(struct fuse_args* args) {
  printf("Usage: %s [options] <mountpoint>\n\n", args->argv[0]);
  printf(
      "File-system specific options:\n"
      "    --host=S               Hostname or IP address of MQTT broker\n"
      "                           (default: \"localhost\")\n"
      "    --port=N               TCP port of MQTT broker\n"
      "                           (default: 1883)\n"
      "    --keepalive=N          Keepalive parameter of MQTT connection\n"
      "                           (default: 60)\n"
      "\n");
  printf("FUSE options:\n");
  fuse_lib_help(args);
}

static void OnMqttMessage(void* user, const char* topic, const void* payload,
                          size_t payloadlen) {
  struct Context* context = user;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(WARNING, "failed to lock entries mutex");
    return;
  }
  struct Entry* entry = EntrySearch(&context->entries, topic);
  if (!entry) {
    LOG(WARNING, "failed to create entry");
    goto rollback_mtx_lock;
  }
  void* data = malloc(payloadlen);
  if (!data) {
    // mburakov: Entry is preserved, but it will be empty
    LOG(WARNING, "failed to copy payload: %s", strerror(errno));
    goto rollback_mtx_lock;
  }
  memcpy(data, payload, payloadlen);
  free(entry->data);
  entry->data = data;
  entry->size = payloadlen;
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock entries mutex");
    // TODO(mburakov): What to do here?
  }
}

static int MqttfsGetattr(const char* path, struct stat* stbuf,
                         struct fuse_file_info* fi) {
  (void)fi;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;
  const struct Entry root = {.subs = context->entries};
  const struct Entry* entry =
      strcmp(path, "/") ? EntryFind(&root.subs, path) : &root;
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }
  memset(stbuf, 0, sizeof(struct stat));
  if (entry->subs || entry == &root) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
  } else {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_size = (off_t)entry->size;
  }
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock entries mutex");
    // TODO(mburakov): What to do here?
  }
  return result;
}

static int MqttfsRead(const char* path, char* buf, size_t size, off_t offset,
                      struct fuse_file_info* fi) {
  (void)fi;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;
  const struct Entry root = {.subs = context->entries};
  const struct Entry* entry =
      strcmp(path, "/") ? EntryFind(&root.subs, path) : &root;
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }
  size_t read_end = (size_t)offset + size;
  if (read_end > entry->size) read_end = entry->size;
  size_t read_size = read_end - (size_t)offset;
  const char* read_from = (const char*)entry->data + offset;
  memcpy(buf, read_from, read_size);
  result = (int)read_size;
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock entries mutex");
    // TODO(mburakov): What to do here?
  }
  return result;
}

static int MqttfsWrite(const char* path, const char* buf, size_t size,
                       off_t offset, struct fuse_file_info* fi) {
  (void)fi;
  (void)offset;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;
  struct Entry root = {.subs = context->entries};
  struct Entry* entry =
      strcmp(path, "/") ? EntrySearch(&root.subs, path) : &root;
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }
  if (entry->subs) {
    // mburakov: Entry is preserved, but it will be empty
    // TODO(mburakov): Does this status make sense?
    result = -EISDIR;
    goto rollback_mtx_lock;
  }
  void* data = malloc(size);
  if (!data) {
    // mburakov: Entry is preserved, but it will be empty
    result = -EIO;
    goto rollback_mtx_lock;
  }
  memcpy(data, buf, size);
  free(entry->data);
  entry->data = data;
  entry->size = size;
  if (!MqttPublish(context->mqtt, path + 1, data, size)) {
    result = -EIO;
    goto rollback_mtx_lock;
  }
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock entries mutex");
    // TODO(mburakov): What to do here?
  }
  return (int)size;
}

static void OnReaddirWalk(void* user, const struct Entry* entry) {
  struct {
    void* buf;
    fuse_fill_dir_t filler;
  }* context = user;
  // TODO(mburakov): Handle filler errors
  context->filler(context->buf, entry->name, NULL, 0,
                  (enum fuse_fill_dir_flags)0);
}

static int MqttfsReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info* fi,
                         enum fuse_readdir_flags flags) {
  (void)offset;
  (void)fi;
  (void)flags;
  struct Context* context = fuse_get_context()->private_data;
  if (mtx_lock(&context->entries_mutex)) {
    LOG(ERR, "failed to lock entries mutex");
    return -EIO;
  }
  int result = 0;
  const struct Entry root = {.subs = context->entries};
  const struct Entry* entry =
      strcmp(path, "/") ? EntryFind(&root.subs, path) : &root;
  if (!entry) {
    result = -ENOENT;
    goto rollback_mtx_lock;
  }
  // TODO(mburakov): Handle filler errors
  filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
  filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
  struct {
    void* buf;
    fuse_fill_dir_t filler;
  } user = {buf, filler};
  EntryWalk(entry->subs, OnReaddirWalk, &user);
rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock nodes mutex");
    // TODO(mburakov): What to do here?
  }
  return result;
}

static const struct fuse_operations g_fuse_operations = {
    .getattr = MqttfsGetattr,
    .read = MqttfsRead,
    .write = MqttfsWrite,
    .readdir = MqttfsReaddir};

int main(int argc, char* argv[]) {
  int result = EXIT_FAILURE;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  g_options.host = strdup("localhost");
  if (!g_options.host) {
    LOG(ERR, "failed to allocate hostname");
    goto rollback_fuse_args_init;
  }
  g_options.port = 1883;
  g_options.keepalive = 60;
  if (fuse_opt_parse(&args, &g_options, g_options_spec, NULL) == -1) {
    LOG(ERR, "failed to parse commandline");
    goto rollback_fuse_args_init;
  }
  if (g_options.show_help) {
    ShowHelp(&args);
    result = EXIT_SUCCESS;
    goto rollback_fuse_args_init;
  }
  if (0 > g_options.port || g_options.port > UINT16_MAX) {
    LOG(ERR, "invalid port number %d", g_options.port);
    goto rollback_fuse_args_init;
  }
  if (0 > g_options.keepalive) {
    LOG(ERR, "invalid keepalive %d", g_options.keepalive);
    goto rollback_fuse_args_init;
  }
  // TODO(mburakov): The current implementation is multithreaded, but one does
  // not simply fork a multithreaded process. So enforce foreground mode.
  if (fuse_opt_add_arg(&args, "-f")) {
    LOG(ERR, "failed to enforce foregroung mode");
    goto rollback_fuse_args_init;
  }
  // TODO(mburakov): How to verify the sanity of the commandline before
  // procceeding with the full initialization?
  struct Context context;
  memset(&context, 0, sizeof(context));
  if (mtx_init(&context.entries_mutex, mtx_plain)) {
    LOG(ERR, "failed to initialize entries mutex");
    goto rollback_fuse_args_init;
  }
  context.mqtt = MqttCreate(g_options.host, g_options.port, g_options.keepalive,
                            OnMqttMessage, &context);
  if (!context.mqtt) {
    LOG(ERR, "failed to create mqtt");
    goto rollback_mtx_init;
  }
  result = fuse_main(args.argc, args.argv, &g_fuse_operations, &context);
  MqttDestroy(context.mqtt);
rollback_mtx_init:
  mtx_destroy(&context.entries_mutex);
rollback_fuse_args_init:
  fuse_opt_free_args(&args);
  LOG(INFO, "clean shutdown");
  return result;
}
