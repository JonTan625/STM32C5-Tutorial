/**
  ******************************************************************************
  * file           : p46_health.c
  * brief          : C5 #46 健康监视（F08）：启动先快照复位原因再清标志；IWDG 目标 2000 ms，
  *                  只有主循环各项都推进过才喂狗——不在定时中断里无条件喂。
  *                  IWDG 启动写法沿用官方 hal/iwdg/reset 与 #21（2026-10-14 上板核验）。
  ******************************************************************************
  */
#include "p46.h"

#define P46_STACK_FILL  0xA5A5A5A5UL

extern uint32_t __StackLimit;   /* 链接脚本 .stack 段的起止（stm32c542xc_flash.ld） */
extern uint32_t __StackTop;

static hal_iwdg_handle_t s_iwdg;
static uint32_t s_reset_flags;
static uint32_t s_progress;
static uint32_t s_stack_check_ms;

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
    *p = P46_STACK_FILL;
    p++;
  }
}

static void stack_measure(void)
{
  const uint32_t *p = &__StackLimit;
  uint32_t words = (uint32_t)(&__StackTop - &__StackLimit);
  uint32_t n = 0U;

  while ((n < words) && (p[n] == P46_STACK_FILL))
  {
    n++;
  }
  g_stack_free_min = n * 4U;
}

uint32_t health_reset_flags(void)
{
  return s_reset_flags;
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
  if (HAL_IWDG_Start(&s_iwdg, 0U, P46_IWDG_MS, 0U) != HAL_OK)
  {
    return -1;
  }
  g_iwdg_pr = IWDG->PR;
  g_iwdg_rlr = IWDG->RLR;
  s_progress = 0U;
  return 0;
}

void health_progress(uint32_t mask)
{
  s_progress |= mask;
}

void health_service(void)
{
  if ((s_progress & P46_PROGRESS_ALL) == P46_PROGRESS_ALL)
  {
    (void)HAL_IWDG_Refresh(&s_iwdg);
    s_progress = 0U;
  }
  if ((HAL_GetTick() - s_stack_check_ms) >= 1000U)
  {
    s_stack_check_ms = HAL_GetTick();
    stack_measure();
  }
}
