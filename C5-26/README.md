# #26 代码使用说明

状态：以文章开头的“本篇状态”为准。

本篇有两个独立的改写程序，各基于一个官方例程，互不依赖：

- `crc/`：基于 `examples/hal/crc/default`，CRC 字节顺序、四种常见 CRC 与硬件/软件耗时对比；
- `rng/`：基于 `examples/hal/rng/candidate_nist_config`，默认配置与 HAL candidate NIST 配置对比、错误处理。

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录（官方 CMake 引用 SDK 根目录的驱动和工具）。
2. 备份原文件，按下表把替换文件放回官方相对路径。
3. 进入对应例程的 `NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。
4. 产物分别为 `hal_crc_default.elf`、`hal_rng_candidate_nist_config.elf`；尺寸与哈希见上一级 `固件构建记录.json`。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `crc/example.c` | `examples/hal/crc/default/application/example.c` | `crc/example_c_改动.diff` |
| `crc/flags.cmake` | `examples/hal/crc/default/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `crc/flags_cmake_改动.diff` |
| `rng/example.c` | `examples/hal/rng/candidate_nist_config/application/example.c` | `rng/example_c_改动.diff` |
| `rng/flags.cmake` | `examples/hal/rng/candidate_nist_config/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `rng/flags_cmake_改动.diff` |

## 上板复现

打开 ST-LINK 虚拟串口（115200 8N1），分别烧录两个程序（Debug 预设），按正文第 09 节核对输出，按第 11 节记录。两个程序都不需要接线和按键。
