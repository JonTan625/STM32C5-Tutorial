# #30 代码使用说明

状态：以文章开头的“本篇状态”为准。

改写程序基于 `examples/hal/adc/single_conversion_trigger_sw_it`，只改 `application/example.c` 与 `flags.cmake`：

- 规则组改为两个通道：第 1 位 PA4（ADC1_IN4，接电位器滑动端），第 2 位 VrefInt；溢出模式改为保留（ADC_DR 带 8 级 FIFO）；
- 每次报告连续转换 16 次，打印 PA4 的平均、最小、最大、峰峰值；用 VREFINT_CAL 求实际 VREF+，PA4 毫伏按实测 VREF+ 与官方的 3300 mV 假设各算一次；
- PA4 的采样时间收成宏 `PA4_SAMPLING_TIME`（练习用）；等待转换加 10 ms 超时；
- `USE_TRACE` 改为 1，结果经 USART2 打印。

**接线：10 kΩ 线性电位器，两端接 CN6 第 4 脚（3V3）和第 6 脚（GND），滑动端接 CN8 第 3 脚（A2，PA4）；不接 5V；先拔 USB 再接线。**

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录。
2. 备份原文件，按下表把替换文件放回官方相对路径。
3. 进入 `examples/hal/adc/single_conversion_trigger_sw_it/NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `example.c` | `examples/hal/adc/single_conversion_trigger_sw_it/application/example.c` | `example_c_改动.diff` |
| `flags.cmake` | `examples/hal/adc/single_conversion_trigger_sw_it/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `flags_cmake_改动.diff` |

## 将来上板

打开 ST-LINK 虚拟串口（115200 8N1），烧录 Debug 预设，按正文第 08 节核对输出、读寄存器并填三组读数表；需要电位器和万用表，不需要按键。
