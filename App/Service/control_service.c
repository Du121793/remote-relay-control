#include "control_service.h"
#include "control_proto.h"
#include "control_queue.h"
#include "relay_domain.h"
#include "../Platform/board_relay.h"

#include "cmsis_os.h"

#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"

#include <stdio.h>
#include <string.h>

#define CONTROL_TCP_PORT 41000U

extern struct netif gnetif;

static struct tcp_pcb *g_control_listener_pcb = NULL;
static control_service_stats_t g_control_stats;
static uint8_t g_control_service_started = 0U;
static uint8_t g_control_last_link_up = 0U;
static volatile uint8_t g_control_poll_pending = 0U;

static void control_service_tcpip_step(void *arg);
static err_t control_service_sent_close(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t control_service_poll_close(void *arg, struct tcp_pcb *tpcb);
static err_t control_service_poll_idle(void *arg, struct tcp_pcb *tpcb);

static void control_service_close(struct tcp_pcb *tpcb)
{
  err_t err;

  if (tpcb == NULL)
  {
    return;
  }

  tcp_arg(tpcb, NULL);
  tcp_recv(tpcb, NULL);
  tcp_err(tpcb, NULL);
  tcp_poll(tpcb, NULL, 0);
  err = tcp_close(tpcb);
  if (err != ERR_OK)
  {
    tcp_abort(tpcb);
  }
}

static void control_service_close_listener(void)
{
  err_t err;

  if (g_control_listener_pcb == NULL)
  {
    return;
  }

  tcp_arg(g_control_listener_pcb, NULL);
  tcp_accept(g_control_listener_pcb, NULL);
  err = tcp_close(g_control_listener_pcb);
  if (err != ERR_OK)
  {
    tcp_abort(g_control_listener_pcb);
  }
  g_control_listener_pcb = NULL;
}

static err_t control_service_send_response(struct tcp_pcb *tpcb, const char *response)
{
  size_t len = strlen(response);
  err_t err;

  err = tcp_write(tpcb, response, (u16_t)len, TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    g_control_stats.errors++;
    return err;
  }

  err = tcp_output(tpcb);
  if (err != ERR_OK)
  {
    g_control_stats.errors++;
  }

  return err;
}

static void control_service_execute(const control_proto_command_t *command, char *response, size_t response_size)
{
  char ip_text[16];
  relay_domain_result_t result;
  HAL_StatusTypeDef queue_status;

  if (command == NULL)
  {
    (void)control_proto_format_error(response, response_size, CONTROL_PROTO_ERR_BAD_COMMAND);
    return;
  }

  memset(&result, 0, sizeof(result));

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_SET_RELAY:
    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      queue_status = control_queue_submit_async(command);
      if (queue_status != HAL_OK)
      {
        result.error = CONTROL_PROTO_ERR_BAD_COMMAND;
      }
      else
      {
        result.error = CONTROL_PROTO_ERR_NONE;
        result.relay_mask = board_relay_snapshot();
        if (command->type == CONTROL_PROTO_CMD_SET_RELAY)
        {
          if ((command->channel == 0U) ||
              (command->channel > BOARD_RELAY_CHANNEL_COUNT) ||
              (command->value > 1U))
          {
            result.error = CONTROL_PROTO_ERR_BAD_VALUE;
          }
          else if (command->value != 0U)
          {
            result.relay_mask |= (1UL << (command->channel - 1U));
          }
          else
          {
            result.relay_mask &= ~(1UL << (command->channel - 1U));
          }
        }
        else if (command->type == CONTROL_PROTO_CMD_SET_RELAY_MASK)
        {
          result.relay_mask = command->mask;
        }
        else
        {
          result.relay_mask = (command->value != 0U) ? BOARD_RELAY_ALL_MASK : 0U;
        }
      }
      break;

    default:
      relay_domain_execute(command, &result);
      break;
  }

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_PING:
      ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip_text, sizeof(ip_text));
      if (result.error == CONTROL_PROTO_ERR_NONE)
      {
        (void)control_proto_format_pong(response, response_size, ip_text, result.relay_mask);
      }
      else
      {
        g_control_stats.errors++;
        (void)control_proto_format_error(response, response_size, result.error);
      }
      break;

    case CONTROL_PROTO_CMD_GET_RELAY:
    case CONTROL_PROTO_CMD_SET_RELAY:
    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      if (result.error == CONTROL_PROTO_ERR_NONE)
      {
        (void)control_proto_format_relay_state(response, response_size, result.relay_mask);
      }
      else
      {
        g_control_stats.errors++;
        (void)control_proto_format_error(response, response_size, result.error);
      }
      break;

    case CONTROL_PROTO_CMD_INVALID:
    default:
      g_control_stats.errors++;
      (void)control_proto_format_error(response, response_size, command->error);
      break;
  }
}

static err_t control_service_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  char request[96];
  char response[320];
  control_proto_command_t command;
  u16_t copy_len;
  err_t send_err;

  LWIP_UNUSED_ARG(arg);

  if ((err != ERR_OK) || (p == NULL))
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    control_service_close(tpcb);
    return ERR_OK;
  }

  tcp_recved(tpcb, p->tot_len);
  copy_len = (p->tot_len < (sizeof(request) - 1U)) ? p->tot_len : (u16_t)(sizeof(request) - 1U);
  pbuf_copy_partial(p, request, copy_len, 0U);
  request[copy_len] = '\0';
  pbuf_free(p);

  g_control_stats.commands++;
  control_proto_parse(request, &command);
  control_service_execute(&command, response, sizeof(response));
  send_err = control_service_send_response(tpcb, response);
  tcp_recv(tpcb, NULL);
  tcp_sent(tpcb, control_service_sent_close);
  tcp_poll(tpcb, control_service_poll_close, 1);
  if (send_err != ERR_OK)
  {
    control_service_close(tpcb);
    return ERR_OK;
  }
  if ((tcp_sndbuf(tpcb) == TCP_SND_BUF) && (tcp_sndqueuelen(tpcb) == 0U))
  {
    control_service_close(tpcb);
  }
  return ERR_OK;
}

static void control_service_error(void *arg, err_t err)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(err);
  g_control_stats.errors++;
}

static err_t control_service_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  LWIP_UNUSED_ARG(arg);

  if ((err != ERR_OK) || (newpcb == NULL))
  {
    g_control_stats.errors++;
    return ERR_VAL;
  }

  g_control_stats.connections++;
  tcp_arg(newpcb, NULL);
  tcp_recv(newpcb, control_service_recv);
  tcp_err(newpcb, control_service_error);
  tcp_poll(newpcb, control_service_poll_idle, 4);
  return ERR_OK;
}

static err_t control_service_poll_idle(void *arg, struct tcp_pcb *tpcb)
{
  LWIP_UNUSED_ARG(arg);

  g_control_stats.errors++;
  tcp_arg(tpcb, NULL);
  tcp_recv(tpcb, NULL);
  tcp_err(tpcb, NULL);
  tcp_poll(tpcb, NULL, 0);
  tcp_abort(tpcb);
  return ERR_ABRT;
}

static err_t control_service_sent_close(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(len);

  if ((tcp_sndbuf(tpcb) == TCP_SND_BUF) && (tcp_sndqueuelen(tpcb) == 0U))
  {
    control_service_close(tpcb);
  }
  return ERR_OK;
}

static err_t control_service_poll_close(void *arg, struct tcp_pcb *tpcb)
{
  LWIP_UNUSED_ARG(arg);

  if ((tcp_sndbuf(tpcb) == TCP_SND_BUF) && (tcp_sndqueuelen(tpcb) == 0U))
  {
    control_service_close(tpcb);
  }
  return ERR_OK;
}

static void control_service_tcpip_step(void *arg)
{
  uint8_t link_up;

  LWIP_UNUSED_ARG(arg);
  link_up = netif_is_link_up(&gnetif) ? 1U : 0U;

  if (!link_up)
  {
    if (g_control_last_link_up != 0U)
    {
      control_service_close_listener();
    }
    g_control_last_link_up = 0U;
    g_control_poll_pending = 0U;
    return;
  }

  if (g_control_listener_pcb == NULL)
  {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);

    if (pcb != NULL)
    {
      if (tcp_bind(pcb, IP_ANY_TYPE, CONTROL_TCP_PORT) == ERR_OK)
      {
        g_control_listener_pcb = tcp_listen_with_backlog(pcb, 2);
        if (g_control_listener_pcb != NULL)
        {
          tcp_accept(g_control_listener_pcb, control_service_accept);
          g_control_last_link_up = 1U;
        }
        else
        {
          g_control_stats.errors++;
          g_control_poll_pending = 0U;
        }
      }
      else
      {
        g_control_stats.errors++;
        (void)tcp_close(pcb);
        g_control_poll_pending = 0U;
      }
    }
    else
    {
      g_control_stats.errors++;
      g_control_poll_pending = 0U;
    }
  }

  g_control_poll_pending = 0U;
}

void control_service_init(void)
{
  if (g_control_service_started != 0U)
  {
    return;
  }

  g_control_service_started = 1U;
}

void control_service_poll(void)
{
  err_t err;

  control_service_init();
  if (g_control_poll_pending != 0U)
  {
    return;
  }

  g_control_poll_pending = 1U;
  err = tcpip_callback(control_service_tcpip_step, NULL);
  if (err != ERR_OK)
  {
    g_control_poll_pending = 0U;
  }
}

void control_service_get_stats(control_service_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_control_stats;
    stats->listener_up = (g_control_listener_pcb != NULL) ? 1U : 0U;
  }
}
