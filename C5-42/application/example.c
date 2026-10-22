/**
  ******************************************************************************
  * file           : example.c
  * brief          : #42 standalone USB HID mouse teaching application.
  ******************************************************************************
  * Copyright (c) 2026 STMicroelectronics. All rights reserved.
  * This software is licensed under terms in the LICENSE file of this component.
  * Teaching adaptation by JonTan, 2026-10-20.
  ******************************************************************************
  */
#include "example.h"
#include "mx_usbx_app.h"
#include "mx_usbx_device_hid_mouse.h"
#include "mx_crs.h"
#include "ux_dcd_stm32.h"

#define BUTTON_GUARD_MS 50U /* 教学抑制窗口，需按实物开关验证。 */

hal_exti_handle_t *pEXTI;
volatile uint8_t ButtonState;
volatile uint32_t g_edges, g_press, g_bounce, g_fault;
volatile uint32_t g_usb_state, g_pcd_state, g_ck48_source;
volatile uint32_t g_crs_cr, g_crs_cfgr, g_crs_isr;
volatile UINT g_init_status, g_tasks_status;
static uint32_t last_press_ms;
static uint8_t have_press;

static void UserDetectedExtiCallback(hal_exti_handle_t *hexti, hal_exti_trigger_t trigger)
{
  STM32_UNUSED(hexti);
  STM32_UNUSED(trigger);
  uint32_t now = HAL_GetTick();
  g_edges++;
  if (have_press == 0U || (uint32_t)(now - last_press_ms) >= BUTTON_GUARD_MS)
  {
    have_press = 1U;
    last_press_ms = now;
    g_press++;
    ButtonState = 1U; /* 单标志可合并多次快速按键；中断里不发USB。 */
  }
  else
  {
    g_bounce++;
  }
}

app_status_t app_init(void)
{
  led_off(MX_STATUS_LED);
  hal_crs_handle_t *crs = mx_crs_gethandle();
  pEXTI = mx_example_exti_trigger_gethandle();
  if (crs == NULL || pEXTI == NULL)
  {
    g_fault = 1U;
    return EXEC_STATUS_ERROR;
  }
  hal_crs_config_t config = {
    .divider = HAL_CRS_SYNC_DIV1,
    .source = HAL_CRS_SYNC_SOURCE_USB,
    .polarity = HAL_CRS_SYNC_POLARITY_RISING,
    .reload = HAL_CRS_RELOAD_DEFAULT_VALUE,
    .frequency_error_limit = HAL_CRS_ERRORLIMIT_DEFAULT_VALUE,
    .trimming = HAL_CRS_TRIMMING_DEFAULT_VALUE,
    .auto_trimming = HAL_CRS_AUTO_TRIMMING_ENABLE,
  };
  if (HAL_CRS_SetConfig(crs, &config) != HAL_OK || HAL_CRS_StartSync(crs) != HAL_OK)
  {
    g_fault = 2U;
    return EXEC_STATUS_ERROR;
  }
  hal_pcd_handle_t *pcd = mx_example_pcd_init();
  if (pcd == NULL)
  {
    g_fault = 3U;
    return EXEC_STATUS_ERROR;
  }
  g_ck48_source = (uint32_t)HAL_RCC_CK48_GetKernelClkSource();
  g_init_status = app_usbx_init();
  if (g_init_status != UX_SUCCESS)
  {
    g_fault = 4U;
    return EXEC_STATUS_ERROR;
  }
  /* 与官方DCD初始化调用一致；PCD只初始化一次。 */
  g_init_status = ux_dcd_stm32_initialize(0U, (ULONG)pcd);
  if (g_init_status != UX_SUCCESS || HAL_PCD_GetState(pcd) != HAL_PCD_STATE_ACTIVE)
  {
    g_fault = 5U;
    return EXEC_STATUS_ERROR;
  }
  if (HAL_EXTI_RegisterTriggerCallback(pEXTI, UserDetectedExtiCallback) != HAL_OK
      || HAL_EXTI_Enable(pEXTI, HAL_EXTI_MODE_INTERRUPT) != HAL_OK)
  {
    g_fault = 6U;
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
}

app_status_t app_process(void)
{
  /* Standalone需要高频推进任务；不能在这里加长时间HAL_Delay。 */
  g_tasks_status = ux_device_stack_tasks_run();
  usbd_hid_mouse_process();
  g_usb_state = _ux_system_slave->ux_system_slave_device.ux_slave_device_state;
  g_pcd_state = (uint32_t)HAL_PCD_GetState(mx_example_pcd_gethandle());
  g_crs_cr = CRS->CR;
  g_crs_cfgr = CRS->CFGR;
  g_crs_isr = CRS->ISR;
  return EXEC_STATUS_OK;
}

app_status_t app_deinit(void)
{
  (void)HAL_EXTI_Disable(pEXTI);
  (void)HAL_PCD_Stop(mx_example_pcd_gethandle());
  (void)HAL_CRS_StopSync(mx_crs_gethandle());
  return EXEC_STATUS_OK;
}
