/*
 * Firmware image loader helpers.
 *
 * Copyright (C) 2026 ASPEED Technology Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <string.h>
#include <uart_console.h>
#include <manifest.h>
#include <stor.h>

#define POLY            0xEDB88320 /* reflection of 0x04C11DB7 */
#define INITIAL_CRC     0xFFFFFFFF
#define FINAL_XOR       0xFFFFFFFF

static uint32_t crc_table[256];

void generate_crc32_table(void)
{
    uint32_t crc;

    for (uint32_t i = 0; i < 256; i++) {
        crc = i;

        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ POLY : (crc >> 1);
        }

        crc_table[i] = crc;
    }
}

uint32_t calc_checksum(const uint8_t *data, size_t length)
{
    uint32_t crc = INITIAL_CRC;

    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF];
    }

    return crc ^ FINAL_XOR;
}

int ast_loader_read(void *dst, uint64_t src, uint32_t len)
{
    uprintf("%s: dst=0x%lx, src=0x%lx, len=%d\n",
            __func__, dst, src, len);
    memcpy(dst, (void *)(uintptr_t)src, len);

    return CPTRA_SUCCESS;
}

int _ast_loader_load_image(uint32_t type, uint32_t *dst)
{
    const struct image_info *img_info = stor_get_image_info_table();
    uint32_t sz = 0;

    uprintf("%s: %s, type=%d, dst=0x%lx\n", __func__,
            cptra_ime_get_image_name(img_info[type].id), type, dst);
    stor_load(type, dst, &sz);

    return CPTRA_SUCCESS;
}

int ast_loader_load_image(uint32_t type, uint32_t *dst)
{
    return _ast_loader_load_image(type, dst);
}

int ast_loader_load_manifest_image(uint32_t type, uint32_t *dst)
{
    return _ast_loader_load_image(type, dst);
}

