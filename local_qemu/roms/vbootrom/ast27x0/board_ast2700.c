/*
 * Caliptra manifest boot image loader.
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
#include <libfdt.h>
#include <image.h>
#include <uart_console.h>
#include <manifest.h>
#include <stor.h>
#include <ast_loader.h>
#include <io.h>
#include <ssp_tsp.h>
#include <platform_ast2700.h>

#define PSP_TIMEOUT 100000000ULL
#define MANIFEST_SEARCH_START (ASPEED_FMC_CS0_BASE)
#define MANIFEST_SEARCH_END   (MANIFEST_SEARCH_START + 0x400000)
#define MANIFEST_SEARCH_STEP  0x10000

static struct reserved_mem_info reservedinfo = {
    .ssp = { .addr = 0x42c000000ULL, .size = 0x02000000ULL },
    .tsp = { .addr = 0x42e000000ULL, .size = 0x02000000ULL },
    .atf = { .addr = 0x430000000ULL, .size = 0x00080000ULL },
    .tee = { .addr = 0x430080000ULL, .size = 0x01000000ULL },
    .ipc_ssp = { .addr = 0x431080000ULL, .size = 0x00800000ULL }
};

static bool has_pspfw;
static bool has_sspfw;
static bool has_tspfw;

extern void panic(const char *);

void board_manifest_image_post_process(uint32_t fw_id)
{
    uintptr_t ep = cptra_ime_get_load_addr(fw_id);
    uint64_t ep_arm = 0;

    /* convert to Arm view */
    ep_arm = convert_mcu_addr_to_arm_dram((uint64_t)ep);

    switch (fw_id) {
    case CPTRA_SSP_FW_ID:
        ssp_init(ep_arm, &reservedinfo);
        has_sspfw = true;
        break;
    case CPTRA_ATF_FW_ID:
        has_pspfw = true;
        writel(ep_arm >> 4, SCU0_CA35_RVBAR0);
        writel(ep_arm >> 4, SCU0_CA35_RVBAR1);
        writel(ep_arm >> 4, SCU0_CA35_RVBAR2);
        writel(ep_arm >> 4, SCU0_CA35_RVBAR3);
        break;
    case CPTRA_UBOOT_FW_ID:
        writel((uint32_t)ep_arm, SCU0_CPU_SMP_EP0);
        writel((uint32_t)(ep_arm >> 32), SCU0_CPU_SMP_EP0 + 4);
        break;
    case CPTRA_TSP_FW_ID:
        tsp_init(ep_arm, &reservedinfo);
        has_tspfw = true;
        break;
    default:
        break;
    }
}

static void board_prepare_for_boot(void)
{
    if (has_pspfw) {
        /* clean up secondary entries */
        writel(0x0, SCU0_CPU_SMP_EP1);
        writel(0x0, SCU0_CPU_SMP_EP1 + 4);
        writel(0x0, SCU0_CPU_SMP_EP2);
        writel(0x0, SCU0_CPU_SMP_EP2 + 4);
        writel(0x0, SCU0_CPU_SMP_EP3);
        writel(0x0, SCU0_CPU_SMP_EP3 + 4);

        /* release CA35 reset */
        writel(0x1, SCU0_CA35_REL);
    }

    /* release SSP reset */
    if (has_sspfw) {
        ssp_enable();
    }

    /* release TSP reset */
    if (has_tspfw) {
        tsp_enable();
    }
}

static int find_manifest_image(uint64_t start_addr, uint64_t end_addr,
                               uint64_t search_step, uint64_t *manifest_base)
{
    struct cptra_checksum_info *chk;
    struct cptra_manifest_hdr *hdr;
    uint32_t hdr_crc32 = 0;
    uint32_t hdr_size;
    uint32_t chk_size;
    uint64_t addr;

    hdr_size = sizeof(struct cptra_manifest_hdr);
    chk_size = sizeof(struct cptra_checksum_info);

    for (addr = start_addr;
         (addr + hdr_size + chk_size) <= end_addr;
         addr += search_step) {

        hdr = (struct cptra_manifest_hdr *)addr;
        chk = (struct cptra_checksum_info *)(addr + hdr_size);

        if (hdr->magic == CPTRA_FLASH_IMG_MAGIC &&
            hdr->img_count < CPTRA_IMC_ENTRY_COUNT) {
            /* Check the flash image header crc checksum */
            generate_crc32_table();
            hdr_crc32 = calc_checksum((uint8_t *)hdr, hdr_size);
            if (hdr_crc32 == chk->hdr_checksum) {
                uprintf("Found valid caliptra flash image at 0x%lx\n", addr);
                *manifest_base = addr;
                return CPTRA_SUCCESS;
            }
        }
    }

    uprintf("No valid caliptra flash image found in range");
    uprintf(" 0x%lx - 0x%lx (step: 0x%lx)\n",
            start_addr, end_addr, search_step);

    return CPTRA_ERR_IMAGE_OFFSET_INVALID;
}

static void dump_reserved_memory(struct reserved_mem_info *info)
{
    struct {
        const char *path;
        struct mem_region *region;
    } nodes[] = {
        { SSP_MEMORY_NODE,     &info->ssp       },
        { TSP_MEMORY_NODE,     &info->tsp       },
        { ATF_MEMORY_NODE,     &info->atf       },
        { OPTEE_MEMORY_NODE,   &info->tee       },
        { IPC_SSP_MEMORY_NODE, &info->ipc_ssp   },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        uprintf("[reserved] %s base: 0x%lx  size: 0x%lx\n", nodes[i].path,
                nodes[i].region->addr, nodes[i].region->size);
    }
}

int load_manifest_boot_image(uint64_t *jump_addr)
{
    const struct image_info *img_info = stor_get_image_info_table();
    struct cptra_image_context *cptra_ctx = cptra_get_ctx();
    uint64_t timeout = PSP_TIMEOUT;
    uint64_t manifest_base = 0;
    uint64_t uboot_start_addr;
    uint64_t uboot_end_addr;
    int ret = CPTRA_SUCCESS;
    void *dtb_ptr = NULL;

    /* Find manifest image */
    ret = find_manifest_image(MANIFEST_SEARCH_START, MANIFEST_SEARCH_END,
                              MANIFEST_SEARCH_STEP, &manifest_base);
    if (ret != CPTRA_SUCCESS) {
        return ret;
    }

    cptra_ctx->manifest_base = manifest_base;

    ret = stor_init();
    if (ret != CPTRA_SUCCESS) {
        return ret;
    }

    if (img_info[CPTRA_UBOOT_FW_ID].size > 0) {
        uboot_start_addr = img_info[CPTRA_UBOOT_FW_ID].offset;
        uboot_end_addr = uboot_start_addr + img_info[CPTRA_UBOOT_FW_ID].size;
        /* Try to find and load a valid U-Boot DTB */
        dtb_ptr = find_and_load_appended_dtb(uboot_start_addr, uboot_end_addr);
        if (dtb_ptr) {
            get_reserved_memory(dtb_ptr, &reservedinfo);
        }
    } else {
        uprintf("[reserved]: No U-Boot image found in caliptra flash image.\n");
        uprintf("[reserved]: Using default reserved memory layout.\n");
        dump_reserved_memory(&reservedinfo);
    }

    ret = cptra_verify_abb_loader();
    if (ret != CPTRA_SUCCESS) {
        return ret;
    }

    ret = cptra_load_abb_image();
    if (ret != CPTRA_SUCCESS) {
        return ret;
    }

    board_prepare_for_boot();

    uprintf("Waiting release PSP ...\n");

    while (!(readl(SCU0_CA35_REL) & BIT(0))) {
        if (--timeout == 0) {
            uprintf("ERROR: PSP release timeout!\n");
            panic("");
        }
    }

    uprintf("PSP is released ...\n");

    *jump_addr = (uint64_t)readl(SCU0_CA35_RVBAR0) << 4;
    uprintf("\nJumping to BL31 (Trusted Firmware-A) at 0x%lx\n\n", *jump_addr);

    return ret;
}

