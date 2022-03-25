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

#include "node.h"

#include <errno.h>
#include <search.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "str.h"

#ifndef UNCONST
#define UNCONST(op) ((void*)(uintptr_t)op)
#endif  // UNCONST

// mburakov: musl does not implement twalk_r.
static void* g_twalk_closure;

static int CompareNodes(const void* lh, const void* rh) {
  const struct Node* lh_node = lh;
  const struct Node* rh_node = rh;
  return strcmp(lh_node->name, rh_node->name);
}

static void NodeCallbackTrampoline(const void* nodep, VISIT which, int depth) {
  (void)depth;
  struct {
    NodeCallback callback;
    void* user;
  }* context = g_twalk_closure;
  const struct Node* const* node = nodep;
  if (which == preorder || which == leaf)
    context->callback(context->user, *node);
}

static void DestroyNode(void* nodep) {
  struct Node* node = nodep;
  if (node->is_dir) {
    tdestroy(node->as_dir.subs, &DestroyNode);
  } else {
    free(node->as_file.data);
    StrFree(&node->as_file.topic);
  }
  free(UNCONST(node->name));
  free(node);
}

struct Node* NodeCreate(const char* name, _Bool is_dir) {
  struct Node temp = {
      .name = strdup(name),
      .is_dir = is_dir,
  };
  if (!temp.name) {
    LOG(ERR, "failed to strdup name: %s", strerror(errno));
    return NULL;
  }
  struct Node* node = malloc(sizeof(struct Node));
  if (!node) {
    LOG(ERR, "failed to allocate node: %s", strerror(errno));
    free(UNCONST(temp.name));
    return NULL;
  }
  memcpy(node, &temp, sizeof(struct Node));
  NodeTouch(node, 1, 1);
  return node;
}

struct Node* NodeFind(struct Node* node, char* path) {
  for (const char* token = strtok(path, "/"); token;
       token = strtok(NULL, "/")) {
    node = NodeGet(node, token);
    if (!node) return NULL;
  }
  return node;
}

struct Node* NodeGet(struct Node* node, const char* name) {
  if (!node->is_dir) return NULL;
  const struct Node key = {.name = name};
  struct Node** pnode = tfind(&key, &node->as_dir.subs, CompareNodes);
  return pnode ? *pnode : NULL;
}

_Bool NodeTouch(struct Node* node, _Bool atime, _Bool mtime) {
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
    LOG(ERR, "failed to get clock: %s", strerror(errno));
    return 0;
  }
  if (atime) node->atime = now;
  if (mtime) node->mtime = now;
  return 1;
}

_Bool NodeInsert(struct Node* parent, const struct Node* node) {
  const struct Node* const* pnode =
      tsearch(node, &parent->as_dir.subs, CompareNodes);
  if (!pnode || *pnode != node) return 0;
  NodeTouch(parent, 0, 1);
  return 1;
}

void NodeForEach(struct Node* node, NodeCallback callback, void* user) {
  struct {
    NodeCallback callback;
    void* user;
  } context = {callback, user};
  g_twalk_closure = &context;
  twalk(node->as_dir.subs, NodeCallbackTrampoline);
  NodeTouch(node, 1, 0);
}

void NodeRemove(struct Node* parent, const struct Node* node) {
  tdelete(node, &parent->as_dir.subs, CompareNodes);
  NodeTouch(parent, 0, 1);
}

void NodeDestroy(struct Node* node) { DestroyNode(node); }
