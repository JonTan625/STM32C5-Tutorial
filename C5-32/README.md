# #32 代码使用说明

状态：以文章开头的“本篇状态”为准。

改写程序基于 `examples/ll/adc/multi_channels_dma`，只改 `application` 目录下三个文件，生成代码未动：

- `example.c`：溢出模式改为保留（OVRMOD = 0），连续转换 + DMA 单次，一次读 16 轮 × 3 通道；ADC/DAC 内核时钟 4 分频改 8 分频；
  传完后统计各通道平均、最小、最大，用 VrefInt 求实际 VREF+，PA4 毫伏与温度各按实测 VREF+ 和官方 3300 mV 假设算一次（温度用 `int32_t`）；
  启动 400 µs 后偷看缓冲区，记录已写槽数；记录 DMA 剩余字节、ADSTART、OVR、整次周期数；等待改为 DWT 计时轮询加 20 ms 超时。
- `stm32c5xx_ll_example.c`：缓冲区扩成 48 个半字、声明 `volatile`、去掉 `non_cacheable_area` 段；启动前填哨兵 0xFFFF；删除不再使用的三个 `ADC_GetData*` 函数。
- `ll_example.h`：增加 `ADC_RANKS`、`ADC_SCANS`、`ADC_BUFFER_SIZE`、`ADC_SENTINEL` 与缓冲区声明；删除对应的三个函数原型。

**本例没有串口。**结果都在全局变量里（`g_` 开头），用 IDE 调试器的 Watch 或 ST-LINK_gdbserver 加 gdb 查看。不要在 Watch 里加 ADC1 的 DR 寄存器（会清 EOC）；不要用 CubeProgrammer 热插拔连接读（会清 NVIC 使能）。

PA4 悬空时读数无意义；想让 PA4 有意义，接 #31 的电位器（两端接 CN6 第 4 脚 3V3 与第 6 脚 GND，滑动端接 CN8 第 3 脚 A2，先拔 USB 再接线）。

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录。
2. 备份原文件，按下表把替换文件放回官方相对路径。
3. 进入 `examples/ll/adc/multi_channels_dma/NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `example.c` | `examples/ll/adc/multi_channels_dma/application/example.c` | `example_c_改动.diff` |
| `ll_example.h` | `examples/ll/adc/multi_channels_dma/application/ll_example.h` | `ll_example_h_改动.diff` |
| `stm32c5xx_ll_example.c` | `examples/ll/adc/multi_channels_dma/application/stm32c5xx_ll_example.c` | `stm32c5xx_ll_example_c_改动.diff` |

## 上板复现

烧录 Debug 预设，运行几秒后暂停，按正文第 08 节读全局变量与缓冲区、填表；不需要按键，不需要接线（PA4 想有意义才接电位器）。
