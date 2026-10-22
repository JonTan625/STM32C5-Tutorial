/**
  * C5 #29：STFCF 用户配置文件（STFCF_USER_CONFIG_FILE），在官方生成的
  * mx_mbedtls_config.h、mx_mbedtls_alt_config.h 之后被 mx_stfcf_config.h 包含。
  * 这里取消 AES、GCM 的 ALT，让 PSA API 改用 Mbed TLS 自带的纯软件实现，
  * 用来和默认的 HAL 硬件实现对比。生成代码不需要改。
  */
#ifndef C5_29_SW_GCM_H
#define C5_29_SW_GCM_H

#undef MBEDTLS_GCM_ALT
#undef MBEDTLS_HAL_GCM_ALT
#undef MBEDTLS_AES_ALT
#undef MBEDTLS_HAL_AES_ALT

#endif /* C5_29_SW_GCM_H */
