/**
  ******************************************************************************
  * file           : example.c
  * brief          : #41 two-board FDCAN request/echo, polling and bounded waits.
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  * Teaching adaptation by JonTan, 2026-10-20.
  ******************************************************************************
  */
#include "example.h"
#include <string.h>

#ifndef NODE_CONTROLLER
#error "Define NODE_CONTROLLER=1 for A, or =0 for B in flags.cmake"
#endif
#if NODE_CONTROLLER
#define NODE_LABEL "A"
#define TX_ID 0x101U
#define RX_ID 0x102U
#else
#define NODE_LABEL "B"
#define TX_ID 0x102U
#define RX_ID 0x101U
#endif
#define TX_TIMEOUT_MS       100U
#define REPLY_TIMEOUT_MS   1000U
#define SEND_PAUSE_MS      1000U
#define DATA_BYTES            8U

hal_fdcan_handle_t *pFDCAN;
volatile uint32_t g_seq, g_tx_ok, g_rx_ok, g_exchange_ok, g_waits, g_fault;
volatile uint32_t g_psr, g_ecr, g_cccr, g_nbtp, g_dbtp, g_txbrp;
volatile uint32_t g_tec, g_rec, g_lec, g_dlec, g_ew, g_ep, g_bo;
volatile uint32_t g_pe2, g_abort_status, g_stop_status;
volatile hal_fdcan_rx_header_t g_last_rx;
uint8_t g_rx[64];
static uint8_t tx_data[DATA_BYTES];

static void Snapshot(void)
{
  /* PSR的LEC/DLEC和ECR的CEL有读副作用，每次日志只抓一份。 */
  g_psr = FDCAN1->PSR;
  g_ecr = FDCAN1->ECR;
  g_cccr = FDCAN1->CCCR;
  g_txbrp = FDCAN1->TXBRP;
  g_tec = (g_ecr & FDCAN_ECR_TEC) >> FDCAN_ECR_TEC_Pos;
  g_rec = (g_ecr & FDCAN_ECR_REC) >> FDCAN_ECR_REC_Pos;
  g_lec = (g_psr & FDCAN_PSR_LEC) >> FDCAN_PSR_LEC_Pos;
  g_dlec = (g_psr & FDCAN_PSR_DLEC) >> FDCAN_PSR_DLEC_Pos;
  g_ew = ((g_psr & FDCAN_PSR_EW) != 0U);
  g_ep = ((g_psr & FDCAN_PSR_EP) != 0U);
  g_bo = ((g_psr & FDCAN_PSR_BO) != 0U);
}

static void Report(const char *event)
{
  Snapshot();
  PRINTF("[41][" NODE_LABEL "] %s seq=%" PRIu32 " tx=%" PRIu32 " rx=%" PRIu32
         " TEC=%" PRIu32 " REC=%" PRIu32 " LEC=%" PRIu32 " DLEC=%" PRIu32
         " EW=%" PRIu32 " EP=%" PRIu32 " BO=%" PRIu32 "\r\n",
         event, g_seq, g_tx_ok, g_rx_ok, g_tec, g_rec, g_lec, g_dlec, g_ew, g_ep, g_bo);
}

static app_status_t Fault(uint32_t code, const char *why, hal_status_t status)
{
  g_fault = code;
  Report(why); /* 在Stop前保存故障现场，不再排队重试。 */
  PRINTF("[41][" NODE_LABEL "] HALT code=%" PRIu32 " hal=0x%08" PRIX32
         " CCCR=0x%08" PRIX32 " TXBRP=0x%08" PRIX32 " reset_required\r\n",
         code, (uint32_t)status, g_cccr, g_txbrp);
  g_abort_status = (uint32_t)HAL_OK;
  if (g_txbrp != 0U)
  {
    g_abort_status = (uint32_t)HAL_FDCAN_ReqAbortOfTxBuffer(pFDCAN, g_txbrp);
  }
  g_stop_status = (uint32_t)HAL_FDCAN_Stop(pFDCAN);
  return EXEC_STATUS_ERROR;
}

static uint32_t RxLevel(void)
{
  uint32_t level = 0U;
  HAL_FDCAN_GetRxFifoFillLevel(pFDCAN, HAL_FDCAN_RX_FIFO_0, &level);
  return level;
}

static hal_status_t Transmit(const uint8_t *data)
{
  hal_fdcan_tx_header_t tx = { .d64 = 0U };
  tx.b.identifier = TX_ID;
  tx.b.identifier_type = HAL_FDCAN_ID_STANDARD;
  tx.b.frame_type = HAL_FDCAN_FRAME_DATA;
  tx.b.error_state_indicator = HAL_FDCAN_ERROR_STATE_IND_ACTIVE;
  tx.b.data_length = HAL_FDCAN_DATA_LEN_CAN_FDCAN_8_BYTE;
  tx.b.frame_format = HAL_FDCAN_HEADER_FRAME_FORMAT_FD_CAN;
  tx.b.bit_rate_switch = HAL_FDCAN_BIT_RATE_SWITCH_ON;
  tx.b.event_fifo_control = HAL_FDCAN_TX_EVENTS_FIFO_DISCARD;
  hal_status_t status = HAL_FDCAN_ReqTransmitMsgFromFIFOQ(pFDCAN, &tx, data);
  if (status != HAL_OK)
  {
    return status;
  }
  const uint32_t mask = HAL_FDCAN_GetLatestTxFifoQRequestBuffer(pFDCAN);
  uint32_t start = HAL_GetTick();
  while ((FDCAN1->TXBRP & mask) != 0U)
  {
    if ((uint32_t)(HAL_GetTick() - start) >= TX_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  /* pending清零也可能是取消，必须核对“实际发送成功”标志。 */
  if ((FDCAN1->TXBTO & mask) == 0U)
  {
    return HAL_ERROR;
  }
  g_tx_ok++;
  return HAL_OK;
}

static hal_status_t Receive(void)
{
  uint32_t start = HAL_GetTick();
  while (RxLevel() == 0U)
  {
    if ((uint32_t)(HAL_GetTick() - start) >= REPLY_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  hal_fdcan_rx_header_t rx;
  memset(g_rx, 0, sizeof(g_rx));
  hal_status_t status = HAL_FDCAN_GetReceivedMessage(pFDCAN, HAL_FDCAN_RX_FIFO_0, &rx, g_rx);
  if (status != HAL_OK)
  {
    return status;
  }
  g_last_rx.d64 = rx.d64;
  if (rx.b.identifier != RX_ID || rx.b.identifier_type != HAL_FDCAN_ID_STANDARD
      || rx.b.frame_type != HAL_FDCAN_FRAME_DATA
      || rx.b.data_length != HAL_FDCAN_DATA_LEN_CAN_FDCAN_8_BYTE
      || rx.b.frame_format != HAL_FDCAN_HEADER_FRAME_FORMAT_FD_CAN
      || rx.b.bit_rate_switch != HAL_FDCAN_BIT_RATE_SWITCH_ON
      || rx.b.frame_status != HAL_FDCAN_RX_FRAME_MATCHED || rx.b.filter_index != 0U)
  {
    return HAL_ERROR;
  }
  g_rx_ok++;
  return HAL_OK;
}

app_status_t app_init(void)
{
  pFDCAN = mx_example_fdcan_init();
  if (pFDCAN == NULL)
  {
    PRINTF("[41][" NODE_LABEL "] INIT_FAIL\r\n");
    return EXEC_STATUS_ERROR;
  }
  HAL_RCC_GPIOE_EnableClock();
  hal_gpio_config_t gpio = {0};
  gpio.mode = HAL_GPIO_MODE_OUTPUT;
  gpio.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio.pull = HAL_GPIO_PULL_NO;
  gpio.speed = HAL_GPIO_SPEED_FREQ_LOW;
  gpio.init_state = HAL_GPIO_PIN_RESET;
  if (HAL_GPIO_Init(HAL_GPIOE, HAL_GPIO_PIN_2, &gpio) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  /* 教学上留1ms等待，不将此数写成器件的切换时间规格。 */
  HAL_Delay(1U);
  g_pe2 = HAL_GPIO_ReadPin(HAL_GPIOE, HAL_GPIO_PIN_2);
  hal_fdcan_global_filter_config_t global = {
    .acceptance_non_matching_std = HAL_FDCAN_NO_MATCH_REJECT,
    .acceptance_non_matching_ext = HAL_FDCAN_NO_MATCH_REJECT,
    .acceptance_remote_std = HAL_FDCAN_REMOTE_REJECT,
    .acceptance_remote_ext = HAL_FDCAN_REMOTE_REJECT,
  };
  if (HAL_FDCAN_SetGlobalFilter(pFDCAN, &global) != HAL_OK
      || HAL_FDCAN_DisableEdgeFiltering(pFDCAN) != HAL_OK
      || HAL_FDCAN_GetMode(pFDCAN) != HAL_FDCAN_MODE_NORMAL
      || HAL_FDCAN_GetClockFreq(pFDCAN) != 144000000U)
  {
    PRINTF("[41][" NODE_LABEL "] CONFIG_FAIL\r\n");
    return EXEC_STATUS_ERROR;
  }
  g_nbtp = FDCAN1->NBTP;
  g_dbtp = FDCAN1->DBTP;
  PRINTF("[41][" NODE_LABEL "] NORMAL 500k/2M TX=%03X RX=%03X PE2=%" PRIu32
         " NBTP=%08" PRIX32 " DBTP=%08" PRIX32 "\r\n",
         (unsigned)TX_ID, (unsigned)RX_ID, g_pe2, g_nbtp, g_dbtp);
  hal_status_t status = HAL_FDCAN_Start(pFDCAN);
  if (status != HAL_OK)
  {
    PRINTF("[41][" NODE_LABEL "] START_FAIL hal=0x%08" PRIX32 "\r\n",(uint32_t)status);
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
}

app_status_t app_process(void)
{
  hal_status_t status;
#if NODE_CONTROLLER
  if (g_exchange_ok != 0U)
  {
    HAL_Delay(SEND_PAUSE_MS);
  }
  g_seq++;
  for (uint32_t i = 0; i < 4U; i++)
  {
    tx_data[i] = (uint8_t)(g_seq >> (8U * i));
  }
  tx_data[4] = 0x41U;
  tx_data[5] = 0xA5U;
  tx_data[6] = 0x5AU;
  tx_data[7] = (uint8_t)g_seq ^ 0xA5U;
  status = Transmit(tx_data);
  if (status != HAL_OK)
  {
    return Fault(10U, "TX_FAIL", status);
  }
  PRINTF("[41][A] TX_OK seq=%" PRIu32 " awaiting_echo\r\n",g_seq);
  status = Receive();
  if (status != HAL_OK)
  {
    return Fault(20U, "RX_FAIL", status);
  }
  if (memcmp(tx_data, g_rx, DATA_BYTES) != 0)
  {
    return Fault(21U, "DATA_MISMATCH", HAL_ERROR);
  }
#else
  status = Receive();
  if (status == HAL_TIMEOUT)
  {
    g_waits++;
    PRINTF("[41][B] WAIT count=%" PRIu32 "\r\n",g_waits);
    return EXEC_STATUS_INIT_OK; /* 等待不等于故障，也不算成功收发。 */
  }
  if (status != HAL_OK)
  {
    return Fault(30U, "BAD_REQUEST", status);
  }
  g_seq = (uint32_t)g_rx[0] | ((uint32_t)g_rx[1] << 8U)
          | ((uint32_t)g_rx[2] << 16U) | ((uint32_t)g_rx[3] << 24U);
  memcpy(tx_data, g_rx, DATA_BYTES);
  status = Transmit(tx_data);
  if (status != HAL_OK)
  {
    return Fault(31U, "ECHO_TX_FAIL", status);
  }
#endif
  g_exchange_ok++;
  Report("EXCHANGE_OK");
  return EXEC_STATUS_OK;
}

app_status_t app_deinit(void)
{
  (void)HAL_FDCAN_Stop(pFDCAN);
  mx_example_fdcan_deinit();
  return EXEC_STATUS_OK;
}
