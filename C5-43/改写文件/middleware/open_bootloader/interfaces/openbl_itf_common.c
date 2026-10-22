/**
  ******************************************************************************
  * @file    openbl_itf_common.c
  * @brief   Contains common functions used by different interfaces
  ******************************************************************************
  * @attention
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
#include "stm32_hal.h"
#include "openbl_itf_common.h"

#include "openbl_itf_flash.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Exported functions --------------------------------------------------------*/

/**
  * @brief  Assigns the given value to the Main Stack Pointer (MSP).
  * @param  top_of_main_stack  Main Stack Pointer value to set.
  * @retval None.
  */
__WEAK void OPENBL_Common_SetMsp(uint32_t top_of_main_stack)
{
  /** @warning This function must not be modified, when needed,
               the "OPENBL_Common_SetMsp" can be implemented in the user file.
   */
  STM32_UNUSED(top_of_main_stack);
}

/**
  * @brief  Enable IRQ Interrupts.
  * @retval None.
  */
__WEAK void OPENBL_Common_EnableIrq(void)
{
  /** @warning This function must not be modified, when needed,
               the "OPENBL_Common_EnableIrq" can be implemented in the user file.
   */
}

/**
  * @brief  Disable IRQ Interrupts.
  * @retval None.
  */
__WEAK void OPENBL_Common_DisableIrq(void)
{
  /** @warning This function must not be modified, when needed,
               the "OPENBL_Common_DisableIrq" can be implemented in the user file.
   */
}

/**
  * @brief  Checks whether the target Protection Status is set or not.
  * @retval Returns SET if protection is enabled else return RESET.
  */
__WEAK openbl_state_t OPENBL_Common_GetProtectionStatus(void)
{
  openbl_state_t state = OPENBL_DISABLE;

  /** @warning This function must not be modified, when needed,
               the "OPENBL_Common_GetProtectionStatus" can be implemented in the user file.
   */

  return state;
}

/**
  * @brief  This function is used to jump to a given address.
  * @param  address The address where the function will jump.
  * @retval None.
  */
/* GCC / Cortex-M33 教学跳转：切MSP以后不得执行C函数尾声。 */
__attribute__((naked, noreturn, noinline))
static void openbl43_branch(uint32_t stack __attribute__((unused)),
                            uint32_t reset __attribute__((unused)))
{
  __asm volatile (
    "movs r2, #0\n"
    "msr control, r2\n"
    "msr basepri, r2\n"
    "msr msplim, r2\n"
    "msr psplim, r2\n"
    "msr msp, r0\n"
    "dsb\n"
    "isb\n"
    "cpsie i\n"
    "bx r1\n");
}

void OPENBL_Common_JumpToAddress(uint32_t address)
{
  const uint32_t stack = *(__IO uint32_t *)address;
  const uint32_t reset = *(__IO uint32_t *)(address + 4U);
  __disable_irq();
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
  /* CMSIS数组覆盖外部中断寄存器；未实现位写入无效。 */
  for (uint32_t i = 0U; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++)
  {
    NVIC->ICER[i] = 0xFFFFFFFFU;
    NVIC->ICPR[i] = 0xFFFFFFFFU;
  }
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
  SCB->VTOR = address;
  __DSB();
  __ISB();
  openbl43_branch(stack, reset);
}
