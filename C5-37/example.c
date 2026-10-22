/**
  ******************************************************************************
  * file           : example.c
  * brief          : w25q128j flash memory read/write example
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

#include <string.h>     /* required for memcmp()       */

#include "example.h"

/* Private macro -------------------------------------------------------------*/

/** @user: configure the buffer size, the number of bytes be written to memory
  */
#define BUFFER_SIZE                  4096U

/** @user: configure the start address to write/read from.
  * The W25Q128J uses zero-based addressing.
  * #37: the test uses two 4 KB sectors from this address; their content is lost.
  */
#define W25Q128J_START_ADDRESS       0U

/* #37: W25Q128JV Rev I p61 tPUW = 5 ms max (no write before), p59 tRST = 30 us */
#define FLASH_POWERUP_WAIT_MS        10U
#define FLASH_RESET_WAIT_MS          1U

/* #37: unaligned range used to show the w25q128j_erase_sectors() boundary case */
#define ODD_ERASE_ADDRESS            (W25Q128J_START_ADDRESS + 0x800U)
#define ODD_ERASE_SIZE               0x900U
#define MARK_SIZE                    16U
#define MARK_VALUE                   0xA5U

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
w25q128j_obj_t      *pFlashObj;

uint8_t TxBuffer[BUFFER_SIZE] = {0U}; /* buffer used for write operations */
uint8_t RxBuffer[BUFFER_SIZE] = {0U}; /* buffer used for read operations */

/* #37: results kept for the debugger as well as the UART log */
uint8_t  FlashId[2]     = {0U};       /* 90h: manufacturer, device     */
uint8_t  FlashJedec[3]  = {0U};       /* 9Fh: manufacturer, type, size */
uint8_t  FlashSr[3]     = {0U};       /* status registers 1..3         */
uint32_t FlashSizeByte  = 0U;
uint32_t EraseTimeMs    = 0U;
uint32_t WriteTimeMs    = 0U;
uint32_t ReadTimeMs     = 0U;

/* Private functions prototype -----------------------------------------------*/

static void BufferFill(uint8_t *pbuffer, uint32_t buff_length);
static uint32_t CountNot(const uint8_t *pbuffer, uint32_t buff_length, uint8_t value);
static void PrintSpiConfig(void);
static w25q128j_status_t EraseRange(uint32_t addr, uint32_t size_byte);

/**
  * ########## Step 1 ##########
  * The initialization of the W25Q128J is triggered by the applicative code.
  * #37: print the SPI1 setting, read 90h/9Fh IDs and the status registers.
  */
app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  /* Retrieve and store the w25q128j object pointer */
  pFlashObj = mx_external_mem_getobject();

  PrintSpiConfig();

  /* Give the flash its power-up write-inhibit time before the first command */
  HAL_Delay(FLASH_POWERUP_WAIT_MS);

  /* Part initialization (sends 66h + 99h software reset) */
  if (w25q128j_init(pFlashObj, MX_EXTERNAL_MEM) != W25Q128J_OK)
  {
    PRINTF("[ERR] Step 1: w25q128j_init failed (SPI or CS pin).\n");
    goto _app_init_exit;
  }
  /* The driver returns right after 99h; the device ignores commands for tRST */
  HAL_Delay(FLASH_RESET_WAIT_MS);

  /* Read the device ID */
  if (w25q128j_read_id(pFlashObj, FlashId) != W25Q128J_OK)
  {
    goto _app_init_exit;
  }
  if (w25q128j_read_jedec_id(pFlashObj, FlashJedec) != W25Q128J_OK)
  {
    goto _app_init_exit;
  }
  PRINTF("[INFO] Step 1: 90h ID = %02" PRIX8 " %02" PRIX8 ", 9Fh JEDEC ID = %02" PRIX8 " %02" PRIX8 " %02" PRIX8 "\n",
         FlashId[0], FlashId[1], FlashJedec[0], FlashJedec[1], FlashJedec[2]);

  /* Verify the device ID: any Winbond part, 3-byte address range */
  if ((FlashJedec[0] != 0xEFU) || (FlashJedec[2] < 0x10U) || (FlashJedec[2] > 0x18U))
  {
    if ((FlashJedec[0] == 0x00U) && (FlashJedec[1] == 0x00U))
    {
      /* PA6 has a pull-down (mx_spi1.c): a flash that never answers reads as 00h */
      PRINTF("[ERR] Step 1: ID reads 00h, no answer on MISO: check VCC, GND, CS, SCK, MISO wiring.\n");
    }
    else if ((FlashJedec[0] == 0xFFU) && (FlashJedec[1] == 0xFFU))
    {
      PRINTF("[ERR] Step 1: ID reads FFh, MISO stuck high: check the MISO wire and the module.\n");
    }
    else
    {
      PRINTF("[ERR] Step 1: not a supported Winbond W25Q device.\n");
    }
    goto _app_init_exit;
  }
  FlashSizeByte = 1UL << FlashJedec[2];

  if ((w25q128j_read_status_reg1(pFlashObj, &FlashSr[0]) != W25Q128J_OK)
      || (w25q128j_read_status_reg2(pFlashObj, &FlashSr[1]) != W25Q128J_OK)
      || (w25q128j_read_status_reg3(pFlashObj, &FlashSr[2]) != W25Q128J_OK))
  {
    goto _app_init_exit;
  }
  PRINTF("[INFO] Step 1: size = %" PRIu32 " KB, SR1 = %02" PRIX8 ", SR2 = %02" PRIX8 ", SR3 = %02" PRIX8 "\n",
         FlashSizeByte / 1024U, FlashSr[0], FlashSr[1], FlashSr[2]);
  if ((FlashSr[0] & 0x1CU) != 0U)
  {
    PRINTF("[WARN] Step 1: BP bits set, the test sector may be write-protected.\n");
  }
  PRINTF("[INFO] Step 1: Device initialization COMPLETED.\n");
  return_status = EXEC_STATUS_INIT_OK;

_app_init_exit:
  return return_status;
} /* end app_init */


app_status_t app_process(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;
  uint32_t tstart;
  uint32_t bad;
  uint8_t byte;

  /**
    * ########## Step 2 ##########
    * erase the sector in which we want to write.
    * #37: time it, then read back and count the bytes that are not FFh.
    */

  /* sector erase */
  tstart = HAL_GetTick();
  if (w25q128j_erase_sectors(pFlashObj, W25Q128J_START_ADDRESS, BUFFER_SIZE) != W25Q128J_OK)
  {
    PRINTF("[ERR] Step 2: erase failed or timed out.\n");
    goto _app_process_exit;
  }
  EraseTimeMs = HAL_GetTick() - tstart;

  if (w25q128j_read(pFlashObj, RxBuffer, W25Q128J_START_ADDRESS, BUFFER_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  bad = CountNot(RxBuffer, BUFFER_SIZE, 0xFFU);
  PRINTF("[INFO] Step 2: Sector erased in %" PRIu32 " ms, %" PRIu32 " bytes not FFh.\n", EraseTimeMs, bad);
  if (bad != 0U)
  {
    goto _app_process_exit;
  }

  /**
    * ########## Step 3 ##########
    * Writes a data buffer to the Flash Memory.
    */

  /* Prepare the data to be written to the flash memory */
  BufferFill(TxBuffer, BUFFER_SIZE);

  /* Write to the flash memory (the driver splits it into 16 page programs) */
  tstart = HAL_GetTick();
  if (w25q128j_write(pFlashObj, TxBuffer, W25Q128J_START_ADDRESS, BUFFER_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  WriteTimeMs = HAL_GetTick() - tstart;

  PRINTF("[INFO] Step 3: Data buffer written to the flash memory in %" PRIu32 " ms.\n", WriteTimeMs);

  /**
    * ########## Step 4 ##########
    * Reads the same data buffer from the external memory, and checks that it matches the buffer written in Step 2.
    */

  /* Read from the flash memory */
  tstart = HAL_GetTick();
  if (w25q128j_read(pFlashObj, RxBuffer, W25Q128J_START_ADDRESS, BUFFER_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  ReadTimeMs = HAL_GetTick() - tstart;
  /* Compare the two data buffers */
  if (memcmp(TxBuffer, RxBuffer, BUFFER_SIZE) != 0)
  {
    PRINTF("[ERR] Step 4: read back differs from the written buffer.\n");
    goto _app_process_exit;
  }

  PRINTF("[INFO] Step 4: Data buffer read in %" PRIu32 " ms and matching the initial value.\n", ReadTimeMs);

  /**
    * ########## Step 5 (#37) ##########
    * Program again without erase: offset F0h holds F0h, write 0Fh over it.
    * A program can only turn bits from 1 to 0, so the expected result is F0h & 0Fh = 00h.
    */
  byte = 0x0FU;
  if (w25q128j_write(pFlashObj, &byte, W25Q128J_START_ADDRESS + 0xF0U, 1U) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  if (w25q128j_read(pFlashObj, &byte, W25Q128J_START_ADDRESS + 0xF0U, 1U) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 5: F0h then 0Fh without erase reads %02" PRIX8 "h.\n", byte);

  /**
    * ########## Step 6 (#37) ##########
    * Erase 0x900 bytes from offset 0x800: the range covers sector 0 and sector 1.
    * Mark sector 1 first, then compare w25q128j_erase_sectors() with EraseRange().
    */
  if (w25q128j_erase(pFlashObj, W25Q128J_START_ADDRESS + W25Q128J_SECTOR_SIZE, W25Q128J_ERASE_SECTOR)
      != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  memset(RxBuffer, MARK_VALUE, MARK_SIZE);
  if (w25q128j_write(pFlashObj, RxBuffer, W25Q128J_START_ADDRESS + W25Q128J_SECTOR_SIZE, MARK_SIZE)
      != W25Q128J_OK)
  {
    goto _app_process_exit;
  }

  if (w25q128j_erase_sectors(pFlashObj, ODD_ERASE_ADDRESS, ODD_ERASE_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  if ((w25q128j_read(pFlashObj, RxBuffer, W25Q128J_START_ADDRESS, MARK_SIZE) != W25Q128J_OK)
      || (w25q128j_read(pFlashObj, &RxBuffer[MARK_SIZE], W25Q128J_START_ADDRESS + W25Q128J_SECTOR_SIZE, MARK_SIZE)
          != W25Q128J_OK))
  {
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 6: driver erase: sector 0 head %" PRIu32 "/16 FFh, sector 1 mark %" PRIu32 "/16 FFh.\n",
         MARK_SIZE - CountNot(RxBuffer, MARK_SIZE, 0xFFU),
         MARK_SIZE - CountNot(&RxBuffer[MARK_SIZE], MARK_SIZE, 0xFFU));

  if (EraseRange(ODD_ERASE_ADDRESS, ODD_ERASE_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  if (w25q128j_read(pFlashObj, RxBuffer, W25Q128J_START_ADDRESS + W25Q128J_SECTOR_SIZE, MARK_SIZE) != W25Q128J_OK)
  {
    goto _app_process_exit;
  }
  bad = CountNot(RxBuffer, MARK_SIZE, 0xFFU);
  PRINTF("[INFO] Step 6: EraseRange: sector 1 mark %" PRIu32 "/16 FFh.\n", MARK_SIZE - bad);
  if (bad != 0U)
  {
    goto _app_process_exit;
  }
  return_status = EXEC_STATUS_OK;

_app_process_exit:
  return return_status;
} /* end app_process */


/**
  * ########## Step 7 ##########
  * Deinitializes the W25Q128J before leaving the scenario.
  */
app_status_t app_deinit(void)
{
  if (w25q128j_deinit(pFlashObj) != W25Q128J_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  PRINTF("[INFO] Step 7: Device de-initialization.\n");

  return EXEC_STATUS_OK;
} /* end app_deinit */


/**
  * brief:  fills a buffer with arbitrary values
  * param:  pbuffer: pointer to a buffer to fill
  * param:  buff_length: buffer length
  * retval: None
  */
static void BufferFill(uint8_t *pbuffer, uint32_t buff_length)
{
  /* Fills the buffer with an arbitrary content.
   * Here we just loop over sequential integers (0..255).
   */
  for (uint32_t buff_index = 0; buff_index < buff_length; buff_index++)
  {
    pbuffer[buff_index] = (uint8_t)(buff_index & 0xFF);
  }
}

/**
  * brief:  counts the bytes that differ from a value
  * retval: number of bytes not equal to value
  */
static uint32_t CountNot(const uint8_t *pbuffer, uint32_t buff_length, uint8_t value)
{
  uint32_t count = 0U;

  for (uint32_t buff_index = 0; buff_index < buff_length; buff_index++)
  {
    if (pbuffer[buff_index] != value)
    {
      count++;
    }
  }
  return count;
}

/**
  * brief:  prints the SPI1 fields that matter for the flash (RM0522 45.11.3/45.11.4)
  * retval: None
  */
static void PrintSpiConfig(void)
{
  uint32_t cfg1 = SPI1->CFG1;
  uint32_t cfg2 = SPI1->CFG2;

  PRINTF("[INFO] SPI1 CFG1 = %08" PRIX32 " (MBR = %" PRIu32 "), CFG2 = %08" PRIX32
         " (MASTER %" PRIu32 " CPOL %" PRIu32 " CPHA %" PRIu32 " SSM %" PRIu32 " AFCNTR %" PRIu32 ")\n",
         cfg1, (cfg1 & SPI_CFG1_MBR_Msk) >> SPI_CFG1_MBR_Pos, cfg2,
         (cfg2 & SPI_CFG2_MASTER_Msk) >> SPI_CFG2_MASTER_Pos, (cfg2 & SPI_CFG2_CPOL_Msk) >> SPI_CFG2_CPOL_Pos,
         (cfg2 & SPI_CFG2_CPHA_Msk) >> SPI_CFG2_CPHA_Pos, (cfg2 & SPI_CFG2_SSM_Msk) >> SPI_CFG2_SSM_Pos,
         (cfg2 & SPI_CFG2_AFCNTR_Msk) >> SPI_CFG2_AFCNTR_Pos);
}

/**
  * brief:  erases every 4 KB sector touched by [addr, addr + size_byte)
  * note:   bytes outside the range but inside those sectors are erased too
  * retval: error status
  */
static w25q128j_status_t EraseRange(uint32_t addr, uint32_t size_byte)
{
  uint32_t first;
  uint32_t last;

  if (size_byte == 0U)
  {
    return W25Q128J_OK;
  }
  first = addr & ~(W25Q128J_SECTOR_SIZE - 1U);
  last  = (addr + size_byte - 1U) & ~(W25Q128J_SECTOR_SIZE - 1U);
  for (uint32_t sector = first; sector <= last; sector += W25Q128J_SECTOR_SIZE)
  {
    if (w25q128j_erase(pFlashObj, sector, W25Q128J_ERASE_SECTOR) != W25Q128J_OK)
    {
      return W25Q128J_ERROR;
    }
  }
  return W25Q128J_OK;
}
