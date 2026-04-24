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

#ifndef __AST27X0_INCLUDE_IMAGE_H__
#define __AST27X0_INCLUDE_IMAGE_H__

#include <stdint.h>

uint64_t convert_mcu_addr_to_arm_dram(uint64_t mcu_load_addr);
void *find_and_load_appended_dtb(uint64_t start_addr, uint64_t end_addr);

#endif /* __AST27X0_INCLUDE_IMAGE_H__ */
