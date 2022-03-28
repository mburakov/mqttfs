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

#ifndef MQTTFS_STR_H_
#define MQTTFS_STR_H_

#include <stddef.h>

struct Str {
  size_t size;
  const char* data;
};

struct Str StrView(const char* str);
_Bool StrCopy(struct Str* to, const struct Str* from);
int StrCompare(const struct Str* a, const struct Str* b);
struct Str StrBasePath(const struct Str* path);
const char* StrFileName(const struct Str* path);
void StrFree(const struct Str* op);

#endif  // MQTTFS_STR_H_
