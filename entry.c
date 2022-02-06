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

#include "entry.h"

#include <errno.h>
#include <search.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define UNCONST(op) ((void*)(ptrdiff_t)op)

// mburakov: musl does not implement twalk_r.
static void* g_twalk_closure;

static void EntryWalkCallbackTrampoline(const void* nodep, VISIT which,
                                        int depth) {
  (void)depth;
  struct {
    EntryWalkCallback callback;
    void* user;
  }* context = g_twalk_closure;
  if (which == preorder || which == leaf)
    context->callback(context->user, *(void* const*)nodep);
}

static int EntryCompare(const void* lh, const void* rh) {
  const struct Entry* lh_entry = lh;
  const struct Entry* rh_entry = rh;
  return strcmp(lh_entry->name, rh_entry->name);
}

static struct Entry* EntryCreate(const char* name) {
  struct Entry* result = calloc(1, sizeof(struct Entry));
  if (!result) {
    LOG(WARNING, "failed to allocate entry: %s", strerror(errno));
    return NULL;
  }
  result->name = strdup(name);
  if (!result->name) {
    LOG(WARNING, "failed to copy entry name: %s", strerror(errno));
    goto rollback_calloc;
  }
  return result;
rollback_calloc:
  free(result);
  return NULL;
}

static void EntryFree(void* key) {
  struct Entry* entry = key;
  tdestroy(entry->subs, EntryFree);
  free(entry->data);
  free(UNCONST(entry->name));
  free(entry);
}

const struct Entry* EntryFind(void* const* rootp, const char* path) {
  char* path_copy = strdup(path);
  if (!path_copy) {
    LOG(WARNING, "failed to copy path: %s", strerror(errno));
    return NULL;
  }
  const struct Entry* result = NULL;
  const struct Entry* const* pentry = &result;
  for (char* item = strtok(path_copy, "/"); item; item = strtok(NULL, "/")) {
    const struct Entry key = {.name = item};
    pentry = tfind(&key, rootp, EntryCompare);
    if (!pentry) goto rollback_strdup;
    rootp = &(*pentry)->subs;
  }
  result = *pentry;
rollback_strdup:
  free(path_copy);
  return result;
}

struct Entry* EntrySearch(void** rootp, const char* path) {
  char* path_copy = strdup(path);
  if (!path_copy) {
    LOG(WARNING, "failed to copy path: %s", strerror(errno));
    return NULL;
  }
  struct Entry* result = NULL;
  struct Entry** pentry = &result;
  for (char* item = strtok(path_copy, "/"); item; item = strtok(NULL, "/")) {
    const struct Entry key = {.name = item};
    pentry = tsearch(&key, rootp, EntryCompare);
    if (!pentry) {
      // mburakov: In case anything was created before, it persists.
      LOG(WARNING, "failed to insert entry");
      goto rollback_strdup;
    }
    if (*pentry == &key) {
      LOG(DEBUG, "creating new entry %s", item);
      struct Entry* entry = EntryCreate(item);
      if (!entry) {
        // mburakov: In case anything was created before, it persists.
        LOG(WARNING, "failed to create entry");
        tdelete(&key, rootp, EntryCompare);
        goto rollback_strdup;
      }
      *pentry = entry;
    }
    rootp = &(*pentry)->subs;
  }
  result = *pentry;
rollback_strdup:
  free(path_copy);
  return result;
}

void EntryWalk(const void* root, EntryWalkCallback callback, void* user) {
  struct {
    EntryWalkCallback callback;
    void* user;
  } context = {callback, user};
  g_twalk_closure = &context;
  twalk(root, EntryWalkCallbackTrampoline);
}

void EntryDestroy(void* root) { tdestroy(root, EntryFree); }
