/*
 * Caliptra manifest storage helpers.
 *
 * Query firmware image offset and size from Caliptra Manifest and provide
 * stor_load() to load images from manifest storage into destination memory.
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

#include <string.h>
#include <uart_console.h>
#include <stor.h>
#include <manifest.h>

#define DEBUG 0

static struct image_info img_info[] = {
    {CPTRA_MANIFEST_FW_ID, 0, 0},
    {CPTRA_FMC_FW_ID, 0, 0},
    {CPTRA_DDR4_IMEM_FW_ID, 0, 0},
    {CPTRA_DDR4_DMEM_FW_ID, 0, 0},
    {CPTRA_DDR4_2D_IMEM_FW_ID, 0, 0},
    {CPTRA_DDR4_2D_DMEM_FW_ID, 0, 0},
    {CPTRA_DDR5_IMEM_FW_ID, 0, 0},
    {CPTRA_DDR5_DMEM_FW_ID, 0, 0},
    {CPTRA_DP_FW_FW_ID, 0, 0},
    {CPTRA_UEFI_FW_ID, 0, 0},
    {CPTRA_ATF_FW_ID, 0, 0},
    {CPTRA_OPTEE_FW_ID, 0, 0},
    {CPTRA_UBOOT_FW_ID, 0, 0},
    {CPTRA_SSP_FW_ID, 0, 0},
    {CPTRA_TSP_FW_ID, 0, 0},
};

static int stor_get_image_info(struct image_info *info)
{
    struct cptra_image_context *cptra_ctx = cptra_get_ctx();
    uint32_t offset, sz;
    int err;

    if (!info) {
        uprintf("Image info pointer is NULL.\n");
        return -1;
    }

    for (int i = 0; i < CPTRA_TSP_FW_ID + 1; i++) {
        /* Call cptra's service to get the image info */
        err = cptra_get_abb_imginfo(info[i].id, &offset, &sz);
        if (err) {
            uprintf("Failed to get image info for ID %d, err=%d\n",
                    info[i].id, err);
            return err;
        }

        info[i].offset = offset + cptra_ctx->manifest_base;
        info[i].size = sz;
#if DEBUG
        uprintf("%s: offset=0x%lx, size=%d\n",
                cptra_ime_get_image_name(info[i].id),
                info[i].offset, info[i].size);
#endif
    }

    return err;
}

void stor_load(uint32_t type, uint32_t *dst, uint32_t *len)
{
    uint64_t src;
    uint64_t dram_addr;
    uint32_t sz = 0;

    src = img_info[type].offset;
    sz = img_info[type].size;

    /* convert to Arm view */
    dram_addr = convert_mcu_addr_to_arm_dram((uint64_t)dst);
    uprintf("%s: %s, dst=0x%lx, src=0x%lx, len=%d\n", __func__,
            cptra_ime_get_image_name(img_info[type].id),
            dram_addr, (unsigned long long)src, sz);
    memcpy((void *)(uintptr_t)dram_addr, (void *)(uintptr_t)src, sz);
    *len = sz;
}

const struct image_info *stor_get_image_info_table(void)
{
    return img_info;
}

int stor_init(void)
{
    return stor_get_image_info(img_info);
}

