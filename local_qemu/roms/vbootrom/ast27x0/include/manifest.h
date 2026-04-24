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

#ifndef __AST27X0_INCLUDE_MANIFEST_H__
#define __AST27X0_INCLUDE_MANIFEST_H__

#include <stdint.h>
#include <stdbool.h>
#include <image.h>

#define CPTRA_FLASH_IMG_MAGIC (0x48534C46) /* 'FLSH' in little endian */
#define CPTRA_MBCMD_SET_AUTH_MANIFEST (0x41544D4E) /* "ATMN" */
#define CPTRA_IMC_ENTRY_COUNT 127

enum {
    CPTRA_MANIFEST_FW_ID = 0x00,
    CPTRA_FMC_FW_ID = 0x01,
    CPTRA_DDR4_IMEM_FW_ID = 0x02,
    CPTRA_DDR4_DMEM_FW_ID = 0x03,
    CPTRA_DDR4_2D_IMEM_FW_ID = 0x04,
    CPTRA_DDR4_2D_DMEM_FW_ID = 0x05,
    CPTRA_DDR5_IMEM_FW_ID = 0x06,
    CPTRA_DDR5_DMEM_FW_ID = 0x07,
    CPTRA_DP_FW_FW_ID = 0x08,
    CPTRA_UEFI_FW_ID = 0x09,
    CPTRA_ATF_FW_ID = 0x0a,
    CPTRA_OPTEE_FW_ID = 0x0b,
    CPTRA_UBOOT_FW_ID = 0x0c,
    CPTRA_SSP_FW_ID = 0x0d,
    CPTRA_TSP_FW_ID = 0x0e,
};

enum cptra_error_code {
    CPTRA_SUCCESS = 0,
    CPTRA_ERR_EXCEED_MEMORY_LIMIT,
    CPTRA_ERR_INVALID_PARAMETER,
    CPTRA_ERR_ABB_LOADER_NOT_READY,
    CPTRA_ERR_READ_HDR,
    CPTRA_ERR_HDR_MAGIC_MISMATCH,
    CPTRA_ERR_EXCEED_MAX_IMG_COUNT,
    CPTRA_ERR_READ_CHKSUM,
    CPTRA_ERR_READ_IMG_INFO,
    CPTRA_ERR_READ_FULL_IMG,
    CPTRA_ERR_HDR_CHKSUM,
    CPTRA_ERR_PAYLOAD_CHKSUM,
    CPTRA_ERR_SOC_MANIFEST_NO_INFO,
    CPTRA_ERR_SOC_MANIFEST_READ_ERROR,
    CPTRA_ERR_SOC_MANIFEST_MAGIC_MISMATCH,
    CPTRA_ERR_SOC_MANIFEST_VFY,
    CPTRA_ERR_SOC_MANIFEST_ECC_SVN_VFY,
    CPTRA_ERR_SOC_MANIFEST_LMS_SVN_VFY,
    CPTRA_ERR_SOC_MANIFEST_VER_MISMATCH,
    CPTRA_ERR_SHA384_CAL,
    CPTRA_ERR_IMAGE_VFY_UNKNOWN_ERROR,
    CPTRA_ERR_IMAGE_VFY_MBOX_ERROR,
    CPTRA_ERR_IMAGE_VFY_FWID_MISMATCH,
    CPTRA_ERR_IMAGE_VFY_HASH_MISMATCH,
    CPTRA_ERR_IMAGE_LOAD_INVALID_PARAM,
    CPTRA_ERR_IMAGE_LOAD,
    CPTRA_ERR_IMAGE_READ,
    CPTRA_ERR_IMAGE_SIZE_INVALID = -1,
    CPTRA_ERR_IMAGE_OFFSET_INVALID = -2,
};

struct cptra_manifest_ime {
    uint32_t fw_id;
    uint32_t flags;
    uint8_t digest[48]; /* SHA384 */
};

struct cptra_manifest_hdr {
    uint32_t magic;
    uint16_t hdr_ver;
    uint16_t img_count;
} __attribute__((__packed__, __aligned__(4)));

struct cptra_checksum_info {
    uint32_t hdr_checksum;
    uint32_t payload_checksum;
} __attribute__((__packed__));

struct cptra_image_info {
    uint32_t identifier;
    uint32_t offset;
    uint32_t size;
} __attribute__((__packed__));

struct cptra_manifest_aspeed_preamble {
    uint32_t manifest_marker;
    uint32_t preamble_size;
    uint32_t manifest_version;
    uint32_t manifest_sec_version;
    uint32_t manifest_flags;
    uint32_t manifest_vendor_ecc384_key[24];
    uint32_t manifest_vendor_lms_key[12];
    uint32_t manifest_vendor_ecc384_sig[24];
    uint32_t manifest_vendor_LMS_sig[405];
    uint32_t manifest_owner_ecc384_key[24];
    uint32_t manifest_owner_lms_key[12];
    uint32_t manifest_owner_ecc384_sig[24];
    uint32_t manifest_owner_LMS_sig[405];
    uint32_t manifest_owner_svn_ecc384_sig[24];
    uint32_t manifest_owner_svn_LMS_sig[405];
    uint32_t metadata_vendor_ecc384_sig[24];
    uint32_t metadata_vendor_LMS_sig[405];
    uint32_t metadata_owner_ecc384_sig[24];
    uint32_t metadata_owner_LMS_sig[405];
} __attribute__((__packed__, __aligned__(4)));

struct cptra_manifest_aspeed_svn {
    uint32_t ver;
    uint32_t sec_ver;
    uint32_t flags;
    uint32_t manifest_owner_ecc384_key[24];
    uint32_t manifest_owner_lms_key[12];
} __attribute__((__packed__));

struct cptra_soc_manifest {
    struct cptra_manifest_aspeed_preamble preamble;
    uint32_t ime_count;
    struct cptra_manifest_ime imc[CPTRA_IMC_ENTRY_COUNT];
} __attribute__((__packed__, __aligned__(4)));

struct cptra_image_context {
    struct cptra_manifest_hdr *hdr;
    struct cptra_checksum_info *chk;
    struct cptra_image_info *img_info;
    struct cptra_soc_manifest *soc_manifest;
    uint64_t manifest_base;
};

int cptra_verify_abb_loader(void);
int cptra_load_abb_image(void);
int cptra_get_abb_imginfo(uint32_t fw_id, uint32_t *ofst, uint32_t *size);
void board_manifest_image_post_process(uint32_t fw_id);

bool cptra_ime_loadable_image(struct cptra_manifest_ime *ime);
char *cptra_ime_get_image_name(uint32_t fw_id);
int cptra_ime_image_offset(struct cptra_image_context *ctx, uint32_t fw_id);
int cptra_ime_image_size(struct cptra_image_context *ctx, uint32_t fw_id);
uintptr_t cptra_ime_get_load_addr(uint32_t fw_id);

int load_manifest_boot_image(uint64_t *jump_addr);
struct cptra_image_context *cptra_get_ctx(void);

#endif /* __AST27X0_INCLUDE_MANIFEST_H__ */
