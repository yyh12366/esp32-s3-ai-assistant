// main/av_blufi_security.h —— BLUFI 安全协商回调（DH + AES-CFB + CRC16），供 av_wifi.c 使用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

void av_blufi_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free);
int av_blufi_encrypt(uint8_t iv8, uint8_t *data, int len);
int av_blufi_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t av_blufi_checksum(uint8_t iv8, uint8_t *data, int len);
int av_blufi_security_init(void);
void av_blufi_security_deinit(void);
