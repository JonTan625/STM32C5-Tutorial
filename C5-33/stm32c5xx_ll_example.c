/**
  ******************************************************************************
  * file           : stm32c5xx_ll_example.c
  * brief          : ADC multi channels conversion triggered by software in DMA mode with LL API
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
#include "ll_example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* MX_ADCx delay between end of calibration and enable operations (unit: CPU clock cycles). */
#define ADC_DELAY_CALIB_ENABLE_CPU_CYCLES (LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES * 8U)
#define ADC_ENABLE_TIMEOUT_MS             (2U)        /* MX_ADCx enable time-out value (unit: milli seconds)      */
#define ADC_DISABLE_TIMEOUT_MS            (2U)        /* MX_ADCx enable time-out value (unit: milli seconds)      */
#define ADC_CALIBRATION_TIMEOUT_MS        (500U)      /* MX_ADCx calibration time-out value (unit: milli seconds) */

/* @user: The maximum data bus width used by DMA in STM32 devices is 64 bits.
          Therefore, 8-byte alignment is the minimum recommended alignment for DMA buffers across STM32 devices. */
#define DMA_ACCESS_ALIGNMENT        (8U)

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/** Buffer to store converted raw data to be transferred through DMA.
  * - Aligned for DMA constraints.
  * - Variable for the ADC conversion data (a count on the ADC scale).
  * C5 #33 改写：
  * - 声明为 volatile：DMA 写、CPU 读，编译器不知道有人在背后改这块内存。
  * - 去掉官方的 section("non_cacheable_area")：STM32C542RC 没有数据缓存；该段在链接脚本里是孤儿段，
  *   启动代码既不拷贝也不清零，放进去的缓冲区上电后是随机内容。改放普通 .bss，启动时清零。
  */
__attribute__((aligned(DMA_ACCESS_ALIGNMENT)))
volatile uint16_t ADCRawData[ADC_BUFFER_SIZE];

/* Private functions prototype -----------------------------------------------*/
void mx_adcx_irqhandler(void);
void mx_lpdmax_chy_irqhandler(void);
static void DelayUs(uint32_t delay_us);


/* Private functions ---------------------------------------------------------*/

/**
  *  Activate the MX_ADCx.
  *  SYSTEM_PERIPHERAL_ERROR MX_ADCx error
  *  SYSTEM_OK               MX_ADCx success
  */
inline system_status_t ADC_Activate(void)
{
#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
  uint32_t timeout_ms = ADC_ENABLE_TIMEOUT_MS;
#endif /* USE_LL_APP_TIMEOUT */
  system_status_t return_status = SYSTEM_OK;

  /* Enable MX_ADCx instance after clearing the MX_ADCx ready flag */
  LL_ADC_ClearFlag_ADRDY(MX_ADCx);
  LL_ADC_Enable(MX_ADCx);

  /* Wait for MX_ADCx effectively enabled */
  while (LL_ADC_IsActiveFlag_ADRDY(MX_ADCx) == 0U)
  {
    /* If ADEN bit is set less than 4 MX_ADCx clock cycles after the ADCAL bit has been cleared (after a calibration),
       ADEN bit is reset by the calibration logic.
       The workaround is to continue setting ADEN until ADRDY is becomes 1.
       Additionally, ADC_ENABLE_TIMEOUT_MS is defined to encompass this 4 MX_ADCx clock cycle duration */
    /* Note: Test of MX_ADCx enabled required due to hardware constraint to not enable MX_ADCx if already enabled. */
    if (LL_ADC_IsEnabled(MX_ADCx) == 0U)
    {
      LL_ADC_Enable(MX_ADCx);
    }

#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
    if (SysTick_IsActiveCounterFlag() != 0)
    {
      if (timeout_ms-- == 0U)
      {
        return_status = SYSTEM_PERIPHERAL_ERROR;
        break;
      }
    }
#endif /* USE_LL_APP_TIMEOUT */
  }

  return return_status;
}


/**
  *  Calibrates the MX_ADCx instance.
  *  SYSTEM_PERIPHERAL_ERROR MX_ADCx error
  *  SYSTEM_OK               MX_ADCx success
  */
inline system_status_t ADC_Calibrate(void)
{
  uint32_t wait_start; /* DWT已由app_init启用，避免空循环被Release优化掉。 */
#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
  uint32_t timeout_ms = ADC_DISABLE_TIMEOUT_MS;
#endif /* USE_LL_APP_TIMEOUT */
  system_status_t return_status = SYSTEM_OK;

  /* Enable MX_ADCx instance before clearing the MX_ADCx ready flag */
  if (LL_ADC_IsEnabled(MX_ADCx) != 0U)
  {
    LL_ADC_Disable(MX_ADCx);
  }
  LL_ADC_ClearFlag_ADRDY(MX_ADCx);

  /* Wait for MX_ADCx effectively deactivated */
  while (LL_ADC_IsEnabled(MX_ADCx) != 0U)
  {
#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
    if (SysTick_IsActiveCounterFlag() != 0)
    {
      if (timeout_ms-- == 0U)
      {
        return_status = SYSTEM_PERIPHERAL_ERROR;
        break;
      }
    }
#endif /* USE_LL_APP_TIMEOUT */
  }

#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
  timeout_ms = ADC_CALIBRATION_TIMEOUT_MS;
#endif /* USE_LL_APP_TIMEOUT */

  if (return_status != SYSTEM_OK)
  {
    return return_status;
  }

  /* Disable the MX_ADCx deep power down feature (enabled by default after reset state) */
  LL_ADC_DisableDeepPowerDown(MX_ADCx);

  /* Enable MX_ADCx internal regulator voltage and wait for MX_ADCx internal voltage regulator stabilization */
  LL_ADC_EnableInternalRegulator(MX_ADCx);
  DelayUs(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);

  /* Starts the MX_ADCx self-calibration in single ended mode */
  LL_ADC_StartCalibration(MX_ADCx, LL_ADC_IN_SINGLE_ENDED);
#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
  timeout_ms = ADC_CALIBRATION_TIMEOUT_MS;
#endif /* USE_LL_APP_TIMEOUT */
  /* Poll for MX_ADCx calibration completion */
  while (LL_ADC_IsCalibrationOnGoing(MX_ADCx) != 0U)
  {
#if defined(USE_LL_APP_TIMEOUT) && (USE_LL_APP_TIMEOUT == 1)
    if (SysTick_IsActiveCounterFlag() != 0)
    {
      if (timeout_ms-- == 0U)
      {
        return_status = SYSTEM_PERIPHERAL_ERROR;
        break;
      }
    }
#endif /* USE_LL_APP_TIMEOUT */
  }

  /* Delay for MX_ADCx stabilization after calibration and before the activation of the MX_ADCx for conversions */
  wait_start = DWT->CYCCNT;
  while ((DWT->CYCCNT - wait_start) < ADC_DELAY_CALIB_ENABLE_CPU_CYCLES)
  {
  }

  if (return_status == SYSTEM_OK)
  {
    return_status = ADC_Activate();
  }

  return return_status;
}


/**
  *  Start the MX_ADCx conversion in DMA mode.
  */
inline void ADC_StartConversion_DMA(void)
{
  uint32_t i;

  /* C5 #33 改写：先把缓冲区填成哨兵，DMA 写过的槽才分得出来；__DMB 保证填完再启动 DMA */
  for (i = 0U; i < ADC_BUFFER_SIZE; i++)
  {
    ADCRawData[i] = ADC_SENTINEL;
  }
  __DMB();

  LL_DMA_ConfigAddresses(MX_LPDMAX_CHY_CH,
                         LL_ADC_DMA_GetRegAddr(MX_ADCx, LL_ADC_DMA_REG_REGULAR_DATA),
                         (uint32_t)ADCRawData);
  LL_DMA_SetBlkDataLength(MX_LPDMAX_CHY_CH, (ADC_BUFFER_SIZE * sizeof(uint16_t)));
  LL_DMA_ClearFlag(MX_LPDMAX_CHY_CH, LL_DMA_FLAG_ALL);
#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
  LL_DMA_EnableIT(MX_LPDMAX_CHY_CH, (LL_DMA_IT_TC | LL_DMA_IT_DTE | LL_DMA_IT_USE));
#else
  LL_DMA_EnableIT(MX_LPDMAX_CHY_CH, LL_DMA_IT_TC);
#endif /* USE_LL_APP_ERROR */
  LL_DMA_EnableChannel(MX_LPDMAX_CHY_CH);

  /* Enable ADC DMA transfer, one-shot (C5 #33): ADC and DMA stop together after the last of ADC_BUFFER_SIZE data */
  LL_ADC_REG_SetDataTransferMode(MX_ADCx, LL_ADC_REG_DMA_TRANSFER_LIMITED);

  /* Enables the MX_ADCx overrun interrupt when USE_LL_APP_ERROR == 1 */
#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
  LL_ADC_ClearFlag(MX_ADCx, LL_ADC_FLAG_OVR);
  LL_ADC_EnableIT(MX_ADCx, LL_ADC_IT_OVR);
#endif /* USE_LL_APP_ERROR */

  /* EXTEN != 0：ADSTART只使能外部触发接收；此刻TIM6仍停着（RM0522 §21.4.19）。 */
  LL_ADC_REG_StartConversion(MX_ADCx);
}


#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
/**
  *  Handles the MX_ADCx interrupts.
  */
void mx_adcx_irqhandler(void)
{
  LL_ADC_ClearFlag_OVR(MX_ADCx);
}
#endif /* USE_LL_APP_ERROR */


/**
  *  Handles the MX_LPDMAX_CHY_CH interrupts.
  */
void mx_lpdmax_chy_irqhandler(void)
{
  DMA_TypeDef *instance;
  uint32_t    flags;
  uint32_t    its;
  uint32_t    dma_channel;

  instance    = LL_DMA_GET_INSTANCE(MX_LPDMAX_CHY_CH);
  dma_channel = LL_DMA_GET_CHANNEL_IDX(MX_LPDMAX_CHY_CH);
  its         = LL_DMA_READ_REG((MX_LPDMAX_CHY_CH), CCR);

  /* Check DMA channel active interrupts */
  if (LL_DMA_IsActiveFlag_MIS(instance, dma_channel) == 0U)
  {
    return; /* the global interrupt flag for the current channel is down , nothing to do */
  }

  flags = LL_DMA_READ_REG((MX_LPDMAX_CHY_CH), CSR);

  /* Transfer Complete Interrupt management */
  if (STM32_READ_BIT((flags & its), LL_DMA_FLAG_TC) != 0U)
  {
    LL_DMA_ClearFlag_TC(MX_LPDMAX_CHY_CH);

    ADC_ConversionCompleteCallback();

    return;
  }

#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
  /* Error Interrupt management */
  LL_DMA_ClearFlag(MX_LPDMAX_CHY_CH, LL_DMA_FLAG_ALL);

  /* Check error flags */
  if (((flags & its) & (LL_DMA_FLAG_DTE | LL_DMA_FLAG_USE)) != 0U)
  {
    LL_DMA_ResetChannel(MX_LPDMAX_CHY_CH);

    LL_DMA_DisableIT(MX_LPDMAX_CHY_CH, LL_DMA_IT_ALL);

    ADC_ConversionErrorCallback();
  }
#endif /* USE_LL_APP_ERROR */
}


/**
  * 使用已启用的DWT等待稳压器启动；本例SystemCoreClock固定144MHz。
  * 由CPU周期保证最小等待，不使用依赖优化等级的空循环。
  * param: delay_us Number of microseconds to delay.
  */
static inline void DelayUs(uint32_t delay_us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = (SystemCoreClock / 1000000U) * delay_us;
  while ((DWT->CYCCNT - start) < cycles)
  {
  }
}
