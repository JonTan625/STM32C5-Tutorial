/**
  ******************************************************************************
  * file           : p46_sampler.c
  * brief          : C5 #46 采样模块：ADC1_IN4（PA4）软件触发单次转换，中断只发布样本。
  *                  ADC 初始化、校准、启动转换沿用官方 hal/adc/single_conversion_trigger_sw_it。
  ******************************************************************************
  */
#include "p46.h"

static hal_adc_handle_t *s_adc;

/* 写者约定：s_pending 由主循环置 1、中断或 sampler_stop 清 0；
   s_ready/s_sample 由中断写、主循环在关中断时取走。 */
static volatile uint8_t  s_pending;
static volatile uint32_t s_request_tick;
static volatile uint8_t  s_ready;
static volatile uint8_t  s_error;
static p46_sample_t      s_sample;

#if defined(P46_TEST) && (P46_TEST != 0)
static volatile uint8_t s_test_mask;
static volatile uint8_t s_test_error;
static uint16_t         s_test_values[4];
static volatile uint8_t s_test_count;
static volatile uint8_t s_test_index;
#endif /* P46_TEST */

static void sampler_conv_cplt(hal_adc_handle_t *hadc);
static void sampler_error_cb(hal_adc_handle_t *hadc);

int32_t sampler_init(void)
{
  /* ADC/DAC 共用内核时钟分频：144 MHz ÷ 8 = 18 MHz（#45 参数 adc_kernel_hz）。
     必须在 ADC 使能前改（RM0522 p316，#30 已上板验证同一写法）。 */
  if (HAL_RCC_ADCDAC_SetKernelClkPrescaler(HAL_RCC_ADCDAC_PRESCALER8) != HAL_OK)
  {
    return -1;
  }

  s_adc = mx_example_adc_init();
  if (s_adc == NULL)
  {
    return -1;
  }
  if (HAL_ADC_RegisterRegUnitaryConvCpltCallback(s_adc, sampler_conv_cplt) != HAL_OK)
  {
    return -1;
  }
  if (HAL_ADC_RegisterErrorCallback(s_adc, sampler_error_cb) != HAL_OK)
  {
    return -1;
  }
  /* 官方顺序：先 Start（使能 ADC）再校准 */
  if (HAL_ADC_Start(s_adc) != HAL_OK)
  {
    return -1;
  }
  if (HAL_ADC_Calibrate(s_adc) != HAL_OK)
  {
    return -1;
  }
  return 0;
}

int32_t sampler_request(uint32_t now_ms)
{
  if (s_pending != 0U)
  {
    return -1;
  }
#if defined(P46_TEST) && (P46_TEST != 0)
  if (s_test_error != 0U)
  {
    s_test_error = 0U;
    return -1;
  }
#endif /* P46_TEST */
  s_request_tick = now_ms;
  s_pending = 1U;
  if (HAL_ADC_REG_StartConv_IT(s_adc) != HAL_OK)
  {
    s_pending = 0U;
    return -1;
  }
  return 0;
}

uint8_t sampler_take(p46_sample_t *p_out)
{
  uint8_t got = 0U;

  __disable_irq();
  if (s_ready != 0U)
  {
    *p_out = s_sample;   /* 两个字段在同一临界区内取走，保证是同一次转换 */
    s_ready = 0U;
    got = 1U;
  }
  __enable_irq();
  return got;
}

uint8_t sampler_pending(uint32_t *p_since_ms)
{
  *p_since_ms = s_request_tick;
  return s_pending;
}

uint8_t sampler_take_error(void)
{
  uint8_t err = s_error;

  s_error = 0U;
  return err;
}

void sampler_stop(void)
{
  __disable_irq();
  s_pending = 0U;      /* 之后到达的回调一律丢弃 */
  s_ready = 0U;
  __enable_irq();
  (void)HAL_ADC_REG_StopConv_IT(s_adc);
  s_error = 0U;
}

static void sampler_conv_cplt(hal_adc_handle_t *hadc)
{
  uint16_t raw = (uint16_t)HAL_ADC_REG_ReadConversionData(hadc);   /* 读 DR 清 EOC */

  if (s_pending == 0U)
  {
    return;            /* 已停止：迟到结果不发布 */
  }
#if defined(P46_TEST) && (P46_TEST != 0)
  if (s_test_mask != 0U)
  {
    return;            /* 吞掉完成事件，s_pending 保持 1，由主循环判超时 */
  }
  if (s_test_index < s_test_count)
  {
    raw = s_test_values[s_test_index];
    s_test_index++;
  }
#endif /* P46_TEST */
  s_sample.raw = raw;
  s_sample.tick = HAL_GetTick();
  s_ready = 1U;
  s_pending = 0U;
}

static void sampler_error_cb(hal_adc_handle_t *hadc)
{
  STM32_UNUSED(hadc);
  s_error = 1U;
  s_pending = 0U;
}

#if defined(P46_TEST) && (P46_TEST != 0)
void sampler_test_mask_complete(uint8_t on)
{
  s_test_mask = on;
}

void sampler_test_inject_error(void)
{
  s_test_error = 1U;
}

uint8_t sampler_test_queue(const uint16_t *p_values, uint32_t count)
{
  if (count > 4U)
  {
    return 0U;
  }
  __disable_irq();
  for (uint32_t i = 0U; i < count; i++)
  {
    s_test_values[i] = p_values[i];
  }
  s_test_count = (uint8_t)count;
  s_test_index = 0U;
  __enable_irq();
  return 1U;
}
#endif /* P46_TEST */
