/**
  ******************************************************************************
  * file           : example.c
  * brief          : RTC timestamp with HAL API
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

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
#define PRINT_TIMESTAMP(h, m, s)                                 \
  do                                                             \
  {                                                              \
    PRINTF("[INFO] Step 3: Timestamp  hour=%2" PRIu32 " ", (h)); \
    PRINTF("min=%2" PRIu32 " ", (m));                            \
    PRINTF("sec=%2" PRIu32 "\n", (s));                           \
  } while (0)

#define PRINT_DATESTAMP(mon, mday)                               \
  do                                                             \
  {                                                              \
    PRINTF("[INFO] Step 3: Datestamp  mon=%2d ", (mon));         \
    PRINTF("mday=%2" PRIu32 " \n", (mday));                      \
  } while (0)
/* Private variables ---------------------------------------------------------*/
volatile hal_rtc_time_t RtcTimeStamp; /* Captured timestamp time */
volatile hal_rtc_date_t RtcDateStamp; /* Captured timestamp date */
volatile hal_rtc_timestamp_information_t TimeStampInfo; /* Timestamp event information */

/* C5 #20：复位后不丢时间 */
#define BOOT_MAGIC 0xC5200000UL             /* 高 16 位作标记，低 16 位是启动次数 */
uint32_t g_rsr;                             /* 进入 app_init 时的复位原因标志（RCC_RSR） */
uint32_t g_calendar_kept;                   /* 1：日历沿用复位前的值；0：重新设置 */
uint32_t g_boot_count;                      /* 存在 TAMP_BKP0R，RTC 域复位才清零 */
static uint8_t s_domain_was_ready;          /* pre_system_init_hook 里判断的结果 */

static system_status_t rtc_resume(void);

/* Private functions prototype -----------------------------------------------*/

/** ########## Step 1 ##########
  * The applicative code initializes the RTC instance.
  * The applicative code enables the RTC timestamp IT feature.
  */
app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  g_rsr = HAL_RCC_GetResetSource(); /* 读完就清，下次只剩新的原因 */
  HAL_RCC_ClearResetFlags();

  HAL_RCC_RTCAPB_EnableClock(); /* 先开 APB 时钟才能读 RTC_ICSR 和 TAMP 备份寄存器 */
  if ((s_domain_was_ready != 0U) && (HAL_RTC_CALENDAR_IsInitialized() == HAL_RTC_CALENDAR_INITIALIZED))
  {
    g_calendar_kept = 1U;
    if (rtc_resume() != SYSTEM_OK)
    {
      PRINTF("[ERROR] Step 1: RTC resume ERROR.\n");
      goto _app_process_exit;
    }
  }
  else
  {
    g_calendar_kept = 0U;
    if (mx_example_rtc_init() != SYSTEM_OK)
    {
      PRINTF("[ERROR] Step 1: Device initialization ERROR.\n");
      goto _app_process_exit;
    }
  }

  /* 启动计数：标记不对就当第一次 */
  g_boot_count = ((TAMP->BKP0R & 0xFFFF0000UL) == BOOT_MAGIC) ? ((TAMP->BKP0R & 0xFFFFUL) + 1U) : 1U;
  TAMP->BKP0R = BOOT_MAGIC | (g_boot_count & 0xFFFFUL);
  PRINTF("[C5#20] boot=%" PRIu32 " calendar=%s RSR=0x%08" PRIX32 "\n", g_boot_count,
         (g_calendar_kept != 0U) ? "kept" : "set", g_rsr);

  /* Ensure timestamp info flag starts in a known state */
  TimeStampInfo.flag = HAL_RTC_TIMESTAMP_NO_EVENT;

  if (HAL_RTC_TIMESTAMP_EnableIT() != HAL_OK)
  {
    PRINTF("[ERROR] Step 1: Enable RTC timestamp IT ERROR.\n");
    goto _app_process_exit;
  }

  PRINTF("[INFO] Step 1: Device initialization COMPLETED.\n");
  return_status = EXEC_STATUS_INIT_OK;

_app_process_exit:

  return return_status;
} /* end app_init */


app_status_t app_process(void)
{
  app_status_t return_status = EXEC_STATUS_OK;
  hal_rtc_time_t rtc_time;
  hal_rtc_date_t rtc_date;

  /** ########## Step 2 ##########
    * The applicative code retrieves the current RTC date and time,
    * displays the current date and time, waits to get a one-second update
    */

  /* Get the RTC current Date and Time */
  if (HAL_RTC_CALENDAR_GetDateTime(&rtc_date, &rtc_time) != HAL_OK)
  {
    PRINTF("[ERROR] Step 2: Get RTC date/time ERROR.\n");
    return_status = EXEC_STATUS_ERROR;
    goto _app_process_exit;
  }

  /* Display time Format : hh:mm:ss */
  PRINTF("[INFO] Step 2: Current Time : %2" PRIu32 ":%2" PRIu32 ":%2" PRIu32 " \n", rtc_time.hour, rtc_time.min,
         rtc_time.sec);

  /* Display date Format : mm-dd-yy */
  PRINTF("[INFO] Step 2: Current Date : %2d" "-%2" PRIu32 "-%2" PRIu32 " \n", rtc_date.mon, rtc_date.mday,
         2000 + rtc_date.year);

  /** ########## Step 3 ##########
    * The applicative code checks the RTC timestamp event to display timestamp/datestamp
    * information.
    */

  /* Check the timestamp information flag*/
  if (TimeStampInfo.flag != HAL_RTC_TIMESTAMP_NO_EVENT)
  {
    if (TimeStampInfo.flag == HAL_RTC_TIMESTAMP_EVENT)
    {
      /* Display time Format : hh:mm:ss */
      PRINT_TIMESTAMP(RtcTimeStamp.hour, RtcTimeStamp.min, RtcTimeStamp.sec);

      /* Display date Format : mm-dd-yy */
      PRINT_DATESTAMP(RtcDateStamp.mon, RtcDateStamp.mday);
    }
    else
    {
      return_status = EXEC_STATUS_ERROR;
      PRINTF("[ERROR] Step 3: Timestamp overflow event \n");
    }
  }

  /* Delay to get time updated each second */
  HAL_Delay(1000);

_app_process_exit:

  return return_status;
} /* end app_process */


/** Deinitializes the RTC instance.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_example_rtc_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */


/** RTC timestamp event callback
  * This callback is invoked when the button is pressed.
  */
void HAL_RTC_TimestampEventCallback()
{
  HAL_RTC_TIMESTAMP_GetDateTime((hal_rtc_time_t *)&RtcTimeStamp, (hal_rtc_date_t *)&RtcDateStamp,
                                (hal_rtc_timestamp_information_t *)&TimeStampInfo);
}

/**  User hook function called before the HAL_Init() function
  * C5 #20：官方每次启动都复位 RTC 域；这里只在 RTC 域还没配好时才复位。
  * RCC_RTCCR 不需要先开时钟就能读，只有 RTC 域复位才清它（RM0522 §9.8.32）。
  */
system_status_t pre_system_init_hook(void)
{
  s_domain_was_ready = ((LL_RCC_LSE_IsReady() != 0U)
                        && (LL_RCC_GetRTCClockSource() == LL_RCC_RTC_CLKSOURCE_LSE)
                        && (LL_RCC_IsEnabledRTC() != 0U)) ? 1U : 0U;
  if (s_domain_was_ready == 0U)
  {
    /* Reset the RTC domain (RTC, TAMP, backup registers, LSE configuration, etc) */
    HAL_PWR_DisableRTCDomainWriteProtection();
    HAL_RCC_ResetRTCDomain();
  }
  return SYSTEM_OK;
}

/** 日历已在走：跳过 LSE 配置（LSEDRV 只能在 LSE 关闭时写）、预分频和日期时间，
  * 只补系统复位后丢掉的部分：总线时钟、时间戳使能、PC13 模拟输入、NVIC。
  * 时间戳配置、GPIO、NVIC 三段照抄 mx_rtc.c。
  */
static system_status_t rtc_resume(void)
{
  hal_rtc_timestamp_config_t timestamp_config;
  hal_gpio_config_t gpio_config;

  HAL_PWR_DisableRTCDomainWriteProtection();
  if (HAL_RTC_DisableWriteProtection() != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }
  timestamp_config.input_edge_polarity = HAL_RTC_TIMESTAMP_EDGE_RISING;
  if ((HAL_RTC_TIMESTAMP_SetConfig(&timestamp_config) != HAL_OK)
      || (HAL_RTC_TIMESTAMP_EnablePinSource() != HAL_OK)
      || (HAL_RTC_EnableWriteProtection() != HAL_OK))
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  HAL_RCC_GPIOC_EnableClock();
  gpio_config.mode = HAL_GPIO_MODE_ANALOG;
  gpio_config.pull = HAL_GPIO_PULL_NO;
  HAL_GPIO_Init(PC13_PORT, PC13_PIN, &gpio_config);

  HAL_CORTEX_NVIC_SetPriority(RTC_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(RTC_IRQn);
  return SYSTEM_OK;
}
