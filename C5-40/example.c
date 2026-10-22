/**
  ******************************************************************************
  * file           : example.c
  * brief          : FDCAN internal loopback, timing and filter checks for #40.
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

#define COM_TIMEOUT_MS     100U
#define REJECT_WINDOW_MS    20U
#define ROUND_COUNT         3U
#define CASE_COUNT          5U

typedef struct
{
  uint32_t kernel_hz, nominal_bps, data_bps;
  uint32_t nbtp, dbtp, cccr, test, ckdiv, rxgfc;
} timing_snapshot_t;

hal_fdcan_handle_t *pFDCAN;
volatile timing_snapshot_t g_rounds[ROUND_COUNT];
volatile uint32_t g_round, g_case, g_passed, g_failed, g_fail_code;
volatile uint32_t g_last_hal, g_psr, g_ecr;
volatile hal_fdcan_rx_header_t g_last_rx;
uint8_t g_rx[64];

static const uint32_t prescalers[ROUND_COUNT] = {1U, 2U, 4U};
static uint8_t tx_data[64];

static uint32_t FifoLevel(hal_fdcan_rx_location_t fifo)
{
  uint32_t level = 0U;
  HAL_FDCAN_GetRxFifoFillLevel(pFDCAN, fifo, &level);
  return level;
}

static app_status_t Fail(uint32_t code, hal_status_t status)
{
  g_failed++;
  g_fail_code = code;
  g_last_hal = (uint32_t)status;
  /* PSR部分状态有读副作用，只在出错时抓一次；正常判据不依赖LEC。 */
  g_psr = FDCAN1->PSR;
  g_ecr = FDCAN1->ECR;
  PRINTF("[40] FAIL round=%" PRIu32 " case=%" PRIu32
         " code=%" PRIu32 " hal=0x%08" PRIX32
         " PSR=0x%08" PRIX32 " ECR=0x%08" PRIX32 "\r\n",
         g_round, g_case, code, g_last_hal, g_psr, g_ecr);
  return EXEC_STATUS_ERROR;
}

/* 每次只提交一帧，确保FIFO空闲判据对应当前帧。 */
static hal_status_t WaitTx(void)
{
  uint32_t start = HAL_GetTick();
  while (HAL_FDCAN_GetTxFifoFreeLevel(pFDCAN) != HAL_FDCAN_TX_FIFO_FREE_LEVEL_3)
  {
    if ((uint32_t)(HAL_GetTick() - start) >= COM_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  return HAL_OK;
}

static app_status_t RunCase(uint32_t index)
{
  hal_fdcan_tx_header_t tx = { .d64 = 0U };
  hal_fdcan_rx_header_t rx;
  hal_fdcan_rx_location_t fifo = HAL_FDCAN_RX_FIFO_0;
  uint32_t bytes = (index == 1U || index == 2U) ? 64U : 8U;
  uint32_t rejected = (index >= 3U);
  uint32_t extended = (index == 2U || index == 4U);
  uint32_t start;
  hal_status_t status;

  g_case = index + 1U;
  tx.b.identifier_type = extended ? HAL_FDCAN_ID_EXTENDED : HAL_FDCAN_ID_STANDARD;
  tx.b.identifier = extended ? 0x18000002U : 0x101U;
  if (rejected != 0U)
  {
    tx.b.identifier++; /* 只改ID，保持其余字段，验证精确掩码。 */
  }
  tx.b.frame_type = HAL_FDCAN_FRAME_DATA;
  tx.b.error_state_indicator = HAL_FDCAN_ERROR_STATE_IND_ACTIVE;
  tx.b.data_length = (bytes == 64U) ? HAL_FDCAN_DATA_LEN_FDCAN_64_BYTE
                                   : HAL_FDCAN_DATA_LEN_CAN_FDCAN_8_BYTE;
  tx.b.frame_format = (index == 0U) ? HAL_FDCAN_HEADER_FRAME_FORMAT_CAN
                                   : HAL_FDCAN_HEADER_FRAME_FORMAT_FD_CAN;
  tx.b.bit_rate_switch = (index == 0U) ? HAL_FDCAN_BIT_RATE_SWITCH_OFF
                                      : HAL_FDCAN_BIT_RATE_SWITCH_ON;
  tx.b.event_fifo_control = HAL_FDCAN_TX_EVENTS_FIFO_DISCARD;
  for (uint32_t i = 0; i < sizeof(tx_data); i++)
  {
    tx_data[i] = (uint8_t)(i ^ (g_round << 4U) ^ index);
  }
  if (FifoLevel(HAL_FDCAN_RX_FIFO_0) != 0U || FifoLevel(HAL_FDCAN_RX_FIFO_1) != 0U)
  {
    return Fail(10U, HAL_ERROR);
  }
  status = HAL_FDCAN_ReqTransmitMsgFromFIFOQ(pFDCAN, &tx, tx_data);
  if (status != HAL_OK)
  {
    return Fail(11U, status);
  }
  status = WaitTx();
  if (status != HAL_OK)
  {
    return Fail(12U, status);
  }

  if (rejected != 0U)
  {
    start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < REJECT_WINDOW_MS)
    {
      if (FifoLevel(HAL_FDCAN_RX_FIFO_0) != 0U || FifoLevel(HAL_FDCAN_RX_FIFO_1) != 0U)
      {
        return Fail(13U, HAL_ERROR);
      }
    }
    PRINTF("[40] REJECT id=0x%08" PRIX32 " both_fifo=0 PASS\r\n", (uint32_t)tx.b.identifier);
  }
  else
  {
    if (extended != 0U)
    {
      fifo = HAL_FDCAN_RX_FIFO_1;
    }
    start = HAL_GetTick();
    while (FifoLevel(fifo) == 0U)
    {
      if ((uint32_t)(HAL_GetTick() - start) >= COM_TIMEOUT_MS)
      {
        return Fail(14U, HAL_TIMEOUT);
      }
    }
    /* 64字节缓冲：驱动按收到的DLC复制，必须先有足够容量。 */
    memset(g_rx, 0, sizeof(g_rx));
    status = HAL_FDCAN_GetReceivedMessage(pFDCAN, fifo, &rx, g_rx);
    if (status != HAL_OK)
    {
      return Fail(15U, status);
    }
    g_last_rx.d64 = rx.d64;
    if (rx.b.identifier != tx.b.identifier || rx.b.identifier_type != tx.b.identifier_type
        || rx.b.frame_type != tx.b.frame_type || rx.b.data_length != tx.b.data_length
        || rx.b.frame_format != tx.b.frame_format || rx.b.bit_rate_switch != tx.b.bit_rate_switch
        || rx.b.frame_status != HAL_FDCAN_RX_FRAME_MATCHED || rx.b.filter_index != 0U
        || memcmp(tx_data, g_rx, bytes) != 0
        || FifoLevel(HAL_FDCAN_RX_FIFO_0) != 0U || FifoLevel(HAL_FDCAN_RX_FIFO_1) != 0U)
    {
      return Fail(16U, HAL_ERROR);
    }
    PRINTF("[40] RX fifo=%" PRIu32 " id=0x%08" PRIX32 " len=%" PRIu32
           " FDF=%u BRS=%u PASS\r\n", extended, (uint32_t)rx.b.identifier, bytes,
           (unsigned)rx.b.frame_format, (unsigned)rx.b.bit_rate_switch);
  }
  g_passed++;
  return EXEC_STATUS_OK;
}

app_status_t app_init(void)
{
  pFDCAN = mx_example_fdcan_init();
  if (pFDCAN == NULL)
  {
    PRINTF("[40] INIT FAIL\r\n");
    return EXEC_STATUS_ERROR;
  }
  if (HAL_FDCAN_SetMode(pFDCAN, HAL_FDCAN_MODE_INTERNAL_LOOPBACK) != HAL_OK
      || HAL_FDCAN_DisableEdgeFiltering(pFDCAN) != HAL_OK)
  {
    return Fail(1U, HAL_ERROR);
  }
  if (HAL_FDCAN_GetClockFreq(pFDCAN) != 144000000U
      || HAL_FDCAN_GetClockDivider(pFDCAN) != HAL_FDCAN_CLOCK_DIV_1)
  {
    return Fail(2U, HAL_ERROR);
  }
  PRINTF("[40] INTERNAL LOOPBACK kernel=144000000 CKDIV=1\r\n");
  return EXEC_STATUS_INIT_OK;
}

app_status_t app_process(void)
{
  hal_fdcan_nominal_bit_timing_t nominal;
  hal_fdcan_data_bit_timing_t data;
  HAL_FDCAN_GetNominalBitTiming(pFDCAN, &nominal);
  HAL_FDCAN_GetDataBitTiming(pFDCAN, &data);

  for (uint32_t i = 0; i < ROUND_COUNT; i++)
  {
    g_round = i + 1U;
    g_case = 0U;
    nominal.nominal_prescaler = prescalers[i];
    hal_status_t status = HAL_FDCAN_SetNominalBitTiming(pFDCAN, &nominal);
    if (status != HAL_OK)
    {
      return Fail(3U, status);
    }
    g_rounds[i].kernel_hz = HAL_FDCAN_GetClockFreq(pFDCAN);
    g_rounds[i].nominal_bps = g_rounds[i].kernel_hz
                            / (nominal.nominal_prescaler * (1U + nominal.nominal_time_seg1
                                                           + nominal.nominal_time_seg2));
    g_rounds[i].data_bps = g_rounds[i].kernel_hz
                         / (data.data_prescaler * (1U + data.data_time_seg1 + data.data_time_seg2));
    g_rounds[i].nbtp = FDCAN1->NBTP;
    g_rounds[i].dbtp = FDCAN1->DBTP;
    g_rounds[i].cccr = FDCAN1->CCCR;
    g_rounds[i].test = FDCAN1->TEST;
    g_rounds[i].ckdiv = FDCAN_CONFIG->CKDIV;
    g_rounds[i].rxgfc = FDCAN1->RXGFC;
    PRINTF("[40] ROUND=%" PRIu32 " nominal=%" PRIu32 " data=%" PRIu32
           " NBTP=0x%08" PRIX32 " DBTP=0x%08" PRIX32 "\r\n",
           g_round, g_rounds[i].nominal_bps, g_rounds[i].data_bps,
           g_rounds[i].nbtp, g_rounds[i].dbtp);
    status = HAL_FDCAN_Start(pFDCAN);
    if (status != HAL_OK)
    {
      return Fail(4U, status);
    }
    for (uint32_t j = 0; j < CASE_COUNT; j++)
    {
      if (RunCase(j) == EXEC_STATUS_ERROR)
      {
        (void)HAL_FDCAN_Stop(pFDCAN);
        return EXEC_STATUS_ERROR;
      }
    }
    status = HAL_FDCAN_Stop(pFDCAN);
    if (status != HAL_OK)
    {
      return Fail(5U, status);
    }
  }
  PRINTF("[40] DONE pass=%" PRIu32 " fail=%" PRIu32 "\r\n", g_passed, g_failed);
  return EXEC_STATUS_OK;
}

app_status_t app_deinit(void)
{
  /* 每轮已Stop；保留全局快照供调试器看，沿用main的LED成功提示。 */
  mx_example_fdcan_deinit();
  return EXEC_STATUS_OK;
}
