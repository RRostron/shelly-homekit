/*
 * Copyright (c) 2020 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos.h"
#include "mgos_app.h"
#include "mgos_hap.h"

#include "HAPPlatform+Init.h"
#include "HAPPlatformAccessorySetup+Init.h"
#include "HAPPlatformKeyValueStore+Init.h"
#include "HAPPlatformServiceDiscovery+Init.h"
#include "HAPPlatformTCPStreamManager+Init.h"

#include "shelly_sw_service.h"

#define KVS_FILE_NAME "kvs.json"
#define NUM_SESSIONS 3
#define IO_BUF_SIZE 2048
#define kAttributeCount 50

static HAPIPSession sessions[NUM_SESSIONS];
static uint8_t in_bufs[NUM_SESSIONS][IO_BUF_SIZE];
static uint8_t out_bufs[NUM_SESSIONS][IO_BUF_SIZE];
static uint8_t scratch_buf[IO_BUF_SIZE];
static HAPIPEventNotificationRef
    notifications[NUM_SESSIONS][kAttributeCount];  // TODO: make dynamic
static HAPIPAccessoryServerStorage s_ip_storage = {
    .sessions = sessions,
    .numSessions = ARRAY_SIZE(sessions),
    .scratchBuffer =
        {
            .bytes = scratch_buf,
            .numBytes = sizeof(scratch_buf),
        },
};

static HAPPlatformKeyValueStore s_kvs;
static HAPPlatformAccessorySetup s_accessory_setup;
static HAPAccessoryServerOptions s_server_options = {
    .ip =
        {
            .transport = &kHAPAccessoryServerTransport_IP,
            .accessoryServerStorage = &s_ip_storage,
        },
};
static HAPAccessoryServerCallbacks s_callbacks;
static HAPPlatformTCPStreamManager s_tcp_stream_manager;
static HAPPlatformServiceDiscovery s_service_discovery;
static HAPAccessoryServerRef s_accessory_server;

static HAPPlatform s_platform = {
    .keyValueStore = &s_kvs,
    .accessorySetup = &s_accessory_setup,
    .ip =
        {
            .tcpStreamManager = &s_tcp_stream_manager,
            .serviceDiscovery = &s_service_discovery,
        },
};

HAPError shelly_identify_cb(HAPAccessoryServerRef *server,
                            const HAPAccessoryIdentifyRequest *request,
                            void *context);

static HAPAccessory s_accessory = {
    .aid = 1,
    .category = kHAPAccessoryCategory_Switches,
    .name = NULL,  // Set from config,
    .manufacturer = CS_STRINGIFY_MACRO(PRODUCT_VENDOR),
    .model = CS_STRINGIFY_MACRO(PRODUCT_MODEL),
    .serialNumber = NULL,     // Set from config.
    .firmwareVersion = NULL,  // Set from build_id.
    .hardwareVersion = CS_STRINGIFY_MACRO(PRODUCT_HW_REV),
    .services = NULL,  // Set later
    .callbacks = {.identify = shelly_identify_cb},
};

HAPError shelly_identify_cb(HAPAccessoryServerRef *server,
                            const HAPAccessoryIdentifyRequest *request,
                            void *context) {
  // TODO: Can we do something better here?
  LOG(LL_INFO, ("=== IDENTIFY ==="));
  (void) server;
  (void) request;
  (void) context;
  return kHAPError_None;
}

static void shelly_hap_server_state_update_cb(HAPAccessoryServerRef *server,
                                              void *context) {
  LOG(LL_INFO, ("HAP server state: %d", HAPAccessoryServerGetState(server)));
  (void) context;
}

static void shelly_status_timer_cb(void *arg) {
  static bool s_tick_tock = false;
  LOG(LL_INFO,
      ("%s uptime: %.2lf, RAM: %lu, %lu free", (s_tick_tock ? "Tick" : "Tock"),
       mgos_uptime(), (unsigned long) mgos_get_heap_size(),
       (unsigned long) mgos_get_free_heap_size()));
  s_tick_tock = !s_tick_tock;
  (void) arg;
}

enum mgos_app_init_result mgos_app_init(void) {
  HAPSetupInfo setupInfo;
  if (!mgos_hap_setup_info_from_string(&setupInfo,
                                       mgos_sys_config_get_hap_salt(),
                                       mgos_sys_config_get_hap_verifier())) {
    LOG(LL_ERROR, ("=== Accessory not provisioned"));
    mgos_hap_add_rpc_service();
    return MGOS_APP_INIT_SUCCESS;
  }

  // Key-value store.
  HAPPlatformKeyValueStoreCreate(
      &s_kvs,
      &(const HAPPlatformKeyValueStoreOptions){.fileName = KVS_FILE_NAME});

  // Accessory setup.
  HAPPlatformAccessorySetupCreate(&s_accessory_setup,
                                  &(const HAPPlatformAccessorySetupOptions){});

  // TCP Stream Manager.
  HAPPlatformTCPStreamManagerCreate(
      &s_tcp_stream_manager, &(const HAPPlatformTCPStreamManagerOptions){
                                 .port = kHAPNetworkPort_Any,
                                 .maxConcurrentTCPStreams = NUM_SESSIONS});

  // Service discovery.
  HAPPlatformServiceDiscoveryCreate(
      &s_service_discovery, &(const HAPPlatformServiceDiscoveryOptions){});

  for (size_t i = 0; i < ARRAY_SIZE(sessions); i++) {
    sessions[i].inboundBuffer.bytes = in_bufs[i];
    sessions[i].inboundBuffer.numBytes = sizeof(in_bufs[i]);
    sessions[i].outboundBuffer.bytes = out_bufs[i];
    sessions[i].outboundBuffer.numBytes = sizeof(out_bufs[i]);
    sessions[i].eventNotifications = notifications[i];
    sessions[i].numEventNotifications = ARRAY_SIZE(notifications[i]);
  }

  s_server_options.maxPairings = kHAPPairingStorage_MinElements;

  s_callbacks.handleUpdatedState = shelly_hap_server_state_update_cb;

  s_accessory.name = mgos_sys_config_get_device_id();
  s_accessory.firmwareVersion = mgos_sys_ro_vars_get_fw_version();
  s_accessory.serialNumber = mgos_sys_config_get_device_sn();

  const HAPService **services = calloc(3 + NUM_SWITCHES + 1, sizeof(*services));
  services[0] = &mgos_hap_accessory_information_service;
  services[1] = &mgos_hap_protocol_information_service;
  services[2] = &mgos_hap_pairing_service;
  int i = 3;
  if (mgos_sys_config_get_sw1_enable()) {
    const struct mgos_config_sw *cfg = mgos_sys_config_get_sw1();
    services[i++] = shelly_sw_service_create(cfg);
  }
  s_accessory.services = services;
  LOG(LL_INFO, ("Exported %d of %d switches", i - 3, NUM_SWITCHES));

  // Initialize accessory server.
  HAPAccessoryServerCreate(&s_accessory_server, &s_server_options, &s_platform,
                           &s_callbacks, NULL /* context */);

  HAPAccessoryServerStart(&s_accessory_server, &s_accessory);

  // Timer for periodic status.
  mgos_set_timer(1000, MGOS_TIMER_REPEAT, shelly_status_timer_cb, NULL);

  return MGOS_APP_INIT_SUCCESS;
}