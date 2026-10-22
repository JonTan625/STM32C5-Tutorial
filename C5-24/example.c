/**
  ******************************************************************************
  * file           : example.c
  * brief          : example program body
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
#include "example.h"
#include <string.h>

/* C5 #24 改写：给程序装一个“黑匣子”。
 * 一块复位不清零的 RAM 记录区，HardFault 和 NMI 处理函数把现场写进去后软复位，下次开机打印。
 * 三次开机依次演示：
 *   第 1 次：跑官方 ECC 单错/双错（中断方式）→ 打开除零陷阱，故意除以 0 → HardFault；
 *   第 2 次：打印上一次的 HardFault 现场 → 把 SRAM2 双错改成 NMI，再读一次 → NMI；
 *   第 3 次：打印 NMI 现场，结束（状态灯常亮）。再复位一次从头开始。
 */

/* Private define ------------------------------------------------------------*/
/* @user: set the timeout period in milliseconds for erasing data */
#define TIMEOUT 1000U
/* @user: SRAM_SINGLE_ERR_ADDR_BASE and SRAM_DOUBLE_ERR_ADDR_BASE must be inside SRAM region*/
#define SRAM_SINGLE_ERR_ADDR_BASE SRAM_BASE
#define SRAM_SINGLE_ERR_ADDR_SIZE (0x200U / 0x04U)
#define SRAM_DOUBLE_ERR_ADDR_BASE (SRAM_BASE + 0x200U)
#define SRAM_DOUBLE_ERR_ADDR_SIZE (0x200U / 0x04U)
#define DATA_VALUE                0xAA55AA55UL
#define SINGLE_ERR_DATA_VALUE     0xAA55AA54UL
#define DOUBLE_ERR_DATA_VALUE     0xAA55AA50UL
#define EXPECTED_SINGLE_ERR_NUM   SRAM_SINGLE_ERR_ADDR_SIZE
#define EXPECTED_DOUBLE_ERR_NUM   SRAM_DOUBLE_ERR_ADDR_SIZE

#define LOG_MAGIC                 0xC5240BADUL
#define FAULT_NONE                0U
#define FAULT_HARD                1U
#define FAULT_NMI                 2U

/* Private typedef -----------------------------------------------------------*/
typedef struct
{
  uint32_t magic;
  uint32_t stage;     /* 下一次开机要做哪一步：0 → 1 → 2 → 3 */
  uint32_t type;      /* 最近一次记下的是什么：FAULT_HARD / FAULT_NMI */
  uint32_t cfsr;      /* SCB->CFSR，可配置故障状态 */
  uint32_t hfsr;      /* SCB->HFSR */
  uint32_t mmfar;     /* SCB->MMFAR（与 BFAR 共用存储，只在对应 VALID 位为 1 时有效） */
  uint32_t bfar;      /* SCB->BFAR */
  uint32_t pc;        /* 异常栈帧里的 PC：出事的那条指令 */
  uint32_t lr;
  uint32_t xpsr;
  uint32_t ecc_isr;   /* RAMCFG_M2ISR */
  uint32_t ecc_dear;  /* RAMCFG_M2DEAR：双错地址 */
} fault_log_t;

/* Private variables ---------------------------------------------------------*/
hal_ramcfg_t RamcfgInstance;
/* Single and Double ECC error flags declaration */
uint32_t ECC_SingleError = 0U;
uint32_t ECC_DoubleError = 0U;
/* Single and double error data buffers */
uint32_t SingleErrorData_Buffer[SRAM_SINGLE_ERR_ADDR_SIZE] = {0U};
uint32_t DoubleErrorData_Buffer[SRAM_DOUBLE_ERR_ADDR_SIZE] = {0U};

/* 链接脚本里没有这个段：GCC 把它当孤儿段放在 .data 之后、.bss 之前，
   启动代码既不给它赋初值也不清零（#12 第 08 节，作者实板验证过同样的放法）。 */
__attribute__((section(".c5_fault_log"))) volatile fault_log_t g_log;
uint32_t g_reset_flags;
volatile uint32_t g_zero;     /* 永远是 0，用来做除数；volatile 防止编译器提前算掉 */
volatile uint32_t g_quotient;

/* Private functions prototype -----------------------------------------------*/
static app_status_t ecc_demo(void);
static void ecc_arm_double_error(uint32_t interrupt);
static void print_log(void);
static void FillMemory(uint32_t *pMemory, uint16_t MemorySize, uint32_t Data);
static void ReadMemory(uint32_t *pMemory, uint32_t *pBuffer, uint16_t BufferSize);
static app_status_t CheckBuffer(uint32_t *pBuffer, uint16_t BufferSize, uint32_t CompareData);
void fault_save(const uint32_t *frame, uint32_t type);

app_status_t app_init(void)
{
  /* Getting Ramcfg Instance */
  RamcfgInstance = mx_example_ramcfg_init();

  g_reset_flags = HAL_RCC_GetResetSource();
  HAL_RCC_ClearResetFlags();
  if ((g_log.magic != LOG_MAGIC) || (g_log.stage >= 3U))
  {
    (void)memset((void *)&g_log, 0, sizeof(g_log)); /* 上电后内容随机：认不出标记就从头开始 */
    g_log.magic = LOG_MAGIC;
  }
  PRINTF("[C5#24] reset flags=0x%08" PRIX32 ", stage=%" PRIu32 "\n", g_reset_flags, g_log.stage);
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  switch (g_log.stage)
  {
    case 0U:
      if (ecc_demo() != EXEC_STATUS_OK)
      {
        return EXEC_STATUS_ERROR;
      }
      PRINTF("[C5#24] ECC demo: single=%" PRIu32 " double=%" PRIu32 "\n", ECC_SingleError, ECC_DoubleError);
      g_log.stage = 1U;
      PRINTF("[C5#24] enable DIV_0_TRP and divide by zero...\n");
      SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;  /* 复位值为 0：不开时除零静默得 0（PM0264 CCR） */
      __DSB();
      __ISB();
      g_quotient = 100U / g_zero;
      PRINTF("[C5#24] should not get here\n");
      return EXEC_STATUS_ERROR;

    case 1U:
      print_log();
      g_log.stage = 2U;
      PRINTF("[C5#24] SRAM2 double error with ECCNMI, read it...\n");
      ecc_arm_double_error(HAL_RAMCFG_IT_ECC_DOUBLE_NMI);
      ReadMemory((uint32_t *)SRAM_DOUBLE_ERR_ADDR_BASE, DoubleErrorData_Buffer, 1U);
      PRINTF("[C5#24] should not get here\n");
      return EXEC_STATUS_ERROR;

    case 2U:
      print_log();
      g_log.stage = 3U;
      PRINTF("[C5#24] done, reset again to restart\n");
      return EXEC_STATUS_OK;

    default:
      return EXEC_STATUS_ERROR;
  }
} /* end app_process */

/** ########## Step 7 ##########
  * Disables and stops the ECC monitoring of the selected RAMCFG instance before leaving the scenario.
  */
app_status_t app_deinit(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  if (HAL_RAMCFG_ECC_Disable(RamcfgInstance) == HAL_OK)
  {
    return_status = EXEC_STATUS_OK;
  }
  return return_status;
} /* end app_deinit */

/* 官方 Step 2～6：先在 ECC 开着时写正确数据，关 ECC 改 1 位/2 位，再开 ECC 读回 */
static app_status_t ecc_demo(void)
{
  if (HAL_RAMCFG_MassErase(RamcfgInstance, TIMEOUT) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  if (HAL_RAMCFG_ECC_Enable(RamcfgInstance) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  FillMemory((uint32_t *)SRAM_SINGLE_ERR_ADDR_BASE, (SRAM_SINGLE_ERR_ADDR_SIZE * 2U), DATA_VALUE);
  if (HAL_RAMCFG_ECC_Disable(RamcfgInstance) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  FillMemory((uint32_t *)SRAM_SINGLE_ERR_ADDR_BASE, SRAM_SINGLE_ERR_ADDR_SIZE, SINGLE_ERR_DATA_VALUE);
  FillMemory((uint32_t *)SRAM_DOUBLE_ERR_ADDR_BASE, SRAM_DOUBLE_ERR_ADDR_SIZE, DOUBLE_ERR_DATA_VALUE);
  if (HAL_RAMCFG_ECC_Enable_IT(RamcfgInstance, (HAL_RAMCFG_IT_ECC_SINGLE | HAL_RAMCFG_IT_ECC_DOUBLE)) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  ReadMemory((uint32_t *)SRAM_SINGLE_ERR_ADDR_BASE, SingleErrorData_Buffer, SRAM_SINGLE_ERR_ADDR_SIZE);
  ReadMemory((uint32_t *)SRAM_DOUBLE_ERR_ADDR_BASE, DoubleErrorData_Buffer, SRAM_DOUBLE_ERR_ADDR_SIZE);
  if ((CheckBuffer(SingleErrorData_Buffer, SRAM_SINGLE_ERR_ADDR_SIZE, DATA_VALUE) != EXEC_STATUS_OK)
      || (CheckBuffer(DoubleErrorData_Buffer, SRAM_DOUBLE_ERR_ADDR_SIZE, DOUBLE_ERR_DATA_VALUE) != EXEC_STATUS_OK)
      || (ECC_SingleError != EXPECTED_SINGLE_ERR_NUM) || (ECC_DoubleError != EXPECTED_DOUBLE_ERR_NUM))
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_OK;
}

/* 同官方步骤造一片双错数据，只是重新打开 ECC 时换成指定的中断方式 */
static void ecc_arm_double_error(uint32_t interrupt)
{
  (void)HAL_RAMCFG_MassErase(RamcfgInstance, TIMEOUT);
  (void)HAL_RAMCFG_ECC_Enable(RamcfgInstance);
  FillMemory((uint32_t *)SRAM_DOUBLE_ERR_ADDR_BASE, SRAM_DOUBLE_ERR_ADDR_SIZE, DATA_VALUE);
  (void)HAL_RAMCFG_ECC_Disable(RamcfgInstance);
  FillMemory((uint32_t *)SRAM_DOUBLE_ERR_ADDR_BASE, SRAM_DOUBLE_ERR_ADDR_SIZE, DOUBLE_ERR_DATA_VALUE);
  (void)HAL_RAMCFG_ECC_Enable_IT(RamcfgInstance, interrupt);
}

static void print_log(void)
{
  PRINTF("[C5#24] last fault: %s\n", (g_log.type == FAULT_HARD) ? "HardFault" :
         ((g_log.type == FAULT_NMI) ? "NMI" : "none"));
  PRINTF("  CFSR=0x%08" PRIX32 " HFSR=0x%08" PRIX32 " MMFAR=0x%08" PRIX32 " BFAR=0x%08" PRIX32 "\n",
         g_log.cfsr, g_log.hfsr, g_log.mmfar, g_log.bfar);
  PRINTF("  PC=0x%08" PRIX32 " LR=0x%08" PRIX32 " xPSR=0x%08" PRIX32 "\n", g_log.pc, g_log.lr, g_log.xpsr);
  PRINTF("  RAMCFG_M2ISR=0x%08" PRIX32 " M2DEAR=0x%08" PRIX32 "\n", g_log.ecc_isr, g_log.ecc_dear);
}

/** RAMCFG ECC ErrorCallback
  * 只在第 1 次开机的中断方式下由 RAMCFG_IRQHandler 调用（计数）；NMI 方式由下面的 NMI_Handler 直接处理。
  */
hal_status_t HAL_RAMCFG_ECC_ErrorCallback(hal_ramcfg_t hramcfg)
{
  hal_ramcfg_ecc_info_t p_info;

  HAL_RAMCFG_ECC_GetInfo(hramcfg, &p_info);
  if (p_info.type == HAL_RAMCFG_ECC_SINGLE)
  {
    ECC_SingleError++;
  }
  else
  {
    ECC_DoubleError++;
  }
  return HAL_OK;
}

/* 异常入口：按 EXC_RETURN 第 2 位判断压栈用的是 MSP 还是 PSP，把栈帧地址交给 C 函数 */
__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile(
    "tst   lr, #4      \n"
    "ite   eq          \n"
    "mrseq r0, msp     \n"
    "mrsne r0, psp     \n"
    "movs  r1, #1      \n"  /* FAULT_HARD */
    "b     fault_save  \n");
}

__attribute__((naked)) void NMI_Handler(void)
{
  __asm volatile(
    "tst   lr, #4      \n"
    "ite   eq          \n"
    "mrseq r0, msp     \n"
    "mrsne r0, psp     \n"
    "movs  r1, #2      \n"  /* FAULT_NMI */
    "b     fault_save  \n");
}

/* 栈帧顺序：R0 R1 R2 R3 R12 LR PC xPSR（PM0264 异常入栈） */
__attribute__((used)) void fault_save(const uint32_t *frame, uint32_t type)
{
  g_log.type     = type;
  g_log.cfsr     = SCB->CFSR;
  g_log.hfsr     = SCB->HFSR;
  g_log.mmfar    = SCB->MMFAR;
  g_log.bfar     = SCB->BFAR;
  g_log.lr       = frame[5];
  g_log.pc       = frame[6];
  g_log.xpsr     = frame[7];
  g_log.ecc_isr  = RAMCFG_SRAM2->ISR;
  g_log.ecc_dear = RAMCFG_SRAM2->DEAR;
  RAMCFG_SRAM2->ICR = RAMCFG_ICR_CDED;  /* 不清 DED 的话 NMI 会一直挂着 */
  __DSB();
  NVIC_SystemReset();
}

static void FillMemory(uint32_t *pMemory, uint16_t MemorySize, uint32_t Data)
{
  /* Repeats for all buffer size */
  for (uint32_t IndexTmp = 0; IndexTmp < MemorySize; IndexTmp++)
  {
    pMemory[IndexTmp] = Data;
  }
}

/** This pragma is added only for event counting in this example and is not
  * a functional requirement for ECC operation.
  */
#if defined(__ICCARM__)
#pragma optimize = none
#endif /* defined(__ICCARM__) */
static void ReadMemory(uint32_t *pMemory, uint32_t *pBuffer, uint16_t BufferSize)
{
  for (uint32_t IndexTmp = 0; IndexTmp < BufferSize; IndexTmp++)
  {
    pBuffer[IndexTmp] = pMemory[IndexTmp];
  }
}
#if defined(__ICCARM__)
#if defined(RELEASE_IAR)
#pragma optimize = high
#else
#pragma optimize = low
#endif /* defined(RELEASE_IAR) */
#endif /* defined(__ICCARM__) */

static app_status_t CheckBuffer(uint32_t *pBuffer, uint16_t BufferSize, uint32_t CompareData)
{
  /* Repeats for all buffer size */
  for (uint32_t IndexTmp = 0; IndexTmp < BufferSize; IndexTmp++)
  {
    if (pBuffer[IndexTmp] != CompareData)
    {
      return EXEC_STATUS_ERROR;
    }
  }
  return EXEC_STATUS_OK;
}
