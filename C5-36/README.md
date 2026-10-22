# #36 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0。
基线为官方 `examples/ll/adc/multi_channels_dma`。改写下列四个既有文件，生成层和构建源列表沿用官方；LL TIM/DBGMCU/USART 为头文件内联接口。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/ll/adc/multi_channels_dma/application/example.c` |
| `ll_example.h` | `examples/ll/adc/multi_channels_dma/application/ll_example.h` |
| `main.c` | `examples/ll/adc/multi_channels_dma/application/main.c` |
| `stm32c5xx_ll_example.c` | `examples/ll/adc/multi_channels_dma/application/stm32c5xx_ll_example.c` |

在完整 SDK 的 `examples/ll/adc/multi_channels_dma/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256；编译源清单位于工作目录 logs。

## 固件与运行

`固件/C5_36_debug.elf` 和 `C5_36_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为 Flash 镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

接线：电位器两端接 3V3、GND，中间脚接 CN8 A2（PA4）。串口：ST-LINK 虚拟串口 115200 8N1。
命令 START / STOP / STATUS / HELP（不分大小写）。运行时每 64 ms 一行 `D 块号 平均 最小 最大 mV VREF+mV`。
`STATUS` 输出 lost、overwritten、adc_ovr、dma_err、rx/rx_drop/rx_err/tx_drop、lines/bad/long 计数。
`g_error`：1 时钟、2 采样模块初始化、3 STOP 超时；非 0 时 LD1 进入官方报错闪烁。
`FAULT_DEMO=1`（编译宏）为教学构造故障：每块处理后阻塞 150 ms，lost 应持续增长。

## 相对官方的改动

- ADC：两位次（PA4、VrefInt），TIM6_TRGO 上升沿触发，OVRMOD=0，DMNGT=11（循环），内核 ÷8 = 18 MHz。
- LPDMA1 CH0：自链表节点循环，TCEM=00，HT/TC 中断只给 `g_sampler.halves` 加 1。
- STOP：停 TIM6 → ADSTP → 读空 ADC FIFO → LPDMA 挂起、复位。
- USART2（PA2/PA3 AF7）LL 中断收发，RX 环 128 B、TX 环 512 B；放不下整行即丢并计数；ORE/FE/NE 清除计数。
- 应用层：IDLE/RUN 状态机，命令行规则沿用 #11；乒乓交接“先拷贝、再验计数”。
- main.c：循环延时 1000 ms 改 1 ms。
- 删除官方 ADC_Activate/ADC_Calibrate/ADC_StartConversion_DMA/ADC_GetData* 与回调，由上述函数替代。

本包保留相对官方基线的四个 diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
