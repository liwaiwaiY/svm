/*
 * Copyright (C) 2025 ASPEED Technology Inc.
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

#include <io.h>
#include <libfdt.h>
#include <uart_console.h>
#include <image.h>
#include <ssp_tsp.h>
#include <platform_ast2700.h>

void get_reserved_memory(const void *fdt_blob, struct reserved_mem_info *info)
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

    const fdt32_t *reg;
    const char *path;
    int size_cells;
    int addr_cells;
    uint64_t addr;
    uint64_t size;
    int offset;
    int parent;
    size_t i;
    int len;
    int j;

    for (i = 0; i < ARRAY_SIZE(nodes); i++) {
        path = nodes[i].path;
        nodes[i].region->addr = 0;
        nodes[i].region->size = 0;
        addr = 0;
        size = 0;

        offset = fdt_path_offset(fdt_blob, path);
        if (offset < 0) {
            uprintf("Cannot find node %s in the device tree.\n", path);
            continue;
        }

        parent = fdt_parent_offset(fdt_blob, offset);
        addr_cells = fdt_address_cells(fdt_blob, parent);
        size_cells = fdt_size_cells(fdt_blob, parent);

        if (addr_cells < 0 || addr_cells > 2) {
            uprintf("unsupported addr_cells: %d\n", addr_cells);
            continue;
        }

        if (size_cells < 0 || size_cells > 2) {
            uprintf("unsupported size_cells: %d\n", size_cells);
            continue;
        }

        reg = fdt_getprop(fdt_blob, offset, "reg", &len);
        if (!reg) {
            uprintf("No reg property found in %s\n", path);
            continue;
        }

        if (len < (addr_cells + size_cells) * (int)sizeof(fdt32_t)) {
            uprintf("reg too short in %s (len=%d)\n", path, len);
            continue;
        }

        for (j = 0; j < addr_cells; j++) {
            addr = (addr << 32) | (uint64_t)fdt32_to_cpu(reg[j]);
        }

        for (j = 0; j < size_cells; j++) {
            size = (size << 32) | (uint64_t)fdt32_to_cpu(reg[j + addr_cells]);
        }

        /* convert to Arm view */
        if (addr_cells == 1) {
            addr = convert_mcu_addr_to_arm_dram(addr);
        }

        nodes[i].region->addr = addr;
        nodes[i].region->size = size;

        uprintf("[reserved] %s base: 0x%lx  size: 0x%lx\n", path,
                nodes[i].region->addr, nodes[i].region->size);
    }
}

int ssp_init(uint64_t load_addr, const struct reserved_mem_info *info)
{
    struct ast2700_scu0 *scu;
    uint32_t reg_val;

    if (load_addr != info->ssp.addr) {
        uprintf("load address %lx doesn't match SSP reserved memory %lx\n",
                load_addr, info->ssp.addr);
        return 1;
    }

    scu = (struct ast2700_scu0 *)ASPEED_CPU_SCU_BASE;

    reg_val = readl((void *)&scu->ssp_ctrl_0);
    if (!(reg_val & SCU_CPU_SSP_TSP_RESET_STS)) {
        return 0;
    }

    writel(SCU_CPU_RST_SSP, (void *)&scu->modrst1_ctrl);
    writel(SCU_CPU_RST_SSP, (void *)&scu->modrst1_clr);

    reg_val = SCU_CPU_SSP_TSP_NIDEN | SCU_CPU_SSP_TSP_DBGEN |
              SCU_CPU_SSP_TSP_DBG_ENABLE | SCU_CPU_SSP_TSP_RESET;
    writel(reg_val, (void *)&scu->ssp_ctrl_0);

    /*
     * SSP Memory Map:
     * - 0x0000_0000 - 0x0587_FFFF: ssp_remap2 -> DRAM[load_addr]
     * - 0x0588_0000 - 0x1FFF_DFFF: ssp_remap1 -> AHB -> DRAM[0]
     * - 0x1FFF_E000 - 0x2000_0000: ssp_remap0 -> TCM (SSP stack)
     *
     * The SSP serves as the secure loader for TSP, ATF, OP-TEE, and U-Boot.
     * Therefore, their load buffers must be visible to the SSP.
     *
     * - SSP remap entry #2 (ssp_remap2_base/size) maps the load buffers
     *   for SSP, TSP, ATF, OP-TEE and IPC share memory. Ensure these buffers
     *   are contiguous.
     * - SSP remap entry #1 (ssp_remap1_base/size) maps the load buffer
     *   for U-Boot at DRAM offset 0x0.
     * - SSP remap entry #0 (ssp_remap0_base/size) maps TCM, which is used for
     *   stack.
     */
    writel(0, (void *)&scu->ssp_memory_base);
    reg_val = info->ssp.size + info->tsp.size + info->atf.size +
              info->tee.size + info->ipc_ssp.size;
    writel(reg_val, (void *)&scu->ssp_memory_size);

    writel(reg_val, (void *)&scu->ssp_ahb_base);
    writel(MAX_I_D_ADDRESS - reg_val - TCM_SIZE, (void *)&scu->ssp_ahb_size);

    writel(MAX_I_D_ADDRESS - TCM_SIZE, (void *)&scu->ssp_tcm_base);
    writel(TCM_SIZE, (void *)&scu->ssp_tcm_size);

    /* Configure physical AHB remap: through H2M, mapped to SYS_DRAM_BASE */
    writel((uint32_t)(SYS_DRAM_BASE >> 4), (void *)&scu->ssp_ctrl_1);

    /* Configure physical DRAM remap */
    reg_val = (uint32_t)(load_addr >> 4);
    writel(reg_val, (void *)&scu->ssp_ctrl_2);

    /*
     * For A1, the Cache region can only be enabled entirely;
     * partial enabling is not supported.
     */
    writel(GENMASK(31, 0), (void *)&scu->ssp_ctrl_3);
    writel(GENMASK(31, 0), (void *)&scu->ssp_ctrl_4);

    /* Enable I and D cache as default */
    writel(SCU_CPU_SSP_TSP_CTRL_ICACHE_EN | SCU_CPU_SSP_TSP_CTRL_DCACHE_EN,
           (void *)&scu->ssp_ctrl_6);

    return 0;
}

int ssp_enable(void)
{
    struct ast2700_scu0 *scu;
    uint32_t reg_val;

    scu = (struct ast2700_scu0 *)ASPEED_CPU_SCU_BASE;
    reg_val = readl((void *)&scu->ssp_ctrl_0);
    reg_val |= SCU_CPU_SSP_TSP_ENABLE | SCU_CPU_SSP_TSP_RESET;
    writel(reg_val, (void *)&scu->ssp_ctrl_0);

    return 0;
}

int tsp_init(uint64_t load_addr, const struct reserved_mem_info *info)
{
    struct ast2700_scu0 *scu;
    uint32_t reg_val;

    if (load_addr != info->tsp.addr) {
        uprintf("load address %lx doesn't match TSP reserved memory %lx\n",
                load_addr, info->tsp.addr);
        return 1;
    }

    scu = (struct ast2700_scu0 *)ASPEED_CPU_SCU_BASE;

    reg_val = readl((void *)&scu->tsp_ctrl_0);
    if (!(reg_val & SCU_CPU_SSP_TSP_RESET_STS)) {
        return 0;
    }

    writel(SCU_CPU_RST2_TSP, (void *)&scu->modrst2_ctrl);
    writel(SCU_CPU_RST2_TSP, (void *)&scu->modrst2_clr);

    reg_val = SCU_CPU_SSP_TSP_NIDEN | SCU_CPU_SSP_TSP_DBGEN |
              SCU_CPU_SSP_TSP_DBG_ENABLE | SCU_CPU_SSP_TSP_RESET;
    writel(reg_val, (void *)&scu->tsp_ctrl_0);

    /* TSP 0x0000_0000 - 0x0200_0000 -> DRAM */
    writel(info->tsp.size, (void *)&scu->tsp_remap_size);

    /* Configure physical DRAM remap */
    reg_val = (uint32_t)(load_addr >> 4);
    writel(reg_val, (void *)&scu->tsp_ctrl_1);

    /*
     * For A1, the Cache region can only be enabled entirely;
     * partial enabling is not supported.
     */
    writel(GENMASK(31, 0), (void *)&scu->tsp_ctrl_2);
    writel(GENMASK(31, 0), (void *)&scu->tsp_ctrl_3);

    /* Enable I and D cache as default */
    writel(SCU_CPU_SSP_TSP_CTRL_ICACHE_EN | SCU_CPU_SSP_TSP_CTRL_DCACHE_EN,
           (void *)&scu->tsp_ctrl_5);

    return 0;
}

int tsp_enable(void)
{
    struct ast2700_scu0 *scu;
    uint32_t reg_val;

    scu = (struct ast2700_scu0 *)ASPEED_CPU_SCU_BASE;
    reg_val = readl((void *)&scu->tsp_ctrl_0);
    reg_val |= SCU_CPU_SSP_TSP_ENABLE | SCU_CPU_SSP_TSP_RESET;
    writel(reg_val, (void *)&scu->tsp_ctrl_0);

    return 0;
}

