# #30 代码使用说明

状态：以文章开头的“本篇状态”为准。

改写程序基于 `examples/hal/adc/temperature_sensor`，只改 `application/example.c` 与 `flags.cmake`：

- ADC 时钟改为 PSIS/8（18 MHz），采样时间 16.06 µs，满足 DS15135 表 55 的 13 µs；分频在 `HAL_ADC_Start()` 之前改（RM0522 第 316 页）；
- 规则组改为两个通道：第 1 位 VrefInt，第 2 位温度传感器；溢出模式改为保留（ADC_DR 带 8 级 FIFO）；
- 用 VREFINT_CAL 求实际 VREF+，再算温度；同时按 3300 mV 假设算一次对照；
- 温度变量改为 `int32_t`；转换出错时 `goto` 退出；
- `USE_TRACE` 改为 1，结果经 USART2 打印。

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录。
2. 备份原文件，按下表把替换文件放回官方相对路径。
3. 进入 `examples/hal/adc/temperature_sensor/NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `example.c` | `examples/hal/adc/temperature_sensor/application/example.c` | `example_c_改动.diff` |
| `flags.cmake` | `examples/hal/adc/temperature_sensor/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `flags_cmake_改动.diff` |

## 上板复现

打开 ST-LINK 虚拟串口（115200 8N1），烧录 Debug 预设，按正文第 08 节核对输出、第 10 节读寄存器。不需要接线和按键。
