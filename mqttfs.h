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

#ifndef MQTTFS_H_
#define MQTTFS_H_

#include <fuse.h>
#include <sys/types.h>
#include <threads.h>

struct Context {
  void* entries;
  mtx_t entries_mutex;
  _Bool has_entries_mutex;
  struct Mqtt* mqtt;
};

int MqttfsGetattr(const char* path, struct stat* stbuf,
                  struct fuse_file_info* fi);
int MqttfsMkdir(const char* path, mode_t mode);
int MqttfsRead(const char* path, char* buf, size_t size, off_t offset,
               struct fuse_file_info* fi);
int MqttfsWrite(const char* path, const char* buf, size_t size, off_t offset,
                struct fuse_file_info* fi);
int MqttfsReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info* fi,
                  enum fuse_readdir_flags flags);
int MqttfsCreate(const char* path, mode_t mode, struct fuse_file_info* fi);

#endif  // MQTTFS_H_
