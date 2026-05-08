#include "mqtt_link_service.h"

#include "cluster_service.h"
#include "../Config/device_config.h"

#include "stm32f7xx_hal.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

typedef struct
{
  uint8_t started;
  uint8_t status_dirty;
  uint32_t last_poll_ms;
  mqtt_link_stats_t stats;
} mqtt_link_context_t;

static mqtt_link_context_t g_mqtt_link_context;

static void mqtt_link_build_topics(const device_config_t *config)
{
  mqtt_link_stats_t *stats = &g_mqtt_link_context.stats;

  if (config == NULL)
  {
    return;
  }

  (void)snprintf(stats->client_id, sizeof(stats->client_id),
                 "autoControl-%s-%s",
                 config->cloud_cluster_id,
                 config->cloud_device_id);

  (void)snprintf(stats->topic_status, sizeof(stats->topic_status),
                 "cluster/%s/status",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_events, sizeof(stats->topic_events),
                 "cluster/%s/events",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_measure, sizeof(stats->topic_measure),
                 "cluster/%s/measure",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_relay, sizeof(stats->topic_command_relay),
                 "cluster/%s/command/relay",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_mask, sizeof(stats->topic_command_mask),
                 "cluster/%s/command/mask",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_command_config_relay, sizeof(stats->topic_command_config_relay),
                 "cluster/%s/command/config/relay",
                 config->cloud_cluster_id);
  (void)snprintf(stats->topic_ack_prefix, sizeof(stats->topic_ack_prefix),
                 "cluster/%s/ack/",
                 config->cloud_cluster_id);

  strncpy(stats->cluster_ca_fingerprint,
          config->cloud_cluster_ca_fingerprint,
          sizeof(stats->cluster_ca_fingerprint) - 1U);
  stats->cluster_ca_fingerprint[sizeof(stats->cluster_ca_fingerprint) - 1U] = '\0';
  strncpy(stats->cluster_binding_secret,
          config->cloud_cluster_binding_secret,
          sizeof(stats->cluster_binding_secret) - 1U);
  stats->cluster_binding_secret[sizeof(stats->cluster_binding_secret) - 1U] = '\0';
}

static uint8_t mqtt_link_network_ready(void)
{
  if ((netif_is_up(&gnetif) == 0) || (netif_is_link_up(&gnetif) == 0))
  {
    return 0U;
  }

  if (ip4_addr_isany_val(*netif_ip4_addr(&gnetif)) != 0)
  {
    return 0U;
  }

  return 1U;
}

static uint8_t mqtt_link_configured(const device_config_t *config)
{
  if (config == NULL)
  {
    return 0U;
  }

  if (config->cloud_enabled == 0U)
  {
    return 0U;
  }

  if ((config->cloud_broker_host[0] == '\0') ||
      (config->cloud_broker_port == 0U) ||
      (config->cloud_cluster_id[0] == '\0') ||
      (config->cloud_device_id[0] == '\0') ||
      (config->cloud_key_id[0] == '\0') ||
      (config->cloud_cluster_ca_fingerprint[0] == '\0') ||
      (config->cloud_cluster_binding_secret[0] == '\0'))
  {
    return 0U;
  }

  return 1U;
}

void mqtt_link_service_mark_status_dirty(void)
{
  g_mqtt_link_context.status_dirty = 1U;
  g_mqtt_link_context.stats.status_dirty_count++;
}

void mqtt_link_service_init(void)
{
  if (g_mqtt_link_context.started != 0U)
  {
    return;
  }

  memset(&g_mqtt_link_context, 0, sizeof(g_mqtt_link_context));
  g_mqtt_link_context.started = 1U;
}

void mqtt_link_service_poll(void)
{
  const device_config_t *config;
  cluster_service_state_t cluster_state;
  uint8_t configured;
  uint8_t should_run = 0U;

  mqtt_link_service_init();

  config = device_config_get();
  configured = mqtt_link_configured(config);
  cluster_service_get_state(&cluster_state);

  if ((configured != 0U) &&
      (mqtt_link_network_ready() != 0U) &&
      (cluster_state.cluster_enabled != 0U) &&
      (cluster_state.role == CLUSTER_ROLE_MASTER) &&
      (cluster_state.master_node_id == cluster_state.self_node_id))
  {
    should_run = 1U;
  }

  g_mqtt_link_context.stats.configured = configured;
  g_mqtt_link_context.stats.enabled = (config->cloud_enabled != 0U) ? 1U : 0U;
  g_mqtt_link_context.stats.transport = config->cloud_transport;
  g_mqtt_link_context.stats.broker_port = config->cloud_broker_port;
  g_mqtt_link_context.stats.should_run = should_run;
  g_mqtt_link_context.last_poll_ms = HAL_GetTick();
  mqtt_link_build_topics(config);

  if (configured == 0U)
  {
    g_mqtt_link_context.stats.last_error = 1001U;
  }
  else if (mqtt_link_network_ready() == 0U)
  {
    g_mqtt_link_context.stats.last_error = 1002U;
  }
  else if (should_run == 0U)
  {
    g_mqtt_link_context.stats.last_error = 1003U;
  }
  else
  {
    g_mqtt_link_context.stats.last_error = 0U;
  }
}

void mqtt_link_service_get_stats(mqtt_link_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_mqtt_link_context.stats;
  }
}
