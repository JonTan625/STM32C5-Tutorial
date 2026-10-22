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
#include <string.h>

/* C5 #25 综合②：掉电不丢的事件记录器（#20 RTC + #21 IWDG + #22 Flash 记录）
 * - 开机：打印 Flash 里的全部记录，再追加一条 BOOT 记录（带复位原因和 RTC 时间）；
 * - B1 短按：追加一条 B1 记录；
 * - B1 按住 3 s：模拟死机，停止喂狗，约 2 s 后 IWDG 复位，下次开机的 BOOT 记录里能看到 IWDG；
 * - 上电时按住 B1：清空记录页。
 * 每条记录 16 字节 = 一个 128 位 Flash 字，只追加不改写；RTC 由 LSE 驱动，复位后接着走。
 */

/* Private define ------------------------------------------------------------*/
#define FLASH_OPERATION_TIMEOUT  3000U
#define LOG_PAGE_ADDR            (FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE) /* C542RC：0x0803E000 */
#define LOG_SLOT_NUM             (FLASH_PAGE_SIZE / sizeof(log_record_t))     /* 512 条 */
#define LOG_MAGIC                0xC5250000UL
#define EVT_BOOT                 1U
#define EVT_B1                   2U
#define IWDG_MAX_TIME_MS         2000U
#define LOOP_MS                  20U
#define HANG_HOLD_MS             3000U
#define BUTTON_PORT              HAL_GPIOC
#define BUTTON_PIN               HAL_GPIO_PIN_13

/* Private typedef -----------------------------------------------------------*/
typedef struct
{
  uint32_t tag;    /* LOG_MAGIC | 事件 << 8 | 附加字节（BOOT：RCC_RSR >> 24） */
  uint32_t seq;
  uint32_t when;   /* 打包的 RTC 时间：年-2000(6) 月(4) 日(5) 时(5) 分(6) 秒(6) */
  uint32_t check;  /* ~(tag ^ seq ^ when) */
} log_record_t;

/* Private variables ---------------------------------------------------------*/
hal_iwdg_handle_t *pIWDG;
hal_flash_handle_t *pFLASH;
uint32_t g_reset_flags;
uint32_t g_slot_used;
uint32_t g_slot_bad;
uint32_t g_seq;
uint32_t g_calendar_kept;
static uint8_t s_domain_was_ready;

/* Private functions prototype -----------------------------------------------*/
static app_status_t rtc_start(void);
static uint32_t rtc_now_packed(void);
static void print_record(uint32_t idx, const log_record_t *rec);
static void log_dump(void);
static app_status_t log_erase_page(void);
static app_status_t log_append(uint32_t event, uint32_t detail);
static app_status_t button_init(void);
static uint8_t button_pressed(void);
static uint32_t bin2bcd(uint32_t v);
static uint32_t bcd2bin(uint32_t v);

app_status_t app_init(void)
{
  g_reset_flags = HAL_RCC_GetResetSource();
  HAL_RCC_ClearResetFlags();

  pFLASH = mx_example_flash_init();
  if ((pFLASH == NULL) || (button_init() != EXEC_STATUS_OK) || (rtc_start() != EXEC_STATUS_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  HAL_FLASH_ITF_Unlock(HAL_FLASH_GetInstance(pFLASH));

  if ((button_pressed() != 0U) && ((g_reset_flags & HAL_RCC_RESET_FLAG_IWDG) == 0U))
  {
    PRINTF("[C5#25] B1 held at power-up: erase log\n");
    if (log_erase_page() != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
    while (button_pressed() != 0U)
    {
    }
  }

  PRINTF("[C5#25] calendar %s, log page 0x%08" PRIX32 "\n", (g_calendar_kept != 0U) ? "kept" : "set from build time",
         (uint32_t)LOG_PAGE_ADDR);
  log_dump();
  if (log_append(EVT_BOOT, g_reset_flags >> 24) != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  print_record(g_slot_used - 1U, &((const log_record_t *)LOG_PAGE_ADDR)[g_slot_used - 1U]);

  /* 长按导致 IWDG 复位时，B1 可能还未松开：不擦页，等松手再开始下一轮 */
  if ((g_reset_flags & HAL_RCC_RESET_FLAG_IWDG) != 0U)
  {
    while (button_pressed() != 0U)
    {
    }
  }

  /* 最后才启动看门狗：前面的擦页、打印都可能比较慢 */
  pIWDG = mx_example_iwdg_init();
  if ((pIWDG == NULL) || (HAL_IWDG_Start(pIWDG, 0U, IWDG_MAX_TIME_MS, 0U) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  uint32_t held_ms = 0U;

  while (1)
  {
    HAL_Delay(LOOP_MS);
    if (button_pressed() != 0U)
    {
      held_ms += LOOP_MS;
      if (held_ms == HANG_HOLD_MS)
      {
        PRINTF("[C5#25] B1 held 3 s: stop feeding the watchdog\n");
        while (1)
        {
          /* 模拟死机 */
        }
      }
    }
    else
    {
      if (held_ms >= LOOP_MS)  /* 松手：算一次短按 */
      {
        if (log_append(EVT_B1, 0U) != EXEC_STATUS_OK)
        {
          return EXEC_STATUS_ERROR;
        }
        print_record(g_slot_used - 1U, &((const log_record_t *)LOG_PAGE_ADDR)[g_slot_used - 1U]);
      }
      held_ms = 0U;
    }
    (void)HAL_IWDG_Refresh(pIWDG);
  }
} /* end app_process */

app_status_t app_deinit(void)
{
  return EXEC_STATUS_OK;
} /* end app_deinit */

/* 同 #20：只在 RTC 域没配好时复位它（RCC_RTCCR 只有 RTC 域复位才清） */
system_status_t pre_system_init_hook(void)
{
  s_domain_was_ready = ((LL_RCC_LSE_IsReady() != 0U)
                        && (LL_RCC_GetRTCClockSource() == LL_RCC_RTC_CLKSOURCE_LSE)
                        && (LL_RCC_IsEnabledRTC() != 0U)) ? 1U : 0U;
  if (s_domain_was_ready == 0U)
  {
    HAL_PWR_DisableRTCDomainWriteProtection();
    HAL_RCC_ResetRTCDomain();
  }
  return SYSTEM_OK;
}

static app_status_t rtc_start(void)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__;  /* "Oct 10 2026" */
  const char *t = __TIME__;  /* "12:34:56" */
  hal_rtc_config_t rtc_config;
  hal_rtc_calendar_config_t calendar_config;
  hal_rtc_date_t date;
  hal_rtc_time_t time;
  uint32_t y;
  uint32_t m;
  uint32_t day;

  HAL_PWR_DisableRTCDomainWriteProtection();
  if (s_domain_was_ready == 0U)
  {
    if ((HAL_RCC_LSE_Enable(HAL_RCC_LSE_ON, HAL_RCC_LSE_DRIVE_HIGH) != HAL_OK)
        || (HAL_RCC_RTC_SetKernelClkSource(HAL_RCC_RTC_CLK_SRC_LSE) != HAL_OK))
    {
      return EXEC_STATUS_ERROR;
    }
  }
  HAL_RCC_RTCAPB_EnableClock();
  if (HAL_RCC_RTC_EnableKernelClock() != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  if ((s_domain_was_ready != 0U) && (HAL_RTC_CALENDAR_IsInitialized() == HAL_RTC_CALENDAR_INITIALIZED))
  {
    g_calendar_kept = 1U;
    return EXEC_STATUS_OK;
  }

  /* 第一次：用编译时刻当初值 */
  for (m = 1U; m < 12U; m++)
  {
    const char *p = &months[(m - 1U) * 3U];
    if ((p[0] == d[0]) && (p[1] == d[1]) && (p[2] == d[2]))
    {
      break;
    }
  }
  day = ((d[4] == ' ') ? 0U : (uint32_t)(d[4] - '0')) * 10U + (uint32_t)(d[5] - '0');
  y = (uint32_t)(d[9] - '0') * 10U + (uint32_t)(d[10] - '0');
  {
    /* Sakamoto 算法求星期，0 = 周日 */
    static const uint8_t k[] = {0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U};
    uint32_t yy = 2000U + y - ((m < 3U) ? 1U : 0U);
    uint32_t w = (yy + yy / 4U - yy / 100U + yy / 400U + k[m - 1U] + day) % 7U;
    date.wday = (hal_rtc_weekday_t)((w == 0U) ? 7U : w); /* HAL：周一 = 1 … 周日 = 7 */
  }
  date.mon = (hal_rtc_month_t)bin2bcd(m);  /* 月份枚举是 BCD：十月 = 0x10 */
  date.mday = day;
  date.year = y;
  time.am_pm = HAL_RTC_TIME_FORMAT_AM_24H;
  time.hour = (uint32_t)(t[0] - '0') * 10U + (uint32_t)(t[1] - '0');
  time.min = (uint32_t)(t[3] - '0') * 10U + (uint32_t)(t[4] - '0');
  time.sec = (uint32_t)(t[6] - '0') * 10U + (uint32_t)(t[7] - '0');
  time.subsec = 0U;
  time.millisec = 0U;

  rtc_config.mode = HAL_RTC_MODE_BCD;
  rtc_config.asynch_prediv = 128U - 1U;  /* 32768 / 128 / 256 = 1 Hz，同官方 mx_rtc.c */
  rtc_config.synch_prediv = 256U - 1U;
  calendar_config.hour_format = HAL_RTC_CALENDAR_HOUR_FORMAT_24;
  calendar_config.bypass_shadow_register = HAL_RTC_CALENDAR_SHADOW_REG_KEEP;
  if ((HAL_RTC_DisableWriteProtection() != HAL_OK) || (HAL_RTC_EnterInitMode() != HAL_OK)
      || (HAL_RTC_SetConfig(&rtc_config) != HAL_OK) || (HAL_RTC_CALENDAR_SetConfig(&calendar_config) != HAL_OK)
      || (HAL_RTC_CALENDAR_SetDateTime(&date, &time) != HAL_OK) || (HAL_RTC_ExitInitMode() != HAL_OK)
      || (HAL_RTC_EnableWriteProtection() != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  g_calendar_kept = 0U;
  return EXEC_STATUS_OK;
}

static uint32_t rtc_now_packed(void)
{
  hal_rtc_date_t date;
  hal_rtc_time_t time;

  if (HAL_RTC_CALENDAR_GetDateTime(&date, &time) != HAL_OK)
  {
    return 0U;
  }
  return (date.year << 26) | (bcd2bin((uint32_t)date.mon) << 22) | (date.mday << 17)
         | (time.hour << 12) | (time.min << 6) | time.sec;
}

static void print_record(uint32_t idx, const log_record_t *rec)
{
  uint32_t w = rec->when;
  uint32_t ev = (rec->tag >> 8) & 0xFFUL;

  PRINTF("#%03" PRIu32 " seq=%" PRIu32 " 20%02" PRIu32 "-%02" PRIu32 "-%02" PRIu32 " %02" PRIu32 ":%02" PRIu32
         ":%02" PRIu32 " %s", idx, rec->seq, w >> 26, (w >> 22) & 0xFUL, (w >> 17) & 0x1FUL, (w >> 12) & 0x1FUL,
         (w >> 6) & 0x3FUL, w & 0x3FUL, (ev == EVT_BOOT) ? "BOOT" : ((ev == EVT_B1) ? "B1" : "?"));
  if (ev == EVT_BOOT)
  {
    uint32_t f = (rec->tag & 0xFFUL) << 24;
    PRINTF("%s%s%s%s%s%s", ((f & HAL_RCC_RESET_FLAG_PIN) != 0U) ? " PIN" : "",
           ((f & HAL_RCC_RESET_FLAG_PWR) != 0U) ? " POR/BOR" : "", ((f & HAL_RCC_RESET_FLAG_SW) != 0U) ? " SW" : "",
           ((f & HAL_RCC_RESET_FLAG_IWDG) != 0U) ? " IWDG" : "", ((f & HAL_RCC_RESET_FLAG_WWDG) != 0U) ? " WWDG" : "",
           ((f & HAL_RCC_RESET_FLAG_LPWR) != 0U) ? " LPWR" : "");
  }
  PRINTF("\n");
}

/* 扫描并打印整页；顺便得到已用条数和最后序号（同 #22） */
static void log_dump(void)
{
  const log_record_t *rec = (const log_record_t *)LOG_PAGE_ADDR;

  g_slot_used = 0U;
  g_slot_bad = 0U;
  for (uint32_t i = 0U; i < LOG_SLOT_NUM; i++)
  {
    if ((rec[i].tag == 0xFFFFFFFFUL) && (rec[i].seq == 0xFFFFFFFFUL)
        && (rec[i].when == 0xFFFFFFFFUL) && (rec[i].check == 0xFFFFFFFFUL))
    {
      break;
    }
    g_slot_used = i + 1U;
    if (((rec[i].tag & 0xFFFF0000UL) != LOG_MAGIC) || (rec[i].check != ~(rec[i].tag ^ rec[i].seq ^ rec[i].when)))
    {
      g_slot_bad++;
      continue;
    }
    g_seq = rec[i].seq;
    print_record(i, &rec[i]);
  }
  PRINTF("[C5#25] %" PRIu32 " records, %" PRIu32 " bad\n", g_slot_used, g_slot_bad);
}

static app_status_t log_erase_page(void)
{
  hal_status_t status;

  HAL_ICACHE_Stop(mx_icache_gethandle());
  status = HAL_FLASH_EraseByAddr(pFLASH, LOG_PAGE_ADDR, FLASH_PAGE_SIZE, FLASH_OPERATION_TIMEOUT);
  HAL_ICACHE_Start(mx_icache_gethandle(), HAL_ICACHE_IT_NONE);
  g_slot_used = 0U;
  return (status == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}

static app_status_t log_append(uint32_t event, uint32_t detail)
{
  log_record_t rec;
  hal_status_t status;

  if (g_slot_used >= LOG_SLOT_NUM)
  {
    if (log_erase_page() != EXEC_STATUS_OK)  /* 单页方案：满了整页重来（#22 第 08 节说明了风险） */
    {
      return EXEC_STATUS_ERROR;
    }
  }
  rec.tag = LOG_MAGIC | ((event & 0xFFUL) << 8) | (detail & 0xFFUL);
  rec.seq = g_seq + 1U;
  rec.when = rtc_now_packed();
  rec.check = ~(rec.tag ^ rec.seq ^ rec.when);

  HAL_ICACHE_Stop(mx_icache_gethandle());
  status = HAL_FLASH_ProgramByAddr(pFLASH, LOG_PAGE_ADDR + (g_slot_used * sizeof(log_record_t)),
                                   (const uint32_t *)&rec, sizeof(rec), FLASH_OPERATION_TIMEOUT);
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

static uint32_t bin2bcd(uint32_t v)
{
  return ((v / 10U) << 4) | (v % 10U);
}

static uint32_t bcd2bin(uint32_t v)
{
  return ((v >> 4) * 10U) + (v & 0x0FUL);
}
