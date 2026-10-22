# #28 代码使用说明

状态：以文章开头的“本篇状态”为准。

本篇有两个独立的改写程序，各基于一个官方例程：

- `aes/`：基于 `examples/hal/aes/cipher_gcm`（该工程同时启用了 CBC 和 GCM），字节数组与数据交换、GCM 篡改拒收、常数时间比较；
- `hash/`：基于 `examples/hal/hash/integrity_sha`，SHA-256 标准向量、数据交换对照、HMAC-SHA256（RFC 4231）、1 KB 计时。

**示例里的密钥全部是公开测试向量，只用于验证。产品密钥不要写进源码、生成代码或公开仓库（正文第 07 节）。**

## 恢复工程

1. 从 ST Package Creator 获取 STM32C5 MCU HAL2.1.0 SDK，保留完整目录。
2. 备份原文件，按下表把替换文件放回官方相对路径。
3. 进入对应例程的 `NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC` 与 `cmake --build --preset debug_GCC_NUCLEO-C542RC`；Release 把 debug 换成 release。
4. 产物分别为 `hal_aes_cipher_gcm.elf`、`hal_hash_integrity_sha.elf`；尺寸与哈希见上一级 `固件构建记录.json`。

| 替换文件 | 官方相对路径 | 差异 |
|---|---|---|
| `aes/example.c` | `examples/hal/aes/cipher_gcm/application/example.c` | `aes/example_c_改动.diff` |
| `aes/flags.cmake` | `examples/hal/aes/cipher_gcm/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `aes/flags_cmake_改动.diff` |
| `hash/example.c` | `examples/hal/hash/integrity_sha/application/example.c` | `hash/example_c_改动.diff` |
| `hash/flags.cmake` | `examples/hal/hash/integrity_sha/NUCLEO-C542RC/cmake/cmake/flags.cmake` | `hash/flags_cmake_改动.diff` |

## 上板复现

打开 ST-LINK 虚拟串口（115200 8N1），分别烧录两个程序（Debug 预设），按正文第 09 节核对输出，按第 11 节记录。不需要接线和按键。
