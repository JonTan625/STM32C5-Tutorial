/**
  ******************************************************************************
  * file           : p48_power.c
  * brief          : C5 #48 低功耗（F05、F09）：Stop1 进出，串口 RX（PA3）下降沿经 EXTI 第 3 线叫醒命令模式。
  *                  Stop1 唤醒源只有 GPIO 的 EXTI 与 PVD/COMP/RTC/LPUART/LPTIM 的 EXTI，USART 不在其中
  *                  （RM0522 §8.7 表 54 p230），所以睡前把 PA3 交给 EXTI。不用 B1（PC13）：勘误 2.2.2，
  *                  LQFP 封装上 PC13 跳变会干扰 LSE。进出 Stop1 的 HAL 调用同 #23（已上板）。
  *                  醒来系统时钟为 HSIDIV3 48 MHz（STOPWUCK 复位默认，RM0522 p280），记录唤醒就用它跑完。
  ******************************************************************************
  */
#include "p48.h"
#include "mx_rcc.h"

#define P48_EXTI_LINE3  (1UL << 3)

volatile uint8_t g_console_wake;
uint32_t g_wakes;
uint32_t g_awake_us_last;
uint32_t g_awake_us_max;
uint32_t g_prev_mode;           /* 上一次是否真的进了 Stop1（HAL_PWR_GetPreviousSystemPowerMode） */

static uint32_t s_wake_cycles;  /* 醒来时的 DWT 计数 */
static uint8_t  s_measuring;    /* 1 = 本次是记录唤醒，回睡前要统计醒着时长 */

void EXTI3_IRQHandler(void);

void power_init(void)
{
  HAL_PWR_LP_EnableFlashLowPWRMode();    /* 同官方 pwr/stop1；DS15135 表 26 的 Stop1 0.04 mA 即此条件 */
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;    /* DWT 计醒着的周期数 */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  /* EXTI 第 3 线选 GPIOA（EXTICR1 的 EXTI3 字段，复位值即 A 口），下降沿触发，先不打开屏蔽 */
  EXTI->EXTICR[0] &= ~EXTI_EXTICR1_EXTI3_Msk;
  EXTI->FTSR1 |= P48_EXTI_LINE3;
  EXTI->RTSR1 &= ~P48_EXTI_LINE3;
  EXTI->IMR1 &= ~P48_EXTI_LINE3;
  HAL_CORTEX_NVIC_SetPriority(EXTI3_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_1, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI3_IRQn);
}

void EXTI3_IRQHandler(void)
{
  if ((EXTI->FPR1 & P48_EXTI_LINE3) != 0U)
  {
    EXTI->FPR1 = P48_EXTI_LINE3;      /* 写 1 清除 */
    EXTI->IMR1 &= ~P48_EXTI_LINE3;    /* 后续字符不再重复触发，等回到串口模式 */
    g_console_wake = 1U;
  }
}

void power_cancel_awake(void)
{
  s_measuring = 0U;             /* 转入命令模式：这次醒来不再算作“记录唤醒” */
}

void power_mark_awake_end(void)
{
  uint32_t us;

  if (s_measuring == 0U)
  {
    return;
  }
  s_measuring = 0U;
  us = (DWT->CYCCNT - s_wake_cycles) / (SystemCoreClock / 1000000UL);
  g_awake_us_last = us;
  g_awake_us_max = (us > g_awake_us_max) ? us : g_awake_us_max;
}

void power_sleep(void)
{
  __disable_irq();
  if ((g_rtc_wake != 0U) || (g_console_wake != 0U))
  {
    __enable_irq();               /* 有事没处理完，不睡 */
    return;
  }
  EXTI->FPR1 = P48_EXTI_LINE3;
  EXTI->IMR1 |= P48_EXTI_LINE3;
  HAL_PWR_CleanPreviousSystemPowerMode();
  /* PRIMASK=1 时 WFI 仍会被挂起的中断叫醒，醒来后再开中断让 ISR 执行，避免“查完标志、睡前来中断”的竞争 */
  HAL_PWR_EnterStopMode(HAL_PWR_LOW_PWR_MODE_WFI, HAL_PWR_STOP1_MODE);
  s_wake_cycles = DWT->CYCCNT;
  g_prev_mode = (uint32_t)HAL_PWR_GetPreviousSystemPowerMode();
  (void)HAL_UpdateCoreClock();    /* SystemCoreClock 改为 48 MHz，并按它重设 SysTick */
  g_wakes++;
  s_measuring = 1U;
  __enable_irq();
}

/* 命令模式要 115200 波特率：恢复与上电相同的 HSE 24 MHz → PSIS 144 MHz（生成代码 mx_rcc_init） */
int32_t power_console_clock(void)
{
  return (mx_rcc_init() == SYSTEM_OK) ? 0 : -1;
}
