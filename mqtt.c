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

#include "mqtt.h"

#include <errno.h>
#include <mosquitto.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

struct Mqtt {
  int holdback;
  MqttMessageCallback callback;
  void* user;
  struct mosquitto* mosq;
};

static void MqttMessageCallbackTrampoline(
    struct mosquitto* mosq, void* user,
    const struct mosquitto_message* message) {
  (void)mosq;
  struct Mqtt* context = user;
  context->callback(context->user, message->topic, message->payload,
                    (size_t)message->payloadlen);
}

struct Mqtt* MqttCreate(const char* host, uint16_t port, uint16_t keepalive,
                        int holdback, MqttMessageCallback callback,
                        void* user) {
  int runtime_version = mosquitto_lib_version(NULL, NULL, NULL);
  LOG(INFO, "using libmosquitto version %d (built against version %d)",
      runtime_version, LIBMOSQUITTO_VERSION_NUMBER);
  int mosq_errno = mosquitto_lib_init();
  if (mosq_errno) {
    LOG(ERR, "failed to initialize mosquitto lib: %s",
        mosquitto_strerror(mosq_errno));
    return 0;
  }
  struct Mqtt* result = malloc(sizeof(struct Mqtt));
  if (!result) {
    LOG(ERR, "failed to allocate mosquitto context: %s", strerror(errno));
    goto rollback_mosquitto_lib_init;
  }
  result->holdback = holdback;
  result->callback = callback;
  result->user = user;
  result->mosq = mosquitto_new(NULL, 1, result);
  if (!result->mosq) {
    LOG(ERR, "failed to create mosquitto instance: %s", strerror(errno));
    goto rollback_malloc;
  }
  mosquitto_message_callback_set(result->mosq, MqttMessageCallbackTrampoline);
  mosq_errno = mosquitto_loop_start(result->mosq);
  if (mosq_errno) {
    LOG(ERR, "failed to start mosquitto loop: %s",
        mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_new;
  }
  LOG(INFO, "connecting to MQTT broker on %s:%d...", host, port);
  mosq_errno = mosquitto_connect(result->mosq, host, port, keepalive);
  if (mosq_errno) {
    LOG(ERR, "failed to connect to MQTT broker: %s",
        mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_loop_start;
  }
  // TODO(mburakov): Make connection blocking
  mosq_errno = mosquitto_subscribe(result->mosq, NULL, "+/#", 0);
  if (mosq_errno) {
    LOG(ERR, "failed to subscribe to MQTT wildcard: %s",
        mosquitto_strerror(mosq_errno));
    goto rollback_mosquitto_connect;
  }
  return result;
rollback_mosquitto_connect:
  mosq_errno = mosquitto_disconnect(result->mosq);
  if (mosq_errno) {
    LOG(WARNING, "failed to disconnect from MQTT broker: %s",
        mosquitto_strerror(mosq_errno));
  }
rollback_mosquitto_loop_start:
  mosq_errno = mosquitto_loop_stop(result->mosq, 1);
  if (mosq_errno) {
    LOG(WARNING, "failed to stop mosquitto loop: %s",
        mosquitto_strerror(mosq_errno));
  }
rollback_mosquitto_new:
  mosquitto_destroy(result->mosq);
rollback_malloc:
  free(result);
rollback_mosquitto_lib_init:
  mosq_errno = mosquitto_lib_cleanup();
  if (mosq_errno) {
    LOG(WARNING, "failed to cleanup mosquitto lib: %s",
        mosquitto_strerror(mosq_errno));
  }
  return NULL;
}

_Bool MqttPublish(struct Mqtt* mqtt, const char* topic, const void* payload,
                  size_t size) {
  int mosq_error =
      mosquitto_publish(mqtt->mosq, NULL, topic, (int)size, payload, 0, 0);
  if (mosq_error) {
    LOG(WARNING, "failed to publish topc: %s", mosquitto_strerror(mosq_error));
    return 0;
  }
  return 1;
}

void MqttCancel(struct Mqtt* mqtt, const char* topic) {
  // TODO(mburakov): Implement me!
  (void)mqtt;
  (void)topic;
}

void MqttDestroy(struct Mqtt* mqtt) {
  int mosq_errno = mosquitto_disconnect(mqtt->mosq);
  if (mosq_errno) {
    LOG(WARNING, "failed to disconnect mosquitto: %s",
        mosquitto_strerror(mosq_errno));
  }
  mosq_errno = mosquitto_loop_stop(mqtt->mosq, 1);
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    LOG(WARNING, "failed to stop mosquitto loop: %s",
        mosquitto_strerror(mosq_errno));
  }
  mosquitto_destroy(mqtt->mosq);
  free(mqtt);
  mosq_errno = mosquitto_lib_cleanup();
  if (mosq_errno != MOSQ_ERR_SUCCESS) {
    LOG(WARNING, "failed to cleanup mosquitto lib: %s",
        mosquitto_strerror(mosq_errno));
  }
}
