/**
  ******************************************************************************
  * file           : example.c
  * brief          : The controller's application handling a data transfer over
  *                  I3C in a non-blocking mode using DMA and HAL API.
  ******************************************************************************
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "example.h"
#include <string.h> /* importing memcmp and memset functions */
#include "stm32_utils_i3c.h" /* #39: SDK I3C timing helper, added to files.cmake */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/** #39: the I3C peripheral is the controller of a mixed bus whose only target is a legacy
  * 24C128/24C256 class I2C EEPROM (16-bit memory address, 64-byte page), the same module as #38.
  * Unlike the HAL I2C driver, the HAL I3C driver takes the 7-bit address as is.
  */
#define EEPROM_ADDR_FIRST      0x50U
#define EEPROM_ADDR_LAST       0x57U
#define EEPROM_TEST_ADDRESS    0x0100U   /* page 4, outside the #38 test area */
#define PAGE_SIZE              64U
#define I2C_OD_FREQ_HZ         400000U   /* legacy I2C Fm */
#define I3C_PP_FREQ_HZ         12500000U
#define I2C_DUTY_PERCENT       30U       /* same as the generated configuration */
#define TIMEOUT_MS             100U
#define WRITE_CYCLE_LIMIT_MS   10U
#define MONITOR_PERIOD_MS      1000U

#define COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
hal_i3c_handle_t *pI3C; /* pointer referencing the I3C handle from the generated code */

/* Context buffer related to Frame context, contain different buffer value for a communication */
hal_i3c_transfer_ctx_t ContextBuffers;

/* #39: results kept for the debugger as well as the UART log */
uint32_t GeneratedTiming[2]  = {0U};   /* TIMINGR0/1 written by the generated code (1 MHz) */
uint32_t UtilsTiming[2]      = {0U};   /* raw result of STM32_UTILS_I3C_CTRL_ComputeTiming() */
uint32_t AppliedTiming[2]    = {0U};   /* values read back after HAL_I3C_CTRL_SetConfig() */
uint8_t  EepromAddr7         = 0U;
uint32_t WriteCycleMs        = 0U;
uint32_t MonitorOk           = 0U;
uint32_t MonitorFail         = 0U;
uint32_t Reinits             = 0U;

uint8_t  TxBuffer[2U + PAGE_SIZE];     /* 16-bit memory address followed by one page */
uint8_t  RxBuffer[PAGE_SIZE];
uint32_t aControlBuffer[4] = {0U};     /* one control word per descriptor (private, no defining byte) */

/* Private functions prototype -----------------------------------------------*/
static hal_status_t I3cStart(void);
static hal_status_t PrepareTiming(void);
static void ReportError(const char *what, hal_status_t hal_status);
static hal_status_t EepromWritePage(uint16_t mem_addr, const uint8_t *p_data);
static hal_status_t EepromRead(uint16_t mem_addr, uint8_t *p_data, uint32_t size_byte, hal_i3c_transfer_mode_t mode);
static hal_status_t WaitReady(uint32_t limit_ms, uint32_t *p_elapsed_ms);

/** ########## Step 1 ##########
  * The I3C instance is initialized by the generated code (1 MHz legacy I2C timing).
  * #39: recompute TIMINGR0/1 for 400 kHz with the SDK helper, check the FREE field, apply and read back.
  */
app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  pI3C = mx_example_i3c_init();
  if (pI3C == NULL)
  {
    goto _app_init_exit;
  }
  GeneratedTiming[0] = I3C1->TIMINGR0;
  GeneratedTiming[1] = I3C1->TIMINGR1;
  PRINTF("[INFO] Step 1: generated TIMINGR0 = 0x%08" PRIX32 ", TIMINGR1 = 0x%08" PRIX32 " (1 MHz mixed bus)\n",
         GeneratedTiming[0], GeneratedTiming[1]);

  if (PrepareTiming() != HAL_OK)
  {
    goto _app_init_exit;
  }
  PRINTF("[INFO] Step 1: Device initialization COMPLETED.\n");
  return_status = EXEC_STATUS_INIT_OK;

_app_init_exit:
  return return_status;
} /* end app_init */


/** First call: steps 2 to 5 (scan, page write, read back, arbitration header test).
  * Next calls (main.c loops every 10 ms): step 6, read one byte every MONITOR_PERIOD_MS, never write again.
  */
app_status_t app_process(void)
{
  static uint32_t first_done = 0U;
  static uint32_t last_tick = 0U;
  hal_status_t hal_status;
  uint32_t elapsed_ms;

  if (first_done != 0U)
  {
    /**
      * ########## Step 6 (#39) ##########
      * Monitor: unplug and replug SDA, each failure is reported, the bus is restarted if needed.
      */
    if ((HAL_GetTick() - last_tick) >= MONITOR_PERIOD_MS)
    {
      last_tick = HAL_GetTick();
      hal_status = EepromRead(EEPROM_TEST_ADDRESS, RxBuffer, 1U, HAL_I2C_PRIVATE_WITHOUT_ARB_RESTART);
      if (hal_status == HAL_OK)
      {
        MonitorOk++;
        PRINTF("[INFO] Step 6: OK %02" PRIX8 " (ok %" PRIu32 ", fail %" PRIu32 ", reinit %" PRIu32 ")\n",
               RxBuffer[0], MonitorOk, MonitorFail, Reinits);
      }
      else
      {
        MonitorFail++;
        ReportError("Step 6 read", hal_status);
        (void)I3cStart();
      }
    }
    return EXEC_STATUS_OK;
  }
  first_done = 1U;
  last_tick = HAL_GetTick();

  /**
    * ########## Step 2 (#39) ##########
    * Look for the EEPROM: address-only legacy I2C messages, retried by the HAL until ACK or timeout.
    */
  for (uint32_t addr7 = EEPROM_ADDR_FIRST; addr7 <= EEPROM_ADDR_LAST; addr7++)
  {
    if (HAL_I3C_CTRL_PoolForDeviceI2cReady(pI3C, (uint8_t)addr7, 2U) == HAL_OK)
    {
      EepromAddr7 = (uint8_t)addr7;
      break;
    }
  }
  if (EepromAddr7 == 0U)
  {
    PRINTF("[ERROR] Step 2: no ACK in 0x50..0x57. Check VCC, GND, SDA, SCL, A0..A2.\n");
    return EXEC_STATUS_ERROR;
  }
  PRINTF("[INFO] Step 2: EEPROM at 7-bit 0x%02" PRIX32 " (HAL I3C takes it unshifted).\n", (uint32_t)EepromAddr7);

  /**
    * ########## Step 3 (#39) ##########
    * One legacy I2C write message: 2 address bytes + 64 data bytes, then ACK polling with an upper bound.
    */
  for (uint32_t i = 0U; i < PAGE_SIZE; i++)
  {
    TxBuffer[2U + i] = (uint8_t)(0x80U + i);
  }
  hal_status = EepromWritePage(EEPROM_TEST_ADDRESS, &TxBuffer[2U]);
  if (hal_status != HAL_OK)
  {
    ReportError("Step 3 write", hal_status);
    return EXEC_STATUS_ERROR;
  }
  if (WaitReady(WRITE_CYCLE_LIMIT_MS, &elapsed_ms) != HAL_OK)
  {
    PRINTF("[ERROR] Step 3: no ACK %" PRIu32 " ms after the page write.\n", elapsed_ms);
    return EXEC_STATUS_ERROR;
  }
  WriteCycleMs = elapsed_ms;
  PRINTF("[INFO] Step 3: page written at 0x%04" PRIX32 ", write cycle %" PRIu32 " ms (1 ms tick).\n",
         (uint32_t)EEPROM_TEST_ADDRESS, WriteCycleMs);

  /**
    * ########## Step 4 (#39) ##########
    * Two messages in one frame: write the 2 address bytes, repeated start, read 64 bytes.
    */
  memset(RxBuffer, 0, sizeof(RxBuffer));
  hal_status = EepromRead(EEPROM_TEST_ADDRESS, RxBuffer, PAGE_SIZE, HAL_I2C_PRIVATE_WITHOUT_ARB_RESTART);
  if (hal_status != HAL_OK)
  {
    ReportError("Step 4 read", hal_status);
    return EXEC_STATUS_ERROR;
  }
  if (memcmp(&TxBuffer[2U], RxBuffer, PAGE_SIZE) != 0)
  {
    PRINTF("[ERROR] Step 4: Data MISMATCH between written and read buffers.\n");
    return EXEC_STATUS_ERROR;
  }
  PRINTF("[INFO] Step 4: %" PRIu32 " bytes read back, IDENTICAL.\n", (uint32_t)PAGE_SIZE);

  /**
    * ########## Step 5 (#39) ##########
    * Same read with the arbitrable header 0x7E enabled. No I3C target acknowledges 0x7E on this bus,
    * RM0522 Table 434 lists that case as CE2. Expected: error, then the bus is restarted.
    */
  hal_status = EepromRead(EEPROM_TEST_ADDRESS, RxBuffer, 1U, HAL_I2C_PRIVATE_WITH_ARB_RESTART);
  if (hal_status == HAL_OK)
  {
    PRINTF("[INFO] Step 5: read WITH 0x7E header succeeded (not the expected CE2).\n");
  }
  else
  {
    ReportError("Step 5 read with 0x7E header", hal_status);
    (void)I3cStart();
  }
  PRINTF("[INFO] Step 6: monitoring every %" PRIu32 " ms, unplug and replug SDA now.\n", (uint32_t)MONITOR_PERIOD_MS);

  return EXEC_STATUS_OK;
} /* end app_process */


/** De-initializes the I3C and DMA instances before leaving the scenario.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_example_i3c_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */


/** brief:  computes the 400 kHz mixed-bus timing with the SDK helper and applies it.
  *         RM0522 I3C_TIMINGR1: FREE is 7 bits [22:16], bits 27:23 are reserved. The helper only checks
  *         its results against 8 bits, so a FREE value above 0x7F is clamped here and reported.
  * retval: HAL_OK or HAL_ERROR
  */
static hal_status_t PrepareTiming(void)
{
  stm32_utils_i3c_ctrl_timing_config_t timing_cfg;
  hal_i3c_ctrl_config_t ctrl_cfg;
  uint32_t free_cycles;

  timing_cfg.clock_src_freq_hz  = HAL_RCC_I3C1_GetKernelClkFreq();
  timing_cfg.i3c_pp_freq_hz     = I3C_PP_FREQ_HZ;
  timing_cfg.i2c_od_freq_hz     = I2C_OD_FREQ_HZ;
  timing_cfg.duty_cycle_purcent = I2C_DUTY_PERCENT;
  timing_cfg.wait_time          = STM32_UTILS_I3C_ACTIVITY_STATE_0;
  timing_cfg.bus_type           = STM32_UTILS_I3C_I2C_MIXED_BUS;
  if (STM32_UTILS_I3C_CTRL_ComputeTiming(&timing_cfg, &UtilsTiming[0], &UtilsTiming[1]) != STM32_UTILS_I3C_OK)
  {
    PRINTF("[ERROR] Step 1: timing helper failed for %" PRIu32 " Hz at %" PRIu32 " Hz kernel clock.\n",
           (uint32_t)I2C_OD_FREQ_HZ, timing_cfg.clock_src_freq_hz);
    return HAL_ERROR;
  }
  free_cycles = UtilsTiming[1] >> I3C_TIMINGR1_FREE_Pos;   /* helper result, may be wider than 7 bits */
  free_cycles &= 0xFFU;
  PRINTF("[INFO] Step 1: helper %" PRIu32 " Hz: TIMINGR0 = 0x%08" PRIX32 ", TIMINGR1 = 0x%08" PRIX32
         " (FREE = %" PRIu32 ")\n", (uint32_t)I2C_OD_FREQ_HZ, UtilsTiming[0], UtilsTiming[1], free_cycles);

  ctrl_cfg.timing_reg0 = UtilsTiming[0];
  ctrl_cfg.timing_reg1 = UtilsTiming[1];
  if (free_cycles > (I3C_TIMINGR1_FREE_Msk >> I3C_TIMINGR1_FREE_Pos))
  {
    /* Clamp: tBUF = [(FREE + 1) x 2 - (0.5 + SDA_HD)] x tI3CCLK, 127 still gives > 1.3 us at 144 MHz */
    ctrl_cfg.timing_reg1 = (UtilsTiming[1] & ~(0xFFUL << I3C_TIMINGR1_FREE_Pos)) | I3C_TIMINGR1_FREE_Msk;
    PRINTF("[WARN] Step 1: FREE %" PRIu32 " does not fit FREE[6:0], clamped to 127.\n", free_cycles);
  }
  if (HAL_I3C_CTRL_SetConfig(pI3C, &ctrl_cfg) != HAL_OK)
  {
    return HAL_ERROR;
  }
  AppliedTiming[0] = I3C1->TIMINGR0;
  AppliedTiming[1] = I3C1->TIMINGR1;
  PRINTF("[INFO] Step 1: applied TIMINGR0 = 0x%08" PRIX32 ", TIMINGR1 = 0x%08" PRIX32 "\n",
         AppliedTiming[0], AppliedTiming[1]);
  return HAL_OK;
}


/** brief:  (re)initializes I3C1 and applies the 400 kHz timing; used after a failed transfer
  */
static hal_status_t I3cStart(void)
{
  Reinits++;
  mx_example_i3c_deinit();
  pI3C = mx_example_i3c_init();
  if (pI3C == NULL)
  {
    return HAL_ERROR;
  }
  return PrepareTiming();
}


/** brief:  prints a failure with the I3C error codes decoded (RM0522 Table 434, I3C_SER)
  */
static void ReportError(const char *what, hal_status_t hal_status)
{
  uint32_t err = HAL_I3C_GetLastErrorCodes(pI3C);

  PRINTF("[ERROR] %s: hal_status = 0x%08" PRIX32 ", errors = 0x%08" PRIX32 "%s%s", what, (uint32_t)hal_status, err,
         ((err & HAL_I3C_ERROR_ADDRESS_NACK) != 0U) ? " ANACK" : "",
         ((err & HAL_I3C_ERROR_DATA_NACK) != 0U) ? " DNACK" : "");
  if ((err & I3C_SER_PERR) != 0U)
  {
    PRINTF(" CE%" PRIu32, err & I3C_SER_CODERR_Msk);
  }
  PRINTF("%s\n", (hal_status == HAL_TIMEOUT) ? " TIMEOUT" : "");
}


/** brief:  builds a transfer context made of the given descriptors and runs it in blocking mode
  */
static hal_status_t RunFrame(const hal_i3c_private_desc_t *p_desc, uint32_t nb_desc, hal_i3c_transfer_mode_t mode,
                             const uint8_t *p_tx, uint32_t tx_size, uint8_t *p_rx, uint32_t rx_size)
{
  if ((HAL_I3C_CTRL_ResetTransferCtx(&ContextBuffers) != HAL_OK)
      || (HAL_I3C_CTRL_InitTransferCtxTc(&ContextBuffers, aControlBuffer,
                                         HAL_I3C_GET_CTRL_BUFFER_SIZE_WORD(nb_desc, mode)) != HAL_OK)
      || ((tx_size != 0U) && (HAL_I3C_CTRL_InitTransferCtxTx(&ContextBuffers, p_tx, tx_size) != HAL_OK))
      || ((rx_size != 0U) && (HAL_I3C_CTRL_InitTransferCtxRx(&ContextBuffers, p_rx, rx_size) != HAL_OK))
      || (HAL_I3C_CTRL_BuildTransferCtxPrivate(&ContextBuffers, p_desc, nb_desc, mode) != HAL_OK))
  {
    return HAL_ERROR;
  }
  return HAL_I3C_CTRL_Transfer(pI3C, &ContextBuffers, TIMEOUT_MS);
}


/** brief:  one legacy I2C write message: address high, address low, 64 data bytes, stop
  */
static hal_status_t EepromWritePage(uint16_t mem_addr, const uint8_t *p_data)
{
  hal_i3c_private_desc_t desc[1] = {{EepromAddr7, 2U + PAGE_SIZE, HAL_I3C_DIRECTION_WRITE}};

  TxBuffer[0] = (uint8_t)(mem_addr >> 8);
  TxBuffer[1] = (uint8_t)(mem_addr & 0xFFU);
  if (p_data != &TxBuffer[2U])
  {
    memcpy(&TxBuffer[2U], p_data, PAGE_SIZE);
  }
  return RunFrame(desc, COUNTOF(desc), HAL_I2C_PRIVATE_WITHOUT_ARB_STOP, TxBuffer, 2U + PAGE_SIZE, NULL, 0U);
}


/** brief:  random read: write the 2 address bytes, repeated start, read size_byte bytes, stop
  */
static hal_status_t EepromRead(uint16_t mem_addr, uint8_t *p_data, uint32_t size_byte, hal_i3c_transfer_mode_t mode)
{
  static uint8_t addr_bytes[2];
  hal_i3c_private_desc_t desc[2] =
  {
    {EepromAddr7, 2U, HAL_I3C_DIRECTION_WRITE},
    {EepromAddr7, size_byte, HAL_I3C_DIRECTION_READ}
  };

  addr_bytes[0] = (uint8_t)(mem_addr >> 8);
  addr_bytes[1] = (uint8_t)(mem_addr & 0xFFU);
  return RunFrame(desc, COUNTOF(desc), mode, addr_bytes, 2U, p_data, size_byte);
}


/** brief:  ACK polling bounded by limit_ms. HAL_I3C_CTRL_PoolForDeviceI2cReady() retries by itself
  *         until its timeout, unlike HAL_I2C_MASTER_PollForSlaveReady() (single trial, #38).
  */
static hal_status_t WaitReady(uint32_t limit_ms, uint32_t *p_elapsed_ms)
{
  uint32_t tstart = HAL_GetTick();
  hal_status_t hal_status = HAL_I3C_CTRL_PoolForDeviceI2cReady(pI3C, EepromAddr7, limit_ms);

  *p_elapsed_ms = HAL_GetTick() - tstart;
  return hal_status;
}
