# #29 代码使用说明

状态：以文章开头的“本篇状态”为准。

改写程序基于 `examples/middleware/stfcf/cipher_aes_gcm`：同一个 `example.c`，默认配置下 GCM 走 AES 硬件（官方配置不变）；以 `-DC5_29_SW=ON` 配置时，经 STFCF 用户配置文件 `c5_29_sw_gcm.h` 改走 Mbed TLS 纯软件。程序用 DWT 计一次加密、一次解密的周期数，并做一次篡改解密，看库是否拒收、是否清零输出。

**示例里的密钥是 NIST 公开测试向量，只用于验证。产品密钥不要写进源码、生成代码或公开仓库。本程序不改选项字节，不碰 RDP、HDP、OTP。**

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录。
2. 备份原文件，按下表把文件放回官方相对路径（`c5_29_sw_gcm.h` 为新增）。

| 本目录文件 | 官方相对路径 | 差异 |
|---|---|---|
| `c5_29_sw_gcm.h` | `examples/middleware/stfcf/cipher_aes_gcm/application/c5_29_sw_gcm.h` | 新增文件 |
| `example.c` | `examples/middleware/stfcf/cipher_aes_gcm/application/example.c` | `example_c_改动.diff` |
| `flags.cmake` | `examples/middleware/stfcf/cipher_aes_gcm/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `flags_cmake_改动.diff` |

## 构建

进入 `examples/middleware/stfcf/cipher_aes_gcm/NUCLEO-C542RC/cmake`：

- 硬件版：`cmake --preset debug_GCC_NUCLEO-C542RC`，再 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；
- 软件版：`cmake --preset debug_GCC_NUCLEO-C542RC -B build/sw_debug_GCC_NUCLEO-C542RC -DC5_29_SW=ON`，再 `cmake --build build/sw_debug_GCC_NUCLEO-C542RC`。

Release 把 debug 换成 release。两版的 ELF 都叫 `stfcf_cipher_aes_gcm.elf`，在不同的构建目录里，烧录时别拿错。尺寸、符号与哈希见上一级 `固件构建记录.json`。

## 上板复现

打开 ST-LINK 虚拟串口（115200 8N1），先后烧录硬件版和软件版，按正文第 06 节核对输出，按第 11 节记录周期数。不需要接线和按键。
