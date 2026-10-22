/**
  ******************************************************************************
  * file           : stm32c5xx_ll_example.c
  * brief          : ADC multi channels conversion triggered by software in DMA mode with LL API
  *                  C5 #36：采样模块（TIM6 触发 ADC，LPDMA 自链表循环，半传输/传输完成乒乓）
  *                  与终端模块（USART2 中断收发 + 环形缓冲）。
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
/* MX_ADCx delay between end of calibration and enable operations (unit: CPU clock cycles).
   C5 #36：ADC 内核 PSIS ÷8 = 18 MHz，4 个 ADC 周期 = 32 个 CPU 周期。 */
#define ADC_DELAY_CALIB_ENABLE_CPU_CYCLES (LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES * 8U)
#define ADC_TIMEOUT_US                    (2000U)
#define DMA_SUSPEND_TIMEOUT_US            (100U)
#define TIMER_PSC                         (143U)   /* 144 MHz ÷ 144 = 1 MHz 计数 */

/* @user: The maximum data bus width used by DMA in STM32 devices is 64 bits.
          Therefore, 8-byte alignment is the minimum recommended alignment for DMA buffers across STM32 devices. */
#define DMA_ACCESS_ALIGNMENT        (8U)

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* C5 #36：沿用 #32 去掉 non_cacheable_area 段；乒乓两半，DMA 循环写 */
__attribute__((aligned(DMA_ACCESS_ALIGNMENT)))
volatile uint16_t ADCRawData[ADC_BUFFER_SIZE];

/* LPDMA 链表节点：CTR1、CTR2、CBR1、CSAR、CDAR、CLLR 六个字，CLLR 指回自己 → 无限循环（RM0522 §14.4.13） */
static uint32_t s_dma_node[LL_DMA_NODE_REGISTER_NUM];

sampler_stats_t g_sampler;
term_stats_t g_term;

static uint8_t s_rx_ring[TERM_RX_SIZE];
static volatile uint32_t s_rx_head;   /* 只由中断写 */
static volatile uint32_t s_rx_tail;   /* 只由主循环写 */
static uint8_t s_tx_ring[TERM_TX_SIZE];
static volatile uint32_t s_tx_head;   /* 只由主循环写 */
static volatile uint32_t s_tx_tail;   /* 只由中断写 */

/* Private functions prototype -----------------------------------------------*/
void mx_lpdmax_chy_irqhandler(void);
void USART2_IRQHandler(void);
static uint32_t WaitAdc(uint32_t (*is_set)(const ADC_TypeDef *), uint32_t expected);

/* Private functions ---------------------------------------------------------*/

/**
  * C5 #36：DWT 周期计数延时，Debug/Release 下一致；app_init 已开启 DWT。
  */
void DelayUs(uint32_t delay_us)
{
  uint32_t start = DWT->CYCCNT;

  while ((DWT->CYCCNT - start) < (SystemCoreClock / 1000000U) * delay_us)
  {
  }
}


/**
  * C5 #36：生成层 mx_adc1_init 已配好 PA4、VrefInt、温度三个位次（289 周期）和 LPDMA1 CH0 的请求、方向、位宽。
  * 这里改成两个位次、TIM6_TRGO 上升沿触发、单次转换、OVRMOD=0、DMNGT=11（循环），全部在 ADSTART=0 时写
  * （RM0522 ADC_CFGR1 第 643～644 页）。
  */
system_status_t Sampler_Init(void)
{
  uint32_t start;

  /* ADC、DAC 共用内核分频，ADC 使能前改为 ÷8 = 18 MHz（沿用 #32～#34） */
  LL_RCC_SetADCDACPrescaler(LL_RCC_ADCDAC_PRESCALER_8);
  LL_ADC_REG_SetSequencerLength(MX_ADCx, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);
  LL_ADC_REG_SetOverrun(MX_ADCx, LL_ADC_REG_OVR_DATA_PRESERVED);
  LL_ADC_REG_SetContinuousMode(MX_ADCx, LL_ADC_REG_CONV_SINGLE);
  LL_ADC_REG_SetTriggerSource(MX_ADCx, LL_ADC_REG_TRIG_TIM6_TRGO);
  LL_ADC_REG_SetTriggerEdge(MX_ADCx, LL_ADC_REG_TRIG_RISING);
  LL_ADC_REG_SetDataTransferMode(MX_ADCx, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);

  /* TIM6：1 MHz 计数，ARR 999 → 每 1 ms 一次更新事件，TRGO = Update；不开中断（同 #33） */
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_TIM6);
  LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_TIM6);
  LL_TIM_EnableARRPreload(TIM6);
  LL_TIM_SetPrescaler(TIM6, TIMER_PSC);
  LL_TIM_SetAutoReload(TIM6, (1000000U / SAMPLE_RATE_HZ) - 1U);
  LL_TIM_GenerateEvent_UPDATE(TIM6);   /* ADC 尚未 ADSTART，这次 TRGO 不会触发转换 */
  LL_TIM_ClearFlag_UPDATE(TIM6);
  LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
  LL_DBGMCU_APB1_GRP1_FreezePeriph(LL_DBGMCU_TIM6_STOP);

  /* 校准与使能：退出深度掉电 → 稳压器 → 校准 → 等 4 个 ADC 周期 → ADEN（同 #34/#35） */
  LL_ADC_DisableDeepPowerDown(MX_ADCx);
  LL_ADC_EnableInternalRegulator(MX_ADCx);
  DelayUs(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);
  LL_ADC_StartCalibration(MX_ADCx, LL_ADC_IN_SINGLE_ENDED);
  if (WaitAdc(LL_ADC_IsCalibrationOnGoing, 0U) == 0U)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }
  start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < ADC_DELAY_CALIB_ENABLE_CPU_CYCLES)
  {
  }
  LL_ADC_ClearFlag_ADRDY(MX_ADCx);
  start = DWT->CYCCNT;
  while (LL_ADC_IsActiveFlag_ADRDY(MX_ADCx) == 0U)
  {
    /* 校准结束后过早写 ADEN 会被清掉，未使能时重写（官方 ADC_Activate 同样处理） */
    if (LL_ADC_IsEnabled(MX_ADCx) == 0U)
    {
      LL_ADC_Enable(MX_ADCx);
    }
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * ADC_TIMEOUT_US)
    {
      return SYSTEM_PERIPHERAL_ERROR;
    }
  }
  return SYSTEM_OK;
}


/**
  * C5 #36：链表模式启动。寄存器里的 LLI0 长度为 0（空传输，不产生 HT/TC，RM0522 第 431 页 TCEM 注），
  * 使能后立即从节点装载全部寄存器；节点 CBR1 = 512 字节，TCEM = 00 → 每块中点 HT、块末 TC，
  * 块结束后再次装载同一节点，目的地址回到缓冲区开头。
  */
void Sampler_Start(void)
{
  s_dma_node[LL_DMA_NODE_CTR1_REG_OFFSET] = LL_DMA_SRC_ADDR_FIXED | LL_DMA_SRC_DATA_WIDTH_HALFWORD
                                            | LL_DMA_DEST_ADDR_INCREMENTED | LL_DMA_DEST_DATA_WIDTH_HALFWORD;
  s_dma_node[LL_DMA_NODE_CTR2_REG_OFFSET] = LL_LPDMA1_REQUEST_ADC1 | LL_DMA_DIRECTION_PERIPH_TO_MEMORY
                                            | LL_DMA_LINKEDLIST_XFER_EVENT_BLOCK;
  s_dma_node[LL_DMA_NODE_CBR1_REG_OFFSET] = ADC_BUFFER_SIZE * sizeof(uint16_t);
  s_dma_node[LL_DMA_NODE_CSAR_REG_OFFSET] = LL_ADC_DMA_GetRegAddr(MX_ADCx, LL_ADC_DMA_REG_REGULAR_DATA);
  s_dma_node[LL_DMA_NODE_CDAR_REG_OFFSET] = (uint32_t)ADCRawData;
  s_dma_node[LL_DMA_NODE_CLLR_REG_OFFSET] = LL_DMA_UPDATE_ALL | ((uint32_t)s_dma_node & DMA_CLLR_LA);
  __DMB();   /* 节点写完再让 DMA 去读 */

  g_sampler.halves = 0U;
  LL_DMA_ConfigControl(MX_LPDMAX_CHY_CH, LL_DMA_PRIORITY_HIGH | LL_DMA_LINKEDLIST_EXECUTION_Q);
  LL_DMA_SetTransferEventMode(MX_LPDMAX_CHY_CH, LL_DMA_LINKEDLIST_XFER_EVENT_BLOCK);
  LL_DMA_SetBlkDataLength(MX_LPDMAX_CHY_CH, 0U);
  LL_DMA_SetLinkedListBaseAddr(MX_LPDMAX_CHY_CH, (uint32_t)s_dma_node);
  LL_DMA_ConfigLinkUpdate(MX_LPDMAX_CHY_CH, LL_DMA_UPDATE_ALL, (uint32_t)s_dma_node);
  LL_DMA_ClearFlag(MX_LPDMAX_CHY_CH, LL_DMA_FLAG_ALL);
  LL_DMA_EnableIT(MX_LPDMAX_CHY_CH, LL_DMA_IT_HT | LL_DMA_IT_TC | LL_DMA_IT_DTE | LL_DMA_IT_ULE | LL_DMA_IT_USE);
  LL_DMA_EnableChannel(MX_LPDMAX_CHY_CH);

  /* EXTEN != 0：ADSTART 只是开始接收外部触发；TIM6 仍停着，所以此刻不会转换（RM0522 §21.4.19，#33 已核） */
  LL_ADC_ClearFlag_OVR(MX_ADCx);
  LL_ADC_REG_StartConversion(MX_ADCx);
  LL_TIM_SetCounter(TIM6, 0U);
  LL_TIM_EnableCounter(TIM6);
}


/**
  * C5 #36：停的顺序——先停触发源，再 ADSTP 复位序列（RM0522 §21.4.18 第 583 页），
  * 把 ADC_DR 的 8 级 FIFO 读空（FIFO 未空时 EOC 会再次置位，第 588、591 页），
  * 最后按 §14.4.4（第 392～393 页）挂起 → 等 SUSPF → 复位 LPDMA 通道。
  * 不读空 FIFO 的话，下次 START 可能先搬走上一轮的残留数据，PA4 与 VrefInt 的位置就错开了（推断）。
  */
uint32_t Sampler_Stop(void)
{
  uint32_t ok = 1U;
  uint32_t start;
  uint32_t i;

  LL_TIM_DisableCounter(TIM6);
  if (LL_ADC_REG_IsConversionOngoing(MX_ADCx) != 0U)
  {
    LL_ADC_REG_StopConversion(MX_ADCx);
  }
  if (WaitAdc(LL_ADC_REG_IsConversionOngoing, 0U) == 0U)
  {
    ok = 0U;
  }
  for (i = 0U; (i < 16U) && (LL_ADC_IsActiveFlag_EOC(MX_ADCx) != 0U); i++)
  {
    (void)LL_ADC_REG_ReadConversionData12(MX_ADCx);
  }
  LL_ADC_ClearFlag_OVR(MX_ADCx);

  LL_DMA_SuspendChannel(MX_LPDMAX_CHY_CH);
  start = DWT->CYCCNT;
  while (LL_DMA_IsActiveFlag_SUSP(MX_LPDMAX_CHY_CH) == 0U)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * DMA_SUSPEND_TIMEOUT_US)
    {
      ok = 0U;
      break;
    }
  }
  LL_DMA_ResetChannel(MX_LPDMAX_CHY_CH);
  LL_DMA_DisableIT(MX_LPDMAX_CHY_CH, LL_DMA_IT_ALL);
  LL_DMA_ClearFlag(MX_LPDMAX_CHY_CH, LL_DMA_FLAG_ALL);
  return ok;
}


uint32_t Sampler_IsOverrun(void)
{
  return LL_ADC_IsActiveFlag_OVR(MX_ADCx);
}


/**
  * C5 #36：LPDMA1 CH0 中断。只清标志、加计数，数据留给主循环；HT 先于 TC 处理，迟到时两者都计。
  */
void mx_lpdmax_chy_irqhandler(void)
{
  uint32_t flags = LL_DMA_READ_REG(MX_LPDMAX_CHY_CH, CSR);

  if ((flags & LL_DMA_FLAG_HT) != 0U)
  {
    LL_DMA_ClearFlag_HT(MX_LPDMAX_CHY_CH);
    g_sampler.halves++;
  }
  if ((flags & LL_DMA_FLAG_TC) != 0U)
  {
    LL_DMA_ClearFlag_TC(MX_LPDMAX_CHY_CH);
    g_sampler.halves++;
  }
  if ((flags & (LL_DMA_FLAG_DTE | LL_DMA_FLAG_ULE | LL_DMA_FLAG_USE)) != 0U)
  {
    LL_DMA_ClearFlag(MX_LPDMAX_CHY_CH, LL_DMA_FLAG_DTE | LL_DMA_FLAG_ULE | LL_DMA_FLAG_USE);
    g_sampler.last_csr = flags;
    g_sampler.dma_errors++;
  }
}


/**
  * C5 #36：USART2 = ST-LINK 虚拟串口，PA2 TX / PA3 RX，AF7（官方 hal/uart/echo_it 生成层；UM3615 表 11）。
  * 时钟 PCLK1 = 144 MHz（复位默认源，官方 ll/usart 例程生成层同此）；FIFO 不开，RXNEIE 收、TXEIE 发。
  */
void Term_Init(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_2, LL_GPIO_AF_7);
  LL_GPIO_SetAFPin_0_7(GPIOA, LL_GPIO_PIN_3, LL_GPIO_AF_7);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_2, LL_GPIO_MODE_ALTERNATE);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_3, LL_GPIO_MODE_ALTERNATE);

  LL_USART_ConfigAsyncMode(USART2);
  LL_USART_ConfigCharacter(USART2, LL_USART_DATAWIDTH_8_BIT, LL_USART_PARITY_NONE, LL_USART_STOP_BIT_1);
  LL_USART_SetTransferDirection(USART2, LL_USART_DIRECTION_TX_RX);
  LL_USART_SetOverSampling(USART2, LL_USART_OVERSAMPLING_16);
  LL_USART_SetBaudRate(USART2, SystemCoreClock, LL_USART_PRESCALER_DIV1, LL_USART_OVERSAMPLING_16, TERM_BAUD);
  LL_USART_Enable(USART2);
  LL_USART_EnableIT_RXNE_RXFNE(USART2);

  NVIC_SetPriority(USART2_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 1, 0));
  NVIC_EnableIRQ(USART2_IRQn);
}


uint32_t Term_GetByte(uint8_t *p_byte)
{
  uint32_t tail = s_rx_tail;

  if (tail == s_rx_head)
  {
    return 0U;
  }
  *p_byte = s_rx_ring[tail];
  s_rx_tail = (tail + 1U) & (TERM_RX_SIZE - 1U);
  return 1U;
}


/**
  * C5 #36：整段入环，放不下就整段丢弃并计数——这是“串口发不过来”时的确定策略。
  * 顺序：先写数据 → 再移 head → 最后开 TXEIE；中断看到空环会自己关 TXEIE。
  */
uint32_t Term_Write(const char *p, uint32_t len)
{
  uint32_t head = s_tx_head;
  uint32_t used = (head - s_tx_tail) & (TERM_TX_SIZE - 1U);
  uint32_t i;

  if (len > (TERM_TX_SIZE - 1U - used))
  {
    g_term.tx_dropped++;
    return 0U;
  }
  for (i = 0U; i < len; i++)
  {
    s_tx_ring[head] = (uint8_t)p[i];
    head = (head + 1U) & (TERM_TX_SIZE - 1U);
  }
  __DMB();
  s_tx_head = head;
  LL_USART_EnableIT_TXE_TXFNF(USART2);
  return 1U;
}


/**
  * C5 #36：ORE 置位时 RXNEIE 也会进中断（RM0522 §43.8.2 USART_CR1 [FIFO 关闭] 第 1856 页），必须写 ORECF 清掉（第 1884 页），
  * 否则会反复进中断。FE、NE 不开中断，但随数据一起出现时顺手清掉并计数。
  */
void USART2_IRQHandler(void)
{
  uint32_t next;
  uint8_t c;

  if (LL_USART_IsActiveFlag_ORE(USART2) != 0U)
  {
    LL_USART_ClearFlag_ORE(USART2);
    g_term.rx_errors++;
  }
  if ((LL_USART_IsActiveFlag_FE(USART2) != 0U) || (LL_USART_IsActiveFlag_NE(USART2) != 0U))
  {
    LL_USART_ClearFlag_FE(USART2);
    LL_USART_ClearFlag_NE(USART2);
    g_term.rx_errors++;
  }
  if (LL_USART_IsActiveFlag_RXNE_RXFNE(USART2) != 0U)
  {
    c = LL_USART_ReceiveData8(USART2);   /* 读 RDR 清 RXNE */
    g_term.rx_bytes++;
    next = (s_rx_head + 1U) & (TERM_RX_SIZE - 1U);
    if (next != s_rx_tail)
    {
      s_rx_ring[s_rx_head] = c;
      s_rx_head = next;
    }
    else
    {
      g_term.rx_dropped++;   /* 主循环来不及取：丢这个字节，接收不停（同 #11） */
    }
  }
  if ((LL_USART_IsEnabledIT_TXE_TXFNF(USART2) != 0U) && (LL_USART_IsActiveFlag_TXE_TXFNF(USART2) != 0U))
  {
    if (s_tx_tail != s_tx_head)
    {
      LL_USART_TransmitData8(USART2, s_tx_ring[s_tx_tail]);
      s_tx_tail = (s_tx_tail + 1U) & (TERM_TX_SIZE - 1U);
    }
    else
    {
      LL_USART_DisableIT_TXE_TXFNF(USART2);
    }
  }
}


/**
  * C5 #36：等待 ADC 状态位变为 expected，超时返回 0。
  */
static uint32_t WaitAdc(uint32_t (*is_set)(const ADC_TypeDef *), uint32_t expected)
{
  uint32_t start = DWT->CYCCNT;

  while (is_set(MX_ADCx) != expected)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * ADC_TIMEOUT_US)
    {
      return 0U;
    }
  }
  return 1U;
}
