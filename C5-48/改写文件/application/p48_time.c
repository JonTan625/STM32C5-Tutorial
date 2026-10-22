/**
  ******************************************************************************
  * file           : p48_time.c
  * brief          : C5 #48 时间模块（F04，由 #46 修正版沿用）：RTC 由 LSE 驱动，TIME 命令校时后在 TAMP_BKP1R 写“已校时”标记。
  *                  B2 复位不复位 RTC 域，标记和日历都保留；无备用电源断电后 RTC 域复位，标记清零，显示 UNSET。
  *                  官方 mx_rtc_init() 每次上电都把日历写成 2025-07-31，会破坏“B2 保留时间”，所以这里不调用它，
  *                  只沿用它的寄存器配置顺序与分频（128 × 256 = 32768）。
  *                  两条勘误（ES0676 Rev 2，Rev Z 适用）：§2.2.3 LSE 低驱动禁用，改用 HIGH（同 rtc/timestamp 生成代码，
  *                  #20 已上板）；§2.7.2 影子寄存器读日历偶发错值，改为旁路影子寄存器并连读两次一致才用。
  ******************************************************************************
  */
#include "p48.h"

#define P48_TIME_MARK   0xC548A5A5UL   /* 本项目私有标记；其他程序留下的值一律不认 */

static uint8_t s_ready;   /* RTC 时钟链已就绪 */
static uint8_t s_valid;   /* 已校时且 RTC 域未复位 */

static uint32_t dec2bcd(uint32_t v)
{
  return ((v / 10U) << 4) | (v % 10U);
}

static uint32_t bcd2dec(uint32_t v)
{
  return ((v >> 4) * 10U) + (v & 0x0FU);
}

/* Sakamoto 算法：0 = 星期日；RTC 编码 1 = 星期一 … 7 = 星期日 */
static hal_rtc_weekday_t weekday_of(uint32_t y, uint32_t m, uint32_t d)
{
  static const uint8_t t[12] = {0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U};
  uint32_t w;

  if (m < 3U)
  {
    y -= 1U;
  }
  w = (y + (y / 4U) - (y / 100U) + (y / 400U) + t[m - 1U] + d) % 7U;
  return (hal_rtc_weekday_t)((w == 0U) ? 7U : w);
}

static int32_t rtc_configure(void)
{
  hal_rtc_config_t rtc_config;
  hal_rtc_calendar_config_t calendar_config;

  rtc_config.mode = HAL_RTC_MODE_BCD;
  rtc_config.asynch_prediv = 128U - 1U;
  rtc_config.synch_prediv = 256U - 1U;
  calendar_config.hour_format = HAL_RTC_CALENDAR_HOUR_FORMAT_24;
  calendar_config.bypass_shadow_register = HAL_RTC_CALENDAR_SHADOW_REG_BYPASS;   /* 勘误 2.7.2 */
  if ((HAL_RTC_SetConfig(&rtc_config) != HAL_OK) || (HAL_RTC_CALENDAR_SetConfig(&calendar_config) != HAL_OK))
  {
    return -1;
  }
  return 0;
}

int32_t time_init(void)
{
  int32_t ret = 0;

  s_ready = 0U;
  s_valid = 0U;
  HAL_PWR_DisableRTCDomainWriteProtection();   /* LSE、RTCSEL、RTCEN、TAMP_BKPxR 都在 RTC 域 */

  if (HAL_RCC_LSE_IsReady() != HAL_RCC_OSC_READY)
  {
    /* 勘误 2.2.3：LSEDRV=00（LOW）禁用。LSE 已在运行时不重配，改驱动要断电一次才生效 */
    if (HAL_RCC_LSE_Enable(HAL_RCC_LSE_ON, HAL_RCC_LSE_DRIVE_HIGH) != HAL_OK)
    {
      return -1;   /* LSE 起振超时：继续运行，但没有时间 */
    }
  }
  /* RTCSEL 一旦选定，只有 RTC 域复位才能改；同值重写无影响 */
  if (HAL_RCC_RTC_GetKernelClkSource() != HAL_RCC_RTC_CLK_SRC_LSE)
  {
    if (HAL_RCC_RTC_SetKernelClkSource(HAL_RCC_RTC_CLK_SRC_LSE) != HAL_OK)
    {
      return -1;
    }
  }
  HAL_RCC_RTCAPB_EnableClock();
  if (HAL_RCC_RTC_EnableKernelClock() != HAL_OK)
  {
    return -1;
  }

  if (TAMP->BKP1R == P48_TIME_MARK)
  {
    s_valid = 1U;          /* B2/IWDG 复位：日历一直在走，不重新配置 */
    if (HAL_RTC_DisableWriteProtection() == HAL_OK)
    {
      RTC->CR |= RTC_CR_BYPSHAD;   /* 只置旁路位，不进初始化模式、不碰小时格式（旧固件可能留下 BYPSHAD=0） */
      (void)HAL_RTC_EnableWriteProtection();
    }
  }
  else
  {
    /* 冷启动或从未校时：只配分频，不写日期——没有依据的时间不伪造 */
    if ((HAL_RTC_DisableWriteProtection() != HAL_OK) || (HAL_RTC_EnterInitMode() != HAL_OK)
        || (rtc_configure() != 0) || (HAL_RTC_ExitInitMode() != HAL_OK))
    {
      ret = -1;
    }
    (void)HAL_RTC_EnableWriteProtection();
  }
  if (ret == 0)
  {
    s_ready = 1U;
  }
  return ret;
}

uint8_t time_valid(void)
{
  return s_valid;
}

int32_t time_get(p48_datetime_t *p_dt)
{
  hal_rtc_date_t date;
  hal_rtc_time_t time;
  hal_rtc_date_t date2;
  hal_rtc_time_t time2;
  uint32_t tries = 0U;

  if (s_valid == 0U)
  {
    return -1;
  }
  /* 勘误 2.7.2 规避方法：旁路影子寄存器后连读两次，一致才用（跨秒时最多再读几次） */
  do
  {
    if ((HAL_RTC_CALENDAR_GetDateTime(&date, &time) != HAL_OK) || (HAL_RTC_CALENDAR_GetDateTime(&date2, &time2) != HAL_OK)
        || (++tries > 4U))
    {
      return -1;
    }
  } while ((date.year != date2.year) || (date.mon != date2.mon) || (date.mday != date2.mday) || (time.hour != time2.hour)
           || (time.min != time2.min) || (time.sec != time2.sec));
  p_dt->year = (uint16_t)(2000U + date.year);
  p_dt->mon  = (uint8_t)bcd2dec((uint32_t)date.mon);   /* 月份枚举是 BCD 编码（LL_RTC_MONTH_OCTOBER = 0x10） */
  p_dt->mday = (uint8_t)date.mday;
  p_dt->hour = (uint8_t)time.hour;
  p_dt->min  = (uint8_t)time.min;
  p_dt->sec  = (uint8_t)time.sec;
  return 0;
}

uint8_t time_is_legal(const p48_datetime_t *p_dt)
{
  static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  uint32_t dim;

  if ((p_dt->year < 2000U) || (p_dt->year > 2099U) || (p_dt->mon < 1U) || (p_dt->mon > 12U))
  {
    return 0U;
  }
  dim = days[p_dt->mon - 1U];
  if ((p_dt->mon == 2U) && ((p_dt->year % 4U) == 0U))
  {
    dim = 29U;             /* 2000～2099 内能被 4 整除即闰年（2000 年也是） */
  }
  if ((p_dt->mday < 1U) || (p_dt->mday > dim) || (p_dt->hour > 23U) || (p_dt->min > 59U) || (p_dt->sec > 59U))
  {
    return 0U;
  }
  return 1U;
}

int32_t time_set(const p48_datetime_t *p_dt)
{
  hal_rtc_date_t date;
  hal_rtc_time_t time;
  int32_t ret = 0;

  if ((s_ready == 0U) || (time_is_legal(p_dt) == 0U))
  {
    return -1;
  }
  date.year = (uint32_t)p_dt->year - 2000U;
  date.mon  = (hal_rtc_month_t)dec2bcd(p_dt->mon);
  date.mday = p_dt->mday;
  date.wday = weekday_of(p_dt->year, p_dt->mon, p_dt->mday);
  time.am_pm = HAL_RTC_TIME_FORMAT_AM_24H;
  time.hour = p_dt->hour;
  time.min  = p_dt->min;
  time.sec  = p_dt->sec;
  time.subsec = 0U;
  time.microsec = 0U;
  time.millisec = 0U;

  if ((HAL_RTC_DisableWriteProtection() != HAL_OK) || (HAL_RTC_EnterInitMode() != HAL_OK)
      || (rtc_configure() != 0) || (HAL_RTC_CALENDAR_SetDateTime(&date, &time) != HAL_OK)
      || (HAL_RTC_ExitInitMode() != HAL_OK))
  {
    ret = -1;
  }
  (void)HAL_RTC_EnableWriteProtection();
  if (ret == 0)
  {
    TAMP->BKP1R = P48_TIME_MARK;   /* 先写好日历再立标记 */
    s_valid = 1U;
  }
  return ret;
}

/* ---- 以下为 #48 新增 ---- */

#define P48_PERIOD_MAGIC  0xC5470000UL   /* TAMP_BKP2R 高 16 位标记，低 16 位为周期秒数 */

volatile uint8_t g_rtc_wake;

void RTC_IRQHandler(void);

/* 2000-01-01 00:00:00 起的秒数；年份限 2000～2099，能被 4 整除即闰年 */
uint32_t time_to_secs(const p48_datetime_t *p_dt)
{
  static const uint16_t before[12] = {0U, 31U, 59U, 90U, 120U, 151U, 181U, 212U, 243U, 273U, 304U, 334U};
  uint32_t y = (uint32_t)p_dt->year - 2000U;
  uint32_t days = (y * 365U) + ((y + 3U) / 4U) + before[p_dt->mon - 1U] + p_dt->mday - 1U;

  if ((p_dt->mon > 2U) && ((y % 4U) == 0U))
  {
    days++;
  }
  return (((days * 24U) + p_dt->hour) * 60U + p_dt->min) * 60U + p_dt->sec;
}

void time_from_secs(uint32_t secs, p48_datetime_t *p_dt)
{
  static const uint8_t dim[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  uint32_t days = secs / 86400U;
  uint32_t rem = secs % 86400U;
  uint32_t y = 0U;
  uint32_t m = 0U;

  while (days >= (((y % 4U) == 0U) ? 366U : 365U))
  {
    days -= ((y % 4U) == 0U) ? 366U : 365U;
    y++;
  }
  while (days >= (uint32_t)(dim[m] + (((m == 1U) && ((y % 4U) == 0U)) ? 1U : 0U)))
  {
    days -= (uint32_t)(dim[m] + (((m == 1U) && ((y % 4U) == 0U)) ? 1U : 0U));
    m++;
  }
  p_dt->year = (uint16_t)(2000U + y);
  p_dt->mon  = (uint8_t)(m + 1U);
  p_dt->mday = (uint8_t)(days + 1U);
  p_dt->hour = (uint8_t)(rem / 3600U);
  p_dt->min  = (uint8_t)((rem % 3600U) / 60U);
  p_dt->sec  = (uint8_t)(rem % 60U);
}

/**
  * 唤醒定时器用 ck_spre（1 Hz），每 (WUT + 1) 秒置一次 WUTF（RM0522 p1576），所以重载值写“周期 − 1”。
  * 配置前必须先停定时器，HAL 会等 WUTWF（RM0522 §39.3.9）。WUTF 由软件在中断里清（WUTOCLR = 0）。
  * RTC 走 EXTI 第 17 线，EXTI_IMR1 复位值已不屏蔽（RM0522 p447、p468）。
  */
int32_t time_wakeup_start(uint32_t period_s)
{
  hal_rtc_wakeup_config_t cfg;
  int32_t ret = 0;

  if ((s_ready == 0U) || (period_s < 1U))
  {
    return -1;
  }
  cfg.clock = HAL_RTC_WAKEUP_TIMER_CLOCK_BCD_UPDATE;
  if (HAL_RTC_DisableWriteProtection() != HAL_OK)
  {
    return -1;
  }
  (void)HAL_RTC_WAKEUP_Stop();
  if ((HAL_RTC_WAKEUP_SetConfig(&cfg) != HAL_OK) || (HAL_RTC_WAKEUP_SetAutoReloadAndAutoClear(period_s - 1U, 0U) != HAL_OK)
      || (HAL_RTC_WAKEUP_Start(HAL_RTC_WAKEUP_IT_ENABLE) != HAL_OK))
  {
    ret = -1;
  }
  (void)HAL_RTC_EnableWriteProtection();
  HAL_CORTEX_NVIC_SetPriority(RTC_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(RTC_IRQn);
  return ret;
}

/* 清 WUTF 要写 RTC_SCR：RTC 域在 time_init() 里已设为可写且不再关闭，避开勘误 2.7.3 */
void RTC_IRQHandler(void)
{
  HAL_RTC_WAKEUP_IRQHandler();
}

void HAL_RTC_WakeUpTimerEventCallback(void)
{
  g_rtc_wake = 1U;
}

uint32_t time_period_load(void)
{
  uint32_t v = TAMP->BKP2R;
  uint32_t p = v & 0xFFFFUL;

  if (((v & 0xFFFF0000UL) != P48_PERIOD_MAGIC) || (p < P48_PERIOD_MIN_S) || (p > P48_PERIOD_MAX_S))
  {
    return P48_PERIOD_DEFAULT_S;
  }
  return p;
}

void time_period_store(uint32_t period_s)
{
  TAMP->BKP2R = P48_PERIOD_MAGIC | (period_s & 0xFFFFUL);
}
