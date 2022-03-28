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

#ifndef MQTTFS_LOG_H_
#define MQTTFS_LOG_H_

#include <fuse_log.h>

#define STR_IMPL(op) #op
#define STR(op) STR_IMPL(op)

#define LOG(level, fmt, ...) \
  fuse_log(FUSE_LOG_##level, \
           "mqttfs: " __FILE__ ":" STR(__LINE__) " " fmt "\n", ##__VA_ARGS__)

#endif  // MQTTFS_LOG_H_
