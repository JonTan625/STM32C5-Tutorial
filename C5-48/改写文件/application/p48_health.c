/**
  ******************************************************************************
  * file           : p48_health.c
  * brief          : C5 #48 健康监视（F08，由 #46 沿用）：启动先快照复位原因再清标志；IWDG 120 s。
  *                  选项字 IWDG_STOP 出厂为 1，IWDG 在 Stop 中照样计数（RM0522 表 28 p151），所以每次醒来都喂；
  *                  记录周期上限 60 s，离最早的 114 s（LSI 33.6 kHz）复位有将近一倍余量。不改选项字。
  *                  IWDG 启动写法沿用官方 hal/iwdg/reset 与 #21（2026-10-14 上板核验）。
  ******************************************************************************
  */
#include "p48.h"

#define P48_STACK_FILL  0xA5A5A5A5UL

extern uint32_t __StackLimit;   /* 链接脚本 .stack 段的起止（stm32c542xc_flash.ld） */
extern uint32_t __StackTop;

static hal_iwdg_handle_t s_iwdg;
static uint32_t s_reset_flags;
static uint32_t s_stack_check_ms;
static uint8_t s_started;

/* 供调试器核对 HAL 按 LSI_VALUE=32 kHz 算出的分频与重载值（#46 取证项） */
uint32_t g_iwdg_pr;
uint32_t g_iwdg_rlr;
/* 栈低水位：启动后从未被用到的栈字节数，每秒更新一次（R45-18 “总 RAM 含栈有依据”的实板取证量） */
uint32_t g_stack_free_min;

void health_capture_reset(void)
{
  uint32_t *p = &__StackLimit;
  uint32_t *sp = (uint32_t *)__get_MSP();

  s_reset_flags = HAL_RCC_GetResetSource();
  HAL_RCC_ClearResetFlags();   /* 不清的话，下次复位会把这次的原因也带上 */

  while (p < (sp - 16))        /* 当前栈指针以下留 64 字节余量，其余填充标记 */
  {
    *p = P48_STACK_FILL;
    p++;
  }
}

static void stack_measure(void)
{
  const uint32_t *p = &__StackLimit;
  uint32_t words = (uint32_t)(&__StackTop - &__StackLimit);
  uint32_t n = 0U;

  while ((n < words) && (p[n] == P48_STACK_FILL))
  {
    n++;
  }
  g_stack_free_min = n * 4U;
}

const char *health_reset_name(void)
{
  /* 多个标志同时置位时按“更具体的原因”优先：看门狗 > 软件 > 低功耗 > 上电/BOR > 引脚 */
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_IWDG) != 0U)
  {
    return "IWDG";
  }
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_WWDG) != 0U)
  {
    return "WWDG";
  }
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_SW) != 0U)
  {
    return "SW";
  }
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_LPWR) != 0U)
  {
    return "LPWR";
  }
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_PWR) != 0U)
  {
    return "POR";
  }
  if ((s_reset_flags & HAL_RCC_RESET_FLAG_PIN) != 0U)
  {
    return "PIN";
  }
  return "NONE";
}

int32_t health_start(void)
{
  if (HAL_IWDG_Init(&s_iwdg, HAL_IWDG1) != HAL_OK)
  {
    return -1;
  }
  /* 不用窗口、不用提前唤醒中断：(句柄, 最小刷新时间, 超时, EWI) */
  if (HAL_IWDG_Start(&s_iwdg, 0U, P48_IWDG_MS, 0U) != HAL_OK)
  {
    return -1;
  }
  g_iwdg_pr = IWDG->PR;
  g_iwdg_rlr = IWDG->RLR;
  s_started = 1U;
  return 0;
}

void health_feed(void)
{
  if (s_started != 0U)
  {
    (void)HAL_IWDG_Refresh(&s_iwdg);
  }
  if ((HAL_GetTick() - s_stack_check_ms) >= 1000U)
  {
    s_stack_check_ms = HAL_GetTick();
    stack_measure();
  }
}
