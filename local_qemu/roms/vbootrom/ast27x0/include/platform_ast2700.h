/*
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

#ifndef __AST27X0_INCLUDE_PLATFORM_AST2700_H__
#define __AST27X0_INCLUDE_PLATFORM_AST2700_H__

/* DRAM base address from the hardware's perspective */
#define SYS_DRAM_BASE           0x400000000ULL
#define ASPEED_FMC_CS0_BASE     0x100000000ULL

#define SCU0_REG            0x12c02000
#define SCU0_CA35_REL           (SCU0_REG + 0x10c)
#define SCU0_CA35_RVBAR0        (SCU0_REG + 0x110)
#define SCU0_CA35_RVBAR1        (SCU0_REG + 0x114)
#define SCU0_CA35_RVBAR2        (SCU0_REG + 0x118)
#define SCU0_CA35_RVBAR3        (SCU0_REG + 0x11c)
#define SCU0_CPU_SMP_EP0        (SCU0_REG + 0x780)
#define SCU0_CPU_SMP_EP1        (SCU0_REG + 0x788)
#define SCU0_CPU_SMP_EP2        (SCU0_REG + 0x790)
#define SCU0_CPU_SMP_EP3        (SCU0_REG + 0x798)

#endif /* __AST27X0_INCLUDE_PLATFORM_AST2700_H__ */
