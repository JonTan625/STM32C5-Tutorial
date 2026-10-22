/**
  ******************************************************************************
  * file           : example.c
  * brief          : I2C EEPROM DMA example: write then read the data and check for data consistency
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
#include <string.h>       /* required for memset() and memcmp() */
#include "example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/** 24C128 / 24C256 class EEPROM (M24128-D in the official example)
  * - 16-bit memory address, 64-byte page.
  * - 7-bit device address 1010 E2 E1 E0 = 0x50..0x57; the official example used 0xA2 (0x51 << 1).
  * #37 -> #38: the device address is found by a bus scan instead of being hard-coded.
  */
#define EEPROM_ADDR_FIRST     0x50U
#define EEPROM_ADDR_LAST      0x57U
#define EEPROM_START_ADDRESS  0U
#define BUFFER_SIZE           256U
#define PAGE_SIZE             64U
#define TIMEOUT_MS            100U    /* one blocking or DMA transfer */
#define WRITE_CYCLE_LIMIT_MS  10U     /* give up ACK polling after this */
#define ROLL_OFFSET           60U     /* page 0 offset used by the roll-over test */
#define ROLL_SIZE             8U
#define MONITOR_SECONDS       30U
#define RECOVERY_PULSES       9U

/* @user: The maximum data bus width used by DMA in STM32 devices is 64 bits.
   Therefore, 8-byte alignment is the minimum recommended alignment for DMA buffers across STM32 devices. */
#define DMA_ALIGNMENT      (8U)

/* Private macro -------------------------------------------------------------*/
#define ADDR8(addr7)       ((uint32_t)(addr7) << 1U)   /* HAL takes the 7-bit address shifted left */

/* Private variables ---------------------------------------------------------*/
hal_i2c_handle_t *pI2C;
volatile uint8_t TxTransferComplete; /* Set to 1 if the write transfer is correctly completed */
volatile uint8_t RxTransferComplete; /* Set to 1 if the read transfer is correctly completed */
volatile uint8_t TransferError;      /* Set to 1 if a transmission or a reception error is detected */

/* #38: results kept for the debugger as well as the UART log */
uint8_t  EepromAddr7    = 0U;   /* 7-bit address found by the scan */
uint8_t  ScanCount      = 0U;   /* number of addresses that acknowledged */
uint32_t WriteCycleMaxMs = 0U;  /* longest ACK polling time after a page write */
uint32_t BerrRetries    = 0U;   /* transfers repeated because only BERR was reported */
uint32_t Recoveries     = 0U;   /* bus recoveries (9 SCL pulses + STOP + re-init) */
uint32_t MonitorOk      = 0U;
uint32_t MonitorFail    = 0U;

/** Buffer for CPU and DMA.
  * - Non-cacheable memory for data cache consistency.
  * - Aligned for DMA constraints.
  * - Mandatory with data cache enabled, harmless otherwise: portable across STM32 series.
  */
__attribute__((section(".bss.non_cacheable_area"), aligned(DMA_ALIGNMENT)))
uint8_t TxBuffer[BUFFER_SIZE]; /* Transmit buffer used for EEPROM write operations */
__attribute__((section(".bss.non_cacheable_area"), aligned(DMA_ALIGNMENT)))
uint8_t RxBuffer[BUFFER_SIZE]; /* Receive buffer used for EEPROM read operations */

/* Private functions prototype -----------------------------------------------*/
static void BufferFill(uint8_t *pbuffer, uint32_t buff_length);
static void MasterMemTxTransferCpltCallback(hal_i2c_handle_t *hi2c);
static void MasterMemRxTransferCpltCallback(hal_i2c_handle_t *hi2c);
static void TransferErrorCallback(hal_i2c_handle_t *hi2c);
static hal_status_t I2cStart(void);
static void PrintLines(const char *when);
static void ReportError(const char *what, hal_status_t hal_status, uint32_t i2c_error_code);
static hal_status_t WaitReady(uint32_t limit_ms, uint32_t *p_elapsed_ms);
static hal_status_t MemTransferDMA(uint8_t is_write, uint32_t mem_addr, uint8_t *p_data, uint32_t size_byte);
static hal_status_t BusRecover(void);

/** ########## Step 1 ##########
  * The init of the I2C and DMA instances are triggered by the applicative code.
  * The user callbacks for I2C master TX/RX transfer completed and transfer error are registered.
  * #38: print TIMINGR and the idle level of SCL/SDA (both must read 1 with working pull-ups).
  */
app_status_t app_init(void)
{

  app_status_t return_status = EXEC_STATUS_ERROR;

  if (I2cStart() != HAL_OK)
  {
    goto _app_init_exit;
  }

  PRINTF("[INFO] Step 1: I2C1 TIMINGR = 0x%08" PRIX32 ", CR1 = 0x%08" PRIX32 "\n", I2C1->TIMINGR, I2C1->CR1);
  PrintLines("idle");
  PRINTF("[INFO] Step 1: Device initialization COMPLETED.\n");

  return_status = EXEC_STATUS_INIT_OK;

_app_init_exit:
  return return_status;
} /* end app_init */


/** Writes an amount of data to the EEPROM to be written then read and verified
  */
app_status_t app_process(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;
  hal_status_t hal_status;
  uint32_t elapsed_ms;
  uint32_t page_index;
  uint8_t  roll[ROLL_SIZE];

  /**
    * ########## Step 2 (#38) ##########
    * Scan 0x08..0x77: one address phase per address, a single trial each.
    * The first acknowledging address in 0x50..0x57 is taken as the EEPROM.
    */
  for (uint32_t addr7 = 0x08U; addr7 <= 0x77U; addr7++)
  {
    if (HAL_I2C_MASTER_PollForSlaveReady(pI2C, ADDR8(addr7), 2U) == HAL_OK)
    {
      ScanCount++;
      PRINTF("[INFO] Step 2: ACK at 7-bit 0x%02" PRIX32 " (8-bit write 0x%02" PRIX32 ")\n", addr7, ADDR8(addr7));
      if ((EepromAddr7 == 0U) && (addr7 >= EEPROM_ADDR_FIRST) && (addr7 <= EEPROM_ADDR_LAST))
      {
        EepromAddr7 = (uint8_t)addr7;
      }
    }
    else if ((I2C1->ISR & I2C_ISR_BUSY) != 0U)
    {
      PRINTF("[ERROR] Step 2: bus stays BUSY at 0x%02" PRIX32 ".\n", addr7);
      PrintLines("busy");
      if (BusRecover() != HAL_OK)
      {
        goto _app_process_exit;
      }
    }
  }
  if (EepromAddr7 == 0U)
  {
    PRINTF("[ERROR] Step 2: no ACK in 0x50..0x57 (%" PRIu32 " device(s) seen). Check VCC, GND, SDA, SCL, pull-ups, A0..A2.\n",
           (uint32_t)ScanCount);
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 2: EEPROM at 0x%02" PRIX32 ".\n", (uint32_t)EepromAddr7);

  /**
    * ########## Step 3 ##########
    * Writes a data buffer to the EEPROM memory, in DMA mode, one page per transfer.
    * #38: after each page, ACK polling measures the internal write cycle, bounded by WRITE_CYCLE_LIMIT_MS.
    */
  BufferFill(TxBuffer, BUFFER_SIZE);
  for (page_index = 0U; page_index < (BUFFER_SIZE / PAGE_SIZE); page_index++)
  {
    hal_status = MemTransferDMA(1U, EEPROM_START_ADDRESS + page_index * PAGE_SIZE, &TxBuffer[page_index * PAGE_SIZE],
                                PAGE_SIZE);
    if (hal_status != HAL_OK)
    {
      goto _app_process_exit;
    }
    if (WaitReady(WRITE_CYCLE_LIMIT_MS, &elapsed_ms) != HAL_OK)
    {
      PRINTF("[ERROR] Step 3: no ACK %" PRIu32 " ms after page %" PRIu32 ".\n", elapsed_ms, page_index);
      goto _app_process_exit;
    }
    if (elapsed_ms > WriteCycleMaxMs)
    {
      WriteCycleMaxMs = elapsed_ms;
    }
  }
  PRINTF("[INFO] Step 3: %" PRIu32 " pages written, longest write cycle %" PRIu32 " ms (1 ms tick).\n",
         page_index, WriteCycleMaxMs);

  /**
    * ########## Step 4 ##########
    * Reads the same data buffer from the EEPROM, and checks that it matches the buffer written in Step 3.
    */
  if (MemTransferDMA(0U, EEPROM_START_ADDRESS, RxBuffer, BUFFER_SIZE) != HAL_OK)
  {
    goto _app_process_exit;
  }
  if (memcmp(TxBuffer, RxBuffer, BUFFER_SIZE) != 0)
  {
    PRINTF("[ERROR] Step 4: Data MISMATCH between written and read buffers.\n");
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 4: %" PRIu32 " bytes read back, IDENTICAL.\n", (uint32_t)BUFFER_SIZE);

  /**
    * ########## Step 5 (#38) ##########
    * Page roll-over: write 8 bytes A0h..A7h from offset 60 of page 0 in one transfer.
    * If the address counter wraps inside the page, A4h..A7h land on offsets 0..3 and page 1 is untouched.
    */
  for (uint32_t i = 0U; i < ROLL_SIZE; i++)
  {
    roll[i] = (uint8_t)(0xA0U + i);
  }
  hal_status = HAL_I2C_MASTER_MemWrite(pI2C, ADDR8(EepromAddr7), EEPROM_START_ADDRESS + ROLL_OFFSET,
                                      HAL_I2C_MEM_ADDR_16BIT, roll, ROLL_SIZE, TIMEOUT_MS);
  if ((hal_status != HAL_OK) || (WaitReady(WRITE_CYCLE_LIMIT_MS, &elapsed_ms) != HAL_OK))
  {
    ReportError("Step 5 write", hal_status, HAL_I2C_GetLastErrorCodes(pI2C));
    goto _app_process_exit;
  }
  if (MemTransferDMA(0U, EEPROM_START_ADDRESS, RxBuffer, PAGE_SIZE + 4U) != HAL_OK)
  {
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 5: offset 0..3 = %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8
         ", 60..63 = %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8
         ", 64..67 = %02" PRIX8 " %02" PRIX8 " %02" PRIX8 " %02" PRIX8 "\n",
         RxBuffer[0], RxBuffer[1], RxBuffer[2], RxBuffer[3], RxBuffer[60], RxBuffer[61], RxBuffer[62], RxBuffer[63],
         RxBuffer[64], RxBuffer[65], RxBuffer[66], RxBuffer[67]);
  PRINTF("[INFO] Step 5: %s\n", ((RxBuffer[0] == 0xA4U) && (RxBuffer[64] == TxBuffer[64])) ?
         "wrapped to the start of the same page" : "did NOT wrap as expected");

  /**
    * ########## Step 6 (#38) ##########
    * Hot-unplug monitor: read one byte every second for MONITOR_SECONDS.
    * Pull SDA or the module during this window: every failure must be reported, never hang.
    */
  PRINTF("[INFO] Step 6: monitoring %" PRIu32 " s, unplug and replug SDA now.\n", (uint32_t)MONITOR_SECONDS);
  for (uint32_t second = 0U; second < MONITOR_SECONDS; second++)
  {
    uint8_t value = 0U;

    hal_status = HAL_I2C_MASTER_MemRead(pI2C, ADDR8(EepromAddr7), EEPROM_START_ADDRESS + 5U, HAL_I2C_MEM_ADDR_16BIT,
                                       &value, 1U, TIMEOUT_MS);
    if (hal_status == HAL_OK)
    {
      MonitorOk++;
      PRINTF("[INFO] Step 6: %2" PRIu32 " s OK %02" PRIX8 "\n", second, value);
    }
    else
    {
      MonitorFail++;
      ReportError("Step 6 read", hal_status, HAL_I2C_GetLastErrorCodes(pI2C));
      if ((I2C1->ISR & I2C_ISR_BUSY) != 0U)
      {
        PrintLines("busy");
        (void)BusRecover();
      }
    }
    HAL_Delay(1000U);
  }
  PRINTF("[INFO] Step 6: ok %" PRIu32 ", fail %" PRIu32 ", BERR retries %" PRIu32 ", recoveries %" PRIu32 "\n",
         MonitorOk, MonitorFail, BerrRetries, Recoveries);

  return_status = EXEC_STATUS_OK;

_app_process_exit:
  return return_status;
} /* end app_process */


/**
  * ########## Step 7 ##########
  * De-initializes the I2C and DMA instances before leaving the scenario.
  */
app_status_t app_deinit(void)
{
  mx_example_i2c_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */


/** Master transmission end of transfer callback
  * @user This implementation of the I2C master Tx transfer complete callback can be customized.
  * This function is executed if the master Tx transfer complete interrupt is generated.
  */
static void MasterMemTxTransferCpltCallback(hal_i2c_handle_t *hi2c)
{
  /* Asynchronous processing related to step 3 */
  TxTransferComplete = 1U;
} /* end MasterMemTxTransferCpltCallback */


/** Master reception end of transfer callback
  * @user This implementation of the I2C master Rx transfer complete callback can be customized.
  * This function is executed if the master Rx transfer complete interrupt is generated.
  */
static void MasterMemRxTransferCpltCallback(hal_i2c_handle_t *hi2c)
{
  /* Asynchronous processing related to step 4 */
  RxTransferComplete = 1U;
} /* end MasterMemRxTransferCpltCallback */


/** I2C transfer error callback
  * @user This implementation of the I2C master transfer error callback can be customized.
  * This function is executed if the master transfer error interrupt is generated.
  */
static void TransferErrorCallback(hal_i2c_handle_t *hi2c)
{
  /* Asynchronous processing related to step 3 and step 4 */
  TransferError = 1U;
} /* end TransferErrorCallback */


/** brief:  fills a buffer with values
  * param:  pbuffer: pointer to a buffer to fill
  * param:  buff_length: buffer length
  * retval: None
  */
static void BufferFill(uint8_t *pbuffer, uint32_t buff_length)
{
  uint32_t buff_index;

  /* Fill the buffer with different values */
  for (buff_index = 0; buff_index < buff_length; buff_index++)
  {
    pbuffer[buff_index] = buff_index;
  }
} /* end BufferFill */


/** brief:  initializes I2C1 + DMA and registers the callbacks (also used after a bus recovery)
  * retval: HAL_OK or HAL_ERROR
  */
static hal_status_t I2cStart(void)
{
  pI2C = mx_example_i2c_init();
  if (pI2C == NULL)
  {
    return HAL_ERROR;
  }
  if ((HAL_I2C_MASTER_RegisterMemTxCpltCallback(pI2C, MasterMemTxTransferCpltCallback) != HAL_OK)
      || (HAL_I2C_MASTER_RegisterMemRxCpltCallback(pI2C, MasterMemRxTransferCpltCallback) != HAL_OK)
      || (HAL_I2C_RegisterErrorCallback(pI2C, TransferErrorCallback) != HAL_OK))
  {
    return HAL_ERROR;
  }
  return HAL_OK;
}


/** brief:  prints the SCL (PB6) and SDA (PB7) pin levels; the input data register is readable in AF mode
  */
static void PrintLines(const char *when)
{
  PRINTF("[INFO] lines %s: SCL = %" PRIu32 ", SDA = %" PRIu32 "\n", when,
         (uint32_t)(HAL_GPIO_ReadPin(HAL_GPIOB, PB6_PIN) == HAL_GPIO_PIN_SET),
         (uint32_t)(HAL_GPIO_ReadPin(HAL_GPIOB, PB7_PIN) == HAL_GPIO_PIN_SET));
}


/** brief:  prints a transfer failure with its cause decoded from the HAL error codes
  */
static void ReportError(const char *what, hal_status_t hal_status, uint32_t i2c_error_code)
{
  /* hal_status_t values are patterns (HAL_ERROR = 0xF5F5F5F5, HAL_TIMEOUT = 0x5A5A5A5A), print them in hex */
  PRINTF("[ERROR] %s: hal_status = 0x%08" PRIX32 ", errors = 0x%02" PRIX32 "%s%s%s%s\n", what, (uint32_t)hal_status,
         i2c_error_code,
         ((i2c_error_code & HAL_I2C_ERROR_AF) != 0U) ? " NACK" : "",
         ((i2c_error_code & HAL_I2C_ERROR_BERR) != 0U) ? " BERR" : "",
         ((i2c_error_code & HAL_I2C_ERROR_ARLO) != 0U) ? " ARLO" : "",
         (hal_status == HAL_TIMEOUT) ? " TIMEOUT" : "");
}


/** brief:  ACK polling with an upper bound. One address phase per millisecond tick.
  *         HAL_I2C_MASTER_PollForSlaveReady() returns HAL_ERROR on the first NACK, it does not retry by itself.
  * retval: HAL_OK when the device acknowledged, HAL_TIMEOUT after limit_ms
  */
static hal_status_t WaitReady(uint32_t limit_ms, uint32_t *p_elapsed_ms)
{
  uint32_t tstart = HAL_GetTick();

  do
  {
    if (HAL_I2C_MASTER_PollForSlaveReady(pI2C, ADDR8(EepromAddr7), 2U) == HAL_OK)
    {
      *p_elapsed_ms = HAL_GetTick() - tstart;
      return HAL_OK;
    }
    *p_elapsed_ms = HAL_GetTick() - tstart;
  } while (*p_elapsed_ms <= limit_ms);

  return HAL_TIMEOUT;
}


/** brief:  one Mem write or read in DMA mode with a timeout on the completion callback.
  *         ES0676 2.8.2: BERR can be reported spuriously in controller mode while the transfer is fine,
  *         but the HAL aborts on it, so a transfer whose only error is BERR is started once more.
  * retval: HAL_OK, HAL_ERROR or HAL_TIMEOUT
  */
static hal_status_t MemTransferDMA(uint8_t is_write, uint32_t mem_addr, uint8_t *p_data, uint32_t size_byte)
{
  hal_status_t hal_status = HAL_ERROR;
  uint32_t error_codes = HAL_I2C_ERROR_NONE;

  for (uint32_t attempt = 0U; attempt < 2U; attempt++)
  {
    uint32_t tstart;

    TxTransferComplete = 0U;
    RxTransferComplete = 0U;
    TransferError = 0U;
    hal_status = (is_write != 0U) ?
                 HAL_I2C_MASTER_MemWrite_DMA(pI2C, ADDR8(EepromAddr7), mem_addr, HAL_I2C_MEM_ADDR_16BIT, p_data, size_byte) :
                 HAL_I2C_MASTER_MemRead_DMA(pI2C, ADDR8(EepromAddr7), mem_addr, HAL_I2C_MEM_ADDR_16BIT, p_data, size_byte);
    if (hal_status == HAL_OK)
    {
      tstart = HAL_GetTick();
      while ((TxTransferComplete == 0U) && (RxTransferComplete == 0U) && (TransferError == 0U))
      {
        if ((HAL_GetTick() - tstart) > TIMEOUT_MS)
        {
          hal_status = HAL_TIMEOUT;
          break;
        }
        __WFI();
      }
      if ((hal_status == HAL_OK) && (TransferError != 0U))
      {
        hal_status = HAL_ERROR;
      }
    }
    if (hal_status == HAL_OK)
    {
      return HAL_OK;
    }
    error_codes = HAL_I2C_GetLastErrorCodes(pI2C);
    if ((hal_status == HAL_ERROR) && (error_codes == HAL_I2C_ERROR_BERR) && (attempt == 0U))
    {
      BerrRetries++;
      continue;
    }
    break;
  }

  ReportError((is_write != 0U) ? "DMA write" : "DMA read", hal_status, error_codes);
  if ((hal_status == HAL_TIMEOUT) || ((I2C1->ISR & I2C_ISR_BUSY) != 0U))
  {
    (void)BusRecover();
  }
  return hal_status;
}


/** brief:  bus recovery when a target holds SDA low: release the peripheral, clock SCL as an open-drain GPIO
  *         until SDA reads high (at most 9 pulses, one byte + ACK), send a STOP, then initialize I2C1 again.
  * retval: HAL_OK if SDA is high at the end and I2C1 restarted
  */
static hal_status_t BusRecover(void)
{
  hal_gpio_config_t gpio_config;

  Recoveries++;
  mx_example_i2c_deinit();   /* PE = 0, I2C reset; pins back to reset state */

  gpio_config.mode        = HAL_GPIO_MODE_OUTPUT;
  gpio_config.output_type = HAL_GPIO_OUTPUT_OPENDRAIN;
  gpio_config.pull        = HAL_GPIO_PULL_UP;
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.alternate   = HAL_GPIO_AF_0;
  gpio_config.init_state  = HAL_GPIO_PIN_SET;
  HAL_RCC_GPIOB_EnableClock();
  (void)HAL_GPIO_Init(HAL_GPIOB, PB6_PIN | PB7_PIN, &gpio_config);

  for (uint32_t pulse = 0U; (pulse < RECOVERY_PULSES) && (HAL_GPIO_ReadPin(HAL_GPIOB, PB7_PIN) == HAL_GPIO_PIN_RESET);
       pulse++)
  {
    HAL_GPIO_WritePin(HAL_GPIOB, PB6_PIN, HAL_GPIO_PIN_RESET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(HAL_GPIOB, PB6_PIN, HAL_GPIO_PIN_SET);
    HAL_Delay(1U);
  }

  /* STOP: SDA low while SCL low, SCL high, then SDA high */
  HAL_GPIO_WritePin(HAL_GPIOB, PB6_PIN, HAL_GPIO_PIN_RESET);
  HAL_GPIO_WritePin(HAL_GPIOB, PB7_PIN, HAL_GPIO_PIN_RESET);
  HAL_Delay(1U);
  HAL_GPIO_WritePin(HAL_GPIOB, PB6_PIN, HAL_GPIO_PIN_SET);
  HAL_Delay(1U);
  HAL_GPIO_WritePin(HAL_GPIOB, PB7_PIN, HAL_GPIO_PIN_SET);
  HAL_Delay(1U);
  PrintLines("after recovery");

  HAL_GPIO_DeInit(HAL_GPIOB, PB6_PIN | PB7_PIN);
  if ((I2cStart() != HAL_OK) || (HAL_GPIO_ReadPin(HAL_GPIOB, PB7_PIN) == HAL_GPIO_PIN_RESET))
  {
    PRINTF("[ERROR] bus recovery failed: SDA still low or I2C1 init error.\n");
    return HAL_ERROR;
  }
  PRINTF("[INFO] bus recovered, I2C1 restarted.\n");
  return HAL_OK;
}
