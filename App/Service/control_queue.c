#include "control_queue.h"
#include "cluster_service.h"

#include "cmsis_os.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "../Config/device_config.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  control_proto_command_t command;
  relay_domain_result_t *result;
  TaskHandle_t waiting_task;
  uint32_t remote_ip_addr;
  uint32_t remote_node_id;
  uint8_t remote;
} control_queue_request_t;

#define CONTROL_QUEUE_LENGTH             32U
#define CONTROL_QUEUE_WAIT_TIMEOUT_MS    1000U

static control_queue_stats_t g_control_queue_stats;
static QueueHandle_t g_control_queue_handle = NULL;
static osThreadId g_control_queue_task_handle = NULL;

static void control_queue_format_request(const control_proto_command_t *command,
                                         char *buffer,
                                         size_t buffer_size)
{
  if ((command == NULL) || (buffer == NULL) || (buffer_size == 0U))
  {
    return;
  }

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_PING:
      (void)snprintf(buffer, buffer_size, "PING\r\n");
      break;

    case CONTROL_PROTO_CMD_GET_RELAY:
      (void)snprintf(buffer, buffer_size, "GET RELAY\r\n");
      break;

    case CONTROL_PROTO_CMD_SET_RELAY:
      (void)snprintf(buffer, buffer_size, "SET RELAY %u %u\r\n",
                     (unsigned int)command->channel,
                     (unsigned int)command->value);
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      (void)snprintf(buffer, buffer_size, "SET RELAY MASK 0x%04lX\r\n",
                     (unsigned long)command->mask);
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      (void)snprintf(buffer, buffer_size, "SET RELAY ALL %u\r\n",
                     (unsigned int)command->value);
      break;

    case CONTROL_PROTO_CMD_INVALID:
    default:
      buffer[0] = '\0';
      break;
  }
}

static void control_queue_parse_remote_response(const char *response, relay_domain_result_t *result)
{
  const char *relay_text;
  char *endptr = NULL;
  unsigned long relay_value;

  if ((response == NULL) || (result == NULL))
  {
    return;
  }

  memset(result, 0, sizeof(*result));
  result->error = CONTROL_PROTO_ERR_BAD_COMMAND;

  if (strncmp(response, "OK ", 3) == 0)
  {
    relay_text = strstr(response, "relay=0x");
    if (relay_text != NULL)
    {
      relay_value = strtoul(relay_text + 8, &endptr, 16);
      if (endptr != NULL)
      {
        result->relay_mask = (uint32_t)relay_value;
      }
    }
    result->error = CONTROL_PROTO_ERR_NONE;
    return;
  }

  if (strstr(response, "ERR code=bad_channel") != NULL)
  {
    result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
  }
  else if (strstr(response, "ERR code=bad_value") != NULL)
  {
    result->error = CONTROL_PROTO_ERR_BAD_VALUE;
  }
}

static HAL_StatusTypeDef control_queue_recv_and_parse(int sock, relay_domain_result_t *result)
{
  char response[256];
  int response_len;

  if (result == NULL)
  {
    return HAL_ERROR;
  }

  response_len = lwip_recv(sock, response, sizeof(response) - 1, 0);
  if (response_len <= 0)
  {
    return HAL_ERROR;
  }

  response[response_len] = '\0';
  control_queue_parse_remote_response(response, result);
  return (result->error == CONTROL_PROTO_ERR_NONE) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef control_queue_fetch_remote_relay_mask(uint32_t ip_addr, relay_domain_result_t *result)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  static const char request[] = "GET RELAY\r\n";

  if ((ip_addr == 0U) || (result == NULL))
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (lwip_send(sock, request, strlen(request), 0) < 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (control_queue_recv_and_parse(sock, result) != HAL_OK)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  lwip_close(sock);
  return HAL_OK;
}

static HAL_StatusTypeDef control_queue_execute_remote_async_fast(uint32_t ip_addr,
                                                                 const control_proto_command_t *command)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  char request[64];

  if ((command == NULL) || (ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  control_queue_format_request(command, request, sizeof(request));
  if (request[0] == '\0')
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 0;
  timeout_value.tv_usec = 250000;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (lwip_send(sock, request, strlen(request), 0) < 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  lwip_close(sock);
  return HAL_OK;
}

static HAL_StatusTypeDef control_queue_execute_remote(uint32_t ip_addr,
                                                      const control_proto_command_t *command,
                                                      relay_domain_result_t *result)
{
  int sock = -1;
  struct sockaddr_in remote_addr;
  struct timeval timeout_value;
  char request[64];
  HAL_StatusTypeDef recv_status;

  if ((command == NULL) || (result == NULL) || (ip_addr == 0U))
  {
    return HAL_ERROR;
  }

  control_queue_format_request(command, request, sizeof(request));
  if (request[0] == '\0')
  {
    return HAL_ERROR;
  }

  sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    return HAL_ERROR;
  }

  timeout_value.tv_sec = 1;
  timeout_value.tv_usec = 0;
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
  (void)lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = PP_HTONS(41000U);
  remote_addr.sin_addr.s_addr = ip_addr;

  if (lwip_connect(sock, (const struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  if (lwip_send(sock, request, strlen(request), 0) < 0)
  {
    lwip_close(sock);
    return HAL_ERROR;
  }

  memset(result, 0, sizeof(*result));
  result->error = CONTROL_PROTO_ERR_NONE;

  recv_status = control_queue_recv_and_parse(sock, result);
  lwip_close(sock);

  if (recv_status == HAL_OK)
  {
    return HAL_OK;
  }

  if ((command->type == CONTROL_PROTO_CMD_SET_RELAY) ||
      (command->type == CONTROL_PROTO_CMD_SET_RELAY_MASK) ||
      (command->type == CONTROL_PROTO_CMD_SET_RELAY_ALL))
  {
    memset(result, 0, sizeof(*result));
    result->error = CONTROL_PROTO_ERR_NONE;
    if (control_queue_fetch_remote_relay_mask(ip_addr, result) == HAL_OK)
    {
      return HAL_OK;
    }
    return HAL_OK;
  }

  return HAL_ERROR;
}

static void control_queue_worker_task(void const *argument)
{
  control_queue_request_t request;
  relay_domain_result_t async_result;

  (void)argument;

  for (;;)
  {
    if (xQueueReceive(g_control_queue_handle, &request, portMAX_DELAY) == pdPASS)
    {
      if (request.remote != 0U)
      {
        relay_domain_result_t *target_result = (request.result != NULL) ? request.result : &async_result;
        HAL_StatusTypeDef remote_status;

        if (request.result != NULL)
        {
          remote_status = control_queue_execute_remote(request.remote_ip_addr, &request.command, target_result);
        }
        else
        {
          remote_status = control_queue_execute_remote_async_fast(request.remote_ip_addr, &request.command);
        }

        if (remote_status != HAL_OK)
        {
          if (request.result != NULL)
          {
            memset(request.result, 0, sizeof(*request.result));
            request.result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
          }
          g_control_queue_stats.dropped++;
        }
        else
        {
          if ((request.result == NULL) && (request.remote_node_id != 0U))
          {
            (void)cluster_service_apply_remote_command(request.remote_node_id, &request.command);
          }
          g_control_queue_stats.executed++;
        }
      }
      else
      {
        relay_domain_execute(&request.command, request.result);
        if ((request.result != NULL) &&
            (request.result->error == CONTROL_PROTO_ERR_NONE) &&
            ((request.command.type == CONTROL_PROTO_CMD_SET_RELAY) ||
             (request.command.type == CONTROL_PROTO_CMD_SET_RELAY_MASK) ||
             (request.command.type == CONTROL_PROTO_CMD_SET_RELAY_ALL)))
        {
          device_config_runtime_relay_mask_set(request.result->relay_mask);
        }
        g_control_queue_stats.executed++;
      }

      if (request.waiting_task != NULL)
      {
        xTaskNotifyGive(request.waiting_task);
      }
    }
  }
}

void control_queue_init(void)
{
  static uint8_t initialized = 0U;

  if (initialized != 0U)
  {
    return;
  }

  memset(&g_control_queue_stats, 0, sizeof(g_control_queue_stats));
  g_control_queue_handle = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_queue_request_t));

  if (g_control_queue_handle != NULL)
  {
    osThreadDef(ControlQueueWorker, control_queue_worker_task, osPriorityAboveNormal, 0, 256);
    g_control_queue_task_handle = osThreadCreate(osThread(ControlQueueWorker), NULL);
  }

  initialized = 1U;
}

HAL_StatusTypeDef control_queue_submit(const control_proto_command_t *command, relay_domain_result_t *result)
{
  control_queue_request_t request;

  if ((command == NULL) || (result == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  request.command = *command;
  request.result = result;
  request.waiting_task = xTaskGetCurrentTaskHandle();
  request.remote_ip_addr = 0U;
  request.remote_node_id = 0U;
  request.remote = 0U;

  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;

  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROL_QUEUE_WAIT_TIMEOUT_MS)) == 0U)
  {
    g_control_queue_stats.dropped++;
    return HAL_TIMEOUT;
  }

  return HAL_OK;
}

HAL_StatusTypeDef control_queue_submit_remote(uint32_t ip_addr,
                                              const control_proto_command_t *command,
                                              relay_domain_result_t *result)
{
  control_queue_request_t request;

  if ((ip_addr == 0U) || (command == NULL) || (result == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  request.command = *command;
  request.result = result;
  request.waiting_task = xTaskGetCurrentTaskHandle();
  request.remote_ip_addr = ip_addr;
  request.remote_node_id = 0U;
  request.remote = 1U;

  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;

  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROL_QUEUE_WAIT_TIMEOUT_MS)) == 0U)
  {
    g_control_queue_stats.dropped++;
    return HAL_TIMEOUT;
  }

  return (result->error == CONTROL_PROTO_ERR_NONE) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef control_queue_submit_remote_async(uint32_t ip_addr,
                                                    uint32_t node_id,
                                                    const control_proto_command_t *command)
{
  control_queue_request_t request;

  if ((ip_addr == 0U) || (command == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  request.command = *command;
  request.result = NULL;
  request.waiting_task = NULL;
  request.remote_ip_addr = ip_addr;
  request.remote_node_id = node_id;
  request.remote = 1U;

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;
  return HAL_OK;
}

void control_queue_poll(void)
{
  control_queue_init();
}

void control_queue_get_stats(control_queue_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_control_queue_stats;
  }
}
