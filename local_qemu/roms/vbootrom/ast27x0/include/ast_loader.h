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

#ifndef __AST27X0_INCLUDE_AST_LOADER_H__
#define __AST27X0_INCLUDE_AST_LOADER_H__

#include <stddef.h>

void generate_crc32_table(void);
uint32_t calc_checksum(const uint8_t *data, size_t length);

int ast_loader_read(void *dst, uint64_t src, uint32_t len);
int ast_loader_load_image(uint32_t type, uint32_t *dst);
int ast_loader_load_manifest_image(uint32_t type, uint32_t *dst);

#endif /* __AST27X0_INCLUDE_AST_LOADER_H__ */
