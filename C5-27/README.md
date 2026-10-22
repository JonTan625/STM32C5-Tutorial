# #27 代码使用说明

状态：以文章开头的“本篇状态”为准。

改写程序基于 `examples/hal/cordic/sincos_polling`（NUCLEO-C542RC），只修改 `application/example.c`；生成代码、构建文件未改动。本工程没有串口，结果在全局变量里，用调试器 Watch 查看。

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录（官方 CMake 引用 SDK 根目录的驱动、CMSIS-DSP 和工具）。
2. 备份原文件，用本目录的 `example.c` 覆盖 `examples/hal/cordic/sincos_polling/application/example.c`。
3. 进入 `NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。
4. 产物为 `hal_cordic_sincos_polling.elf`；尺寸与哈希见上一级 `固件构建记录.json`。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `example.c` | `examples/hal/cordic/sincos_polling/application/example.c` | `example_c_改动.diff` |

## 上板复现

烧录 Debug 预设，运行到 LD1 常亮（`success_handler`）后暂停，在 Watch 里按正文第 08 节核对变量；再烧录 Release 预设记录耗时。不需要接线和按键。
