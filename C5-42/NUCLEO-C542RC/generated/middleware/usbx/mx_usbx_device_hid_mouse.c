/**
  ******************************************************************************
  * @file    mx_usbx_device_hid_mouse.c
  * @brief   USBX Device Mouse HID applicative source file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025-2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the mx_usbx_license.md file
  * in the same directory as the generated code.
  * If no mx_usbx_license.md file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "mx_usbx_device_hid_mouse.h"
#include "mx_led.h"
/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define CURSOR_STEP     10
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
UX_SLAVE_CLASS_HID *p_hid_mouse;
volatile uint32_t g_configured, g_activations, g_deactivations;
volatile uint32_t g_queued, g_queue_fail, g_ignored, g_step;
volatile UINT g_queue_status;
volatile int32_t g_x, g_y;
volatile UCHAR g_last_report[4];
extern volatile uint8_t ButtonState;
UCHAR usbd_hid_mouse_report_descriptor[] =
{
  0x05U, 0x01U, /* Usage Page (Generic Desktop)*/
  0x09U, 0x02U, /* Usage (Mouse) */
  0xA1U, 0x01U, /* Collection (Application) */
  0x09U, 0x01U, /* Usage (Pointer) */

  0xA1U, 0x00U, /* Collection (Physical) */
  0x05U, 0x09U, /* Usage Page (Buttons) */
  0x19U, 0x01U, /* Usage Minimum (01U) */
  0x29U, 0x03U, /* Usage Maximum (03U) */

  0x15U, 0x00U, /* Logical Minimum (0U) */
  0x25U, 0x01U, /* Logical Maximum (1U) */
  0x95U, 0x03U, /* Report Count (3U) */
  0x75U, 0x01U, /* Report Size (1U) */

  0x81U, 0x02U, /* Input(Data, Variable, Absolute) 3U button bit fields */
  0x95U, 0x01U, /* Report Count (1U) */
  0x75U, 0x05U, /* Report Size (5U) */
  0x81U, 0x01U, /* Input (Constant), 5U constant field */

  0x05U, 0x01U, /* Usage Page (Generic Desktop) */
  0x09U, 0x30U, /* Usage (X) */
  0x09U, 0x31U, /* Usage (Y) */
  0x09U, 0x38U, /* Usage (Wheel), 0x38; descriptor bytes unchanged */

  0x15U, 0x81U, /* Logical Minimum (-127) */
  0x25U, 0x7FU, /* Logical Maximum (127) */
  0x75U, 0x08U, /* Report Size (8U) */
  0x95U, 0x03U, /* Report Count (3U) */

  0x81U, 0x06U, /* Input(Data, Variable, Relative), Three position bytes (X & Y & Z)*/
  0xC0U, 0xC0U  /* End collection, Close Pointer collection*/
};
/* Private function prototypes -----------------------------------------------*/
static VOID usbd_hid_mouse_get_pointer_data(UX_SLAVE_CLASS_HID_EVENT *p_hid_event);

/**
  * @brief  usbd_hid_mouse_activate
  *         This function is called when insertion of a HID Mouse device.
  * @retval none
  */
VOID usbd_hid_mouse_activate(VOID *hid_instance)
{
  p_hid_mouse = (UX_SLAVE_CLASS_HID *)hid_instance;
  ButtonState = 0U;
  g_step = 0U;
  g_configured = 1U;
  g_activations++;
  led_on(MX_STATUS_LED); /* 类激活标记，不能替代主机枚举/报告检查。 */
}

/**
  * @brief  usbd_hid_mouse_deactivate
  *         This function is called when extraction of a HID Mouse device.
  * @param  hid_instance: Pointer to the hid class instance.
  * @retval none
  */
VOID usbd_hid_mouse_deactivate(VOID *hid_instance)
{
  if (p_hid_mouse == hid_instance)
  {
    p_hid_mouse = UX_NULL;
    ButtonState = 0U;
    g_configured = 0U;
    g_deactivations++;
    g_step = 0U;
    led_off(MX_STATUS_LED);
  }
}

/**
  * @brief  usbd_hid_mouse_set_report
  *         This function is invoked when the host sends a HID SET_REPORT
  *         to the application over Endpoint 0.
  * @retval status
  */
UINT usbd_hid_mouse_set_report(UX_SLAVE_CLASS_HID *hid_instance,
                               UX_SLAVE_CLASS_HID_EVENT *hid_event)
{
  UINT status = UX_SUCCESS;

  UX_PARAMETER_NOT_USED(hid_instance);
  UX_PARAMETER_NOT_USED(hid_event);

  return status;
}

/**
  * @brief  usbd_hid_mouse_get_report
  *         This function is invoked when host is requesting event through
  *         control GET_REPORT request.
  * @retval status
  */
UINT usbd_hid_mouse_get_report(UX_SLAVE_CLASS_HID *hid_instance,
                               UX_SLAVE_CLASS_HID_EVENT *hid_event)
{
  UINT status = UX_SUCCESS;

  UX_PARAMETER_NOT_USED(hid_instance);
  UX_PARAMETER_NOT_USED(hid_event);

  return status;
}

/**
  * @brief  usbd_hid_mouse_report_address
  *         Return the device HID MOUSE Report Descriptor
  * @retval Pointer to HID Report Descriptor buffer
  */
UCHAR *usbd_hid_mouse_report_address(VOID)
{
  return usbd_hid_mouse_report_descriptor;
}

/**
  * @brief  usbd_hid_mouse_report_length
  *         Return the device HID Report Descriptor
  * @retval Size of HID Report Descriptor buffer
  */
ULONG usbd_hid_mouse_report_length(VOID)
{
  return sizeof(usbd_hid_mouse_report_descriptor);
}

/**
  * @brief  usbd_hid_mouse_report_id
  *         Return the device HID Report ID Support
  * @retval UX_FALSE / UX_TRUE
  */
ULONG usbd_hid_mouse_report_id(VOID)
{
  return UX_FALSE;
}

/**
  * @brief  usbd_hid_mouse_process
  *         Run HID mouse task
  * @param  none
  * @retval none
  */
VOID usbd_hid_mouse_process(VOID)
{
  UX_SLAVE_CLASS_HID_EVENT hid_event;
  uint32_t flags = __get_PRIMASK();
  __disable_irq();
  uint8_t pressed = ButtonState;
  ButtonState = 0U;
  __set_PRIMASK(flags);
  if (pressed == 0U)
  {
    return;
  }
  if (p_hid_mouse == UX_NULL)
  {
    g_ignored++;
    return;
  }
  ux_utility_memory_set(&hid_event, 0, sizeof(hid_event));
  usbd_hid_mouse_get_pointer_data(&hid_event);
  g_queue_status = ux_device_class_hid_event_set(p_hid_mouse, &hid_event);
  if (g_queue_status == UX_SUCCESS)
  {
    g_queued++; /* 只统计入队，不冒充主机已收到。 */
    g_step = (g_step + 1U) % 16U;
  }
  else
  {
    g_queue_fail++; /* 记录失败，不推进方向，不自动重放按键。 */
  }
}

/**
  * @brief  usbd_hid_mouse_get_pointer_data
  *         Gets Pointer Data.
  * @retval None
  */
static VOID usbd_hid_mouse_get_pointer_data(UX_SLAVE_CLASS_HID_EVENT *p_hid_event)
{
  g_x = 0;
  g_y = 0;
  switch (g_step / 4U)
  {
    case 0U: g_x = CURSOR_STEP; break;
    case 1U: g_y = CURSOR_STEP; break;
    case 2U: g_x = -CURSOR_STEP; break;
    default: g_y = -CURSOR_STEP; break;
  }
  p_hid_event->ux_device_class_hid_event_length = 4U;
  p_hid_event->ux_device_class_hid_event_buffer[0] = 0U;
  p_hid_event->ux_device_class_hid_event_buffer[1] = (UCHAR)(int8_t)g_x;
  p_hid_event->ux_device_class_hid_event_buffer[2] = (UCHAR)(int8_t)g_y;
  p_hid_event->ux_device_class_hid_event_buffer[3] = 0U;
  for (uint32_t i = 0U; i < 4U; i++)
  {
    g_last_report[i] = p_hid_event->ux_device_class_hid_event_buffer[i];
  }
}
