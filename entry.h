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

#ifndef MQTTFS_ENTRY_H_
#define MQTTFS_ENTRY_H_

#include <stddef.h>

struct Entry {
  const char* name;
  void* data;
  size_t size;
  void* subs;
  _Bool dir;
};

typedef void (*EntryWalkCallback)(void* user, const struct Entry* node);

const struct Entry* EntryFind(void* const* rootp, const char* path);
struct Entry* EntrySearch(void** rootp, const char* path);
void EntryWalk(const void* root, EntryWalkCallback callback, void* user);
void EntryDestroy(void* root);

#endif  // MQTTFS_ENTRY_H_
