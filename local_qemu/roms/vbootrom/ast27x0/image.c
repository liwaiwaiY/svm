/*
 * Boot image parsing and loading.
 *
 * Copyright 2025 Google LLC
 * Copyright (C) ASPEED Technology Inc.
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
#include <libfdt.h>
#include <uart.h>
#include <uart_console.h>
#include <io.h>
#include <image.h>
#include <fmc_image.h>
#include <manifest.h>
#include <platform_ast2700.h>

/*
 * This global struct is explicitly initialized, so it is placed in the .data
 * section. The function pointer (uputc) is set to uart_aspeed_poll_out at
 * compile time, which ensures its address is embedded in the final binary.
 *
 * If the struct were uninitialized, it would be placed in the .bss section,
 * and the function pointer would default to zero (NULL).
 * This distinction is especially important in bare-metal or ROM-based
 * environments, where only initialized data (.data) is included in the final
 * .bin image.
 *
 * To prevent this:
 * - Use initialized globals (ensures placement in .data)
 *
 * In embedded systems, especially when generating stripped-down .bin
 * firmware, it's critical to ensure that essential function pointers are
 * preserved explicitly.
 */
static struct uart_console ucons = {
    .uputc = uart_aspeed_poll_out
};

static const char *splash_screen =
      " _    ______  ____  ____  __________  ____  __  ___      ___   ______________  ______  ______\n"
      "| |  / / __ )/ __ \\/ __ \\/_  __/ __ \\/ __ \\/  |/  /     /   | / ___/_  __/__ \\/__  / |/ / __ \\\n"
      "| | / / __  / / / / / / / / / / /_/ / / / / /|_/ /_____/ /| | \\__ \\ / /  __/ /  / /|   / / / /\n"
      "| |/ / /_/ / /_/ / /_/ / / / / _, _/ /_/ / /  / /_____/ ___ |___/ // /  / __/  / //   / /_/ /\n"
      "|___/_____/\\____/\\____/ /_/ /_/ |_|\\____/_/  /_/     /_/  |_/____//_/  /____/ /_//_/|_\\____/\n"
      "\n";

static void print_build_info()
{
   uprintf("%s", splash_screen);
   uprintf("Build Date : %s %s\n", __DATE__, __TIME__);
   uprintf("FW Version : %s\n", GIT_VERSION);
   uprintf("\n");
}

/*
 * Remap 32-bit BootMCU load address to 64-bit Cortex-A35 DRAM address.
 * BootMCU loads the U-BOOT FIT image to a 32-bit address (e.g. 0x80000000),
 * but Cortex-A35 accesses DRAM starting at DRAM_ADDR (e.g. 0x400000000).
 */
uint64_t convert_mcu_addr_to_arm_dram(uint64_t mcu_load_addr)
{
    /*
     * If address is below 0x80000000, it's a physical address in SRAM.
     * No remapping is needed, return as-is.
     */
    if (mcu_load_addr < 0x80000000) {
        return mcu_load_addr;
    }

    return SYS_DRAM_BASE + ((uint64_t)mcu_load_addr - 0x80000000);
}

/*
 * Scan the image tail for an appended Device Tree Blob (DTB),
 * copy it to DRAM, and return its DRAM address if valid.
 *
 * This is used to locate SPL or U-Boot DTBs that are appended
 * after their raw images.
 */
void *find_and_load_appended_dtb(uint64_t start_addr, uint64_t end_addr)
{
    void *dram_dtb_addr = (void *)(uintptr_t)SYS_DRAM_BASE;
    const uint32_t *magic_ptr;
    size_t copy_size;
    uint64_t addr;

    for (addr = ALIGN_UP(start_addr, 4); addr + 4 < end_addr; addr += 4) {
        /* Check for DTB magic number (aligned on 4-byte boundary) */
        magic_ptr = (const uint32_t *)(uintptr_t)addr;
        if (*magic_ptr != cpu_to_fdt32(FDT_MAGIC)) {
            continue;
        }

        /* Copy from flash to DRAM for validation */
        copy_size = end_addr - addr;
        memcpy(dram_dtb_addr, (const void *)(uintptr_t)addr, copy_size);

        /* Verify if the copied region is a valid DTB */
        if (fdt_check_header(dram_dtb_addr) == 0) {
            uprintf("Valid DTB found at 0x%lx, copied to 0x%lx\n",
                    addr, (uint64_t)dram_dtb_addr);
            return dram_dtb_addr;
        } else {
            uprintf("FDT_MAGIC at 0x%lx but invalid DTB header\n", addr);
        }
    }

    uprintf("No valid DTB found between 0x%lx and 0x%lx\n",
            start_addr, end_addr);
    return NULL;
}

uint64_t load_boot_image(void)
{
    int ret = CPTRA_SUCCESS;
    uint64_t jump_addr;

    uart_aspeed_init(UART12);
    uart_console_register(&ucons);
    print_build_info();

    /*
     * Step 1: Try to boot using Caliptra Manifest
     *
     * If ALL steps above succeed:
     *   - Return BL31 entry address immediately
     *
     * If ANY step fails:
     *   - Do NOT panic here
     *   - Fall back to legacy FIT image boot flow
     *
     * NOTE:
     *   Caliptra Manifest boot is treated as the preferred path.
     *   FIT boot is kept as a compatibility mechanism.
     */
    ret = load_manifest_boot_image(&jump_addr);
    if (ret == CPTRA_SUCCESS) {
        return jump_addr;
    }

    uprintf("Caliptra boot image load failed (%d)\n", ret);

    /*
     * Step 2: Fallback to FIT image boot
     *
     * This path scans flash for a valid FIT image, loads U-Boot first,
     * then loads remaining firmware components (ATF, FDT, SSPFW, TSPFW, etc).
     *
     * If FIT boot fails, the system will panic.
     */
    return load_fit_boot_image();
}

