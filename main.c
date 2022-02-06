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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "entry.h"
#include "log.h"
#include "mqtt.h"
#include "mqttfs.h"

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
    // mburakov: Entry is preserved, but it will be empty.
    LOG(WARNING, "failed to copy payload: %s", strerror(errno));
    goto rollback_mtx_lock;
  }
  memcpy(data, payload, payloadlen);
  free(entry->data);
  entry->data = data;
  entry->size = payloadlen;

  if (entry->ph) {
    // mburakov: There's a blocked poll call on this entry.
    entry->was_updated = 1;
    int result = fuse_notify_poll(entry->ph);
    fuse_pollhandle_destroy(entry->ph);
    entry->ph = NULL;
    if (result) {
      // mburakov: Entry is preserved with its data, but poll will not wake.
      LOG(ERR, "failed to wake poll: %s", strerror(result));
      goto rollback_mtx_lock;
    }
  }

rollback_mtx_lock:
  if (mtx_unlock(&context->entries_mutex)) {
    LOG(CRIT, "failed to unlock entries mutex");
    // mburakov: This is unlikely to be possible, and there's nothing we can
    // really do here except just logging this error message.
  }
}

static void* MqttfsInit(struct fuse_conn_info* conn, struct fuse_config* cfg) {
  (void)conn;
  (void)cfg;
  // TODO(mburakov): How to bail out in a clean way?
  // TODO(mburakov): Uses lazy initialization and fail in actual ops?
  struct Context* context = calloc(1, sizeof(struct Context));
  if (!context) {
    LOG(ERR, "Failed to allocate context");
    return NULL;
  }
  if (mtx_init(&context->entries_mutex, mtx_plain)) {
    LOG(ERR, "failed to initialize entries mutex");
    goto rollback_context;
  }
  context->has_entries_mutex = 1;
  context->mqtt = MqttCreate(g_options.host, g_options.port,
                             g_options.keepalive, OnMqttMessage, context);
  if (!context->mqtt) {
    LOG(ERR, "failed to create mqtt");
    goto rollback_mtx_init;
  }
  cfg->direct_io = 1;
  // TODO(mburakov): Support this:
  // cfg->nullpath_ok = 1;
  return context;
rollback_mtx_init:
  mtx_destroy(&context->entries_mutex);
rollback_context:
  free(context);
  return NULL;
}

static void MqttfsDestroy(void* private_data) {
  struct Context* context = private_data;
  if (!context) return;
  if (context->mqtt) MqttDestroy(context->mqtt);
  if (context->has_entries_mutex) mtx_destroy(&context->entries_mutex);
  if (context->entries) EntryDestroy(context->entries);
  free(context);
}

static const struct fuse_operations g_fuse_operations = {
    .getattr = MqttfsGetattr,
    .mkdir = MqttfsMkdir,
    .read = MqttfsRead,
    .write = MqttfsWrite,
    .readdir = MqttfsReaddir,
    .init = MqttfsInit,
    .destroy = MqttfsDestroy,
    .create = MqttfsCreate,
    .poll = MqttfsPoll};

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
  result = fuse_main(args.argc, args.argv, &g_fuse_operations, NULL);
rollback_fuse_args_init:
  fuse_opt_free_args(&args);
  LOG(INFO, "clean shutdown");
  return result;
}
