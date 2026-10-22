/**
  ******************************************************************************
  * file           : example.c
  * brief          : example program body
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
#include "mx_icache.h"

/* C5 #22 改写：把“开机次数”和“B1 按下次数”存进主 Flash 最后一页，断电不丢。
 * - 每条记录 16 字节，正好一个 128 位 Flash 字（RM0522 §6.3.5：ECC 按 128 位算，只写一次）；
 * - 只追加不改写：开机或按 B1 都往后写一条新记录，页写满了才擦整页从头再来；
 * - 擦写前停 ICACHE、写完再开（RM0522 §7.4.5 建议，官方 write_protection 例程同样处理）；
 * - 上电时按住 B1：先擦掉这一页，相当于恢复出厂。
 */

/* Private define ------------------------------------------------------------*/
#define FLASH_OPERATION_TIMEOUT  3000U
#define LOG_PAGE_ADDR            (FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE) /* C542RC：0x0803E000 */
#define LOG_SLOT_NUM             (FLASH_PAGE_SIZE / sizeof(log_record_t))     /* 8192 / 16 = 512 */
#define LOG_MAGIC                0xC5220000UL
#define LOG_TYPE_BOOT            1U
#define LOG_TYPE_B1              2U
#define BUTTON_PORT              HAL_GPIOC
#define BUTTON_PIN               HAL_GPIO_PIN_13
#define CYCLES_PER_US            144U   /* HCLK 144 MHz（mx_rcc.c） */

/* Private typedef -----------------------------------------------------------*/
typedef struct
{
  uint32_t tag;    /* LOG_MAGIC | 类型 */
  uint32_t seq;    /* 记录序号，单调加 1 */
  uint32_t value;  /* BOOT：开机次数；B1：按键次数 */
  uint32_t check;  /* ~(tag ^ seq ^ value)，用来认出写了一半的记录 */
} log_record_t;

/* Private variables ---------------------------------------------------------*/
hal_flash_handle_t *pFLASH;  /* pointer referencing the FLASH handle from the generated code */
uint32_t g_slot_used;        /* 本页已用的记录数 */
uint32_t g_slot_bad;         /* 非空白但校验不过的记录数 */
uint32_t g_seq;              /* 最后一条记录的序号 */
uint32_t g_boot_count;
uint32_t g_press_count;
uint32_t g_erase_us;         /* 最近一次整页擦除用时（DWT） */
uint32_t g_program_us;       /* 最近一次写 16 字节用时（DWT） */

/* Private functions prototype -----------------------------------------------*/
static void log_scan(void);
static app_status_t log_erase_page(void);
static app_status_t log_append(uint32_t type, uint32_t value);
static app_status_t button_init(void);
static uint8_t button_pressed(void);

app_status_t app_init(void)
{
  pFLASH = mx_example_flash_init();
  if (pFLASH == NULL)
  {
    return EXEC_STATUS_ERROR;
  }
  HAL_FLASH_ITF_Unlock(HAL_FLASH_GetInstance(pFLASH)); /* HAL 先查锁状态，不会重复写解锁序列（RM0522 §6.3.5） */

  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;  /* 打开 DWT 周期计数器，用来量擦写时间 */
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

  if (button_init() != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  if (button_pressed() != 0U)
  {
    PRINTF("[C5#22] B1 held at power-up: erase log page\n");
    if (log_erase_page() != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
    while (button_pressed() != 0U)
    {
      /* 等松手，免得马上又记一次按键 */
    }
  }

  log_scan();
  PRINTF("[C5#22] page 0x%08" PRIX32 ": used %" PRIu32 "/%u, bad %" PRIu32 ", last seq %" PRIu32 "\n",
         (uint32_t)LOG_PAGE_ADDR, g_slot_used, (unsigned int)LOG_SLOT_NUM, g_slot_bad, g_seq);

  g_boot_count++;
  if (log_append(LOG_TYPE_BOOT, g_boot_count) != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  PRINTF("[C5#22] boot=%" PRIu32 " presses=%" PRIu32 " (write %" PRIu32 " us, last erase %" PRIu32 " us)\n",
         g_boot_count, g_press_count, g_program_us, g_erase_us);
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  uint8_t last = 0U;

  while (1)
  {
    uint8_t now = button_pressed();
    if ((now != 0U) && (last == 0U))
    {
      g_press_count++;
      if (log_append(LOG_TYPE_B1, g_press_count) != EXEC_STATUS_OK)
      {
        return EXEC_STATUS_ERROR;
      }
      PRINTF("[C5#22] B1 #%" PRIu32 " saved at slot %" PRIu32 " (write %" PRIu32 " us)\n",
             g_press_count, g_slot_used - 1U, g_program_us);
    }
    last = now;
    HAL_Delay(20U); /* 20 ms 采样一次，顺带消抖 */
  }
} /* end app_process */

app_status_t app_deinit(void)
{
  HAL_FLASH_ITF_Lock(HAL_FLASH_GetInstance(pFLASH));
  mx_example_flash_deinit();
  return EXEC_STATUS_OK;
} /* end app_deinit */

/* 从头扫到第一个全 0xFF 的空位；每条有效记录都更新一次计数 */
static void log_scan(void)
{
  const log_record_t *rec = (const log_record_t *)LOG_PAGE_ADDR;

  g_slot_used = 0U;
  g_slot_bad = 0U;
  for (uint32_t i = 0U; i < LOG_SLOT_NUM; i++)
  {
    if ((rec[i].tag == 0xFFFFFFFFUL) && (rec[i].seq == 0xFFFFFFFFUL)
        && (rec[i].value == 0xFFFFFFFFUL) && (rec[i].check == 0xFFFFFFFFUL))
    {
      break;
    }
    g_slot_used = i + 1U;
    if (((rec[i].tag & 0xFFFF0000UL) != LOG_MAGIC) || (rec[i].check != ~(rec[i].tag ^ rec[i].seq ^ rec[i].value)))
    {
      g_slot_bad++; /* 写到一半掉电等情况：跳过，不信它 */
      continue;
    }
    g_seq = rec[i].seq;
    if ((rec[i].tag & 0xFFFFUL) == LOG_TYPE_BOOT)
    {
      g_boot_count = rec[i].value;
    }
    else if ((rec[i].tag & 0xFFFFUL) == LOG_TYPE_B1)
    {
      g_press_count = rec[i].value;
    }
    else
    {
      g_slot_bad++;
    }
  }
}

static app_status_t log_erase_page(void)
{
  hal_status_t status;
  uint32_t t0;

  HAL_ICACHE_Stop(mx_icache_gethandle());
  t0 = DWT->CYCCNT;
  status = HAL_FLASH_EraseByAddr(pFLASH, LOG_PAGE_ADDR, FLASH_PAGE_SIZE, FLASH_OPERATION_TIMEOUT);
  g_erase_us = (DWT->CYCCNT - t0) / CYCLES_PER_US;
  HAL_ICACHE_Start(mx_icache_gethandle(), HAL_ICACHE_IT_NONE);
  g_slot_used = 0U;
  return (status == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}

static app_status_t log_append(uint32_t type, uint32_t value)
{
  log_record_t rec;
  hal_status_t status;
  uint32_t t0;

  if (g_slot_used >= LOG_SLOT_NUM)
  {
    /* 页满：整页擦掉从头写。擦完到写下第一条之间掉电，会丢掉全部历史——
       正式产品用两页轮换（先写新页再擦旧页），这里为了看清机制只用一页 */
    if (log_erase_page() != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
    /* 换页后先保存另一类计数，再写本次事件；避免正常复位也丢失另一类计数。
       此时空页可容纳 512 条，下面的递归只进入一次，不会再次擦页。 */
    if (log_append((type == LOG_TYPE_BOOT) ? LOG_TYPE_B1 : LOG_TYPE_BOOT,
                   (type == LOG_TYPE_BOOT) ? g_press_count : g_boot_count) != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
  }

  rec.tag = LOG_MAGIC | type;
  rec.seq = g_seq + 1U;
  rec.value = value;
  rec.check = ~(rec.tag ^ rec.seq ^ rec.value);

  HAL_ICACHE_Stop(mx_icache_gethandle());
  t0 = DWT->CYCCNT;
  status = HAL_FLASH_ProgramByAddr(pFLASH, LOG_PAGE_ADDR + (g_slot_used * sizeof(log_record_t)),
                                   (const uint32_t *)&rec, sizeof(rec), FLASH_OPERATION_TIMEOUT);
  g_program_us = (DWT->CYCCNT - t0) / CYCLES_PER_US;
  HAL_ICACHE_Start(mx_icache_gethandle(), HAL_ICACHE_IT_NONE);
  if (status != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  g_seq = rec.seq;
  g_slot_used++;
  return EXEC_STATUS_OK;
}

static app_status_t button_init(void)
{
  hal_gpio_config_t gpio_config;

  HAL_RCC_GPIOC_EnableClock();
  gpio_config.mode        = HAL_GPIO_MODE_INPUT;
  gpio_config.pull        = HAL_GPIO_PULL_NO;   /* 板上已有外部下拉（#08） */
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.alternate   = HAL_GPIO_AF_0;
  gpio_config.init_state  = HAL_GPIO_PIN_RESET;
  return (HAL_GPIO_Init(BUTTON_PORT, BUTTON_PIN, &gpio_config) == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}

static uint8_t button_pressed(void)
{
  return (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == HAL_GPIO_PIN_SET) ? 1U : 0U;
}
