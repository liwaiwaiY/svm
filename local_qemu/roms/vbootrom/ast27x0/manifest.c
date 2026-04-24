/*
 * Caliptra manifest image parsing and loading.
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
#include <io.h>
#include <manifest.h>
#include <uart_console.h>
#include <ast_loader.h>

static struct cptra_image_context cptra_ctx;

static int cptra_crc32_check(struct cptra_image_context *ctx)
{
    uint32_t hdr_crc32 = 0;

    /* Check the flash image header crc checksum */
    generate_crc32_table();
    hdr_crc32 = calc_checksum((uint8_t *)ctx->hdr,
                              sizeof(struct cptra_manifest_hdr));
    if (hdr_crc32 != ctx->chk->hdr_checksum) {
        uprintf("Check flash image header checksum ...fail.\n");
        return CPTRA_ERR_HDR_CHKSUM;
    }

    uprintf("Check flash image checksum ...pass.\n");

    return CPTRA_SUCCESS;
}

static int cptra_read_header(struct cptra_image_context *ctx)
{
    int ret = 0;
    static struct cptra_manifest_hdr hdr = { 0 };

    /* Read the manfiest header */
    ret = ast_loader_read(&hdr, ctx->manifest_base,
                          sizeof(struct cptra_manifest_hdr));
    if (ret) {
        uprintf("Failed to read manifest header.\n");
        return CPTRA_ERR_READ_HDR;
    }

    if (hdr.magic != CPTRA_FLASH_IMG_MAGIC) {
        uprintf("Manifest header magic mismatch: 0x%x.\n", hdr.magic);
        return CPTRA_ERR_HDR_MAGIC_MISMATCH;
    }

    if (hdr.img_count > CPTRA_IMC_ENTRY_COUNT) {
        uprintf("Manifest image count exceeds maximum limit: %d.\n",
                hdr.img_count);
        return CPTRA_ERR_EXCEED_MAX_IMG_COUNT;
    }

    /* The image header is correct, keep it in context */
    ctx->hdr = &hdr;

    return CPTRA_SUCCESS;
}

static int cptra_read_chk_img_info(struct cptra_image_context *ctx)
{
    int ret = 0;
    int img_num = ctx->hdr->img_count;
    static uint8_t chk_img[sizeof(struct cptra_checksum_info) +
                           sizeof(struct cptra_image_info) *
                           CPTRA_IMC_ENTRY_COUNT] = { 0 };
    uint32_t ofst = sizeof(struct cptra_manifest_hdr);
    uint32_t size = 0;

    if (img_num > CPTRA_IMC_ENTRY_COUNT) {
        return CPTRA_ERR_EXCEED_MAX_IMG_COUNT;
    }

    size = sizeof(struct cptra_checksum_info);
    size += sizeof(struct cptra_image_info) * img_num;
    ret = ast_loader_read(chk_img, ctx->manifest_base + ofst,
                          size);
    if (ret) {
        return CPTRA_ERR_READ_IMG_INFO;
    }

    ctx->chk = (struct cptra_checksum_info *)chk_img;
    ctx->img_info = (struct cptra_image_info *)(chk_img +
                     sizeof(struct cptra_checksum_info));

    return CPTRA_SUCCESS;
}

static int cptra_read_abb_header(struct cptra_image_context *ctx)
{
    int ret = 0;

    /* header, checksum, img_info has been read exit directly */
    if (ctx->hdr && ctx->chk && ctx->img_info) {
        return ret;
    }

    ret = cptra_read_header(ctx);
    if (ret) {
        goto fail;
    }

    ret = cptra_read_chk_img_info(ctx);
    if (ret) {
        goto fail;
    }

    ret = cptra_crc32_check(ctx);
    if (ret) {
        goto fail;
    }

fail:
    return ret;
}

static int cptra_read_abb_soc_manifest(struct cptra_image_context *ctx)
{
    int ret = 0;
    static struct cptra_soc_manifest soc_manifest;

    ret = ast_loader_load_image(CPTRA_MANIFEST_FW_ID,
                                (uint32_t *)&soc_manifest);
    if (ret) {
        return CPTRA_ERR_SOC_MANIFEST_READ_ERROR;
    }

    if (soc_manifest.preamble.manifest_marker !=
        CPTRA_MBCMD_SET_AUTH_MANIFEST)
        return CPTRA_ERR_SOC_MANIFEST_MAGIC_MISMATCH;

    ctx->soc_manifest = &soc_manifest;

    uprintf("ver: %x, flags: %x, soc_ver: %x\n",
        soc_manifest.preamble.manifest_version,
        soc_manifest.preamble.manifest_flags,
        soc_manifest.preamble.manifest_sec_version);
    uprintf("manfiest vendor pubk: 0x%08x\n",
        soc_manifest.preamble.manifest_vendor_ecc384_key[0]);
    uprintf("manfiest owner pubk: 0x%08x\n",
        soc_manifest.preamble.manifest_owner_ecc384_key[0]);

    return CPTRA_SUCCESS;
}

static void cptra_deinit_abb_loader(int ret)
{
    if (ret && cptra_ctx.hdr) {
        memset(cptra_ctx.hdr, 0, sizeof(struct cptra_manifest_hdr));
    }

    if (ret && cptra_ctx.chk) {
        memset(cptra_ctx.chk, 0, sizeof(struct cptra_checksum_info));
    }

    if (ret && cptra_ctx.img_info) {
        memset(cptra_ctx.img_info, 0,
               sizeof(struct cptra_image_info) * CPTRA_IMC_ENTRY_COUNT);
    }

    if (ret && cptra_ctx.soc_manifest) {
        memset(cptra_ctx.soc_manifest, 0,
               sizeof(struct cptra_soc_manifest));
    }
}

static int cptra_init_abb_loader(void)
{
    int ret = 0;

    if (cptra_ctx.hdr && cptra_ctx.img_info && cptra_ctx.chk) {
        return ret;
    }

    ret = cptra_read_abb_header(&cptra_ctx);
    uprintf("Read abb header... %s (0x%x)\n", ret ? "fail" : "pass", ret);

    cptra_deinit_abb_loader(ret);
    return ret;
}

int cptra_verify_abb_loader(void)
{
    int ret = 0;

    if (cptra_ctx.soc_manifest) {
        return ret;
    }

    ret = cptra_read_abb_soc_manifest(&cptra_ctx);
    uprintf("Read soc manifest... %s (0x%x)\n", ret ? "fail" : "pass", ret);

    cptra_deinit_abb_loader(ret);
    return ret;
}

int cptra_load_abb_image(void)
{
    int ret = 0;
    uint32_t *load_addr = 0;
    struct cptra_soc_manifest *man = cptra_ctx.soc_manifest;
    struct cptra_manifest_ime *ime = &man->imc[0];

    if (!man) {
        return CPTRA_ERR_ABB_LOADER_NOT_READY;
    }

    for (ime = &man->imc[0]; ime < man->imc + man->ime_count && !ret; ime++) {
        /* Check the whether ime denote image should be loaded */
        if (!cptra_ime_loadable_image(ime)) {
            continue;
        }

        load_addr = (uint32_t *)cptra_ime_get_load_addr(ime->fw_id);
        if (!load_addr) {
            continue;
        }

        ret = ast_loader_load_manifest_image(ime->fw_id, load_addr);
        uprintf("Load %s image... %s (0x%x)\n",
                cptra_ime_get_image_name(ime->fw_id),
                ret ? "fail" : "pass", ret);

        if (!ret) {
            board_manifest_image_post_process(ime->fw_id);
        }
    }

    cptra_deinit_abb_loader(ret);
    return ret;
}

int cptra_get_abb_imginfo(uint32_t fw_id, uint32_t *ofst, uint32_t *size)
{
    int ret = 0;
    int image_offset = 0;
    int image_size = 0;

    ret = cptra_init_abb_loader();
    if (ret) {
        return ret;
    }

    image_offset = cptra_ime_image_offset(&cptra_ctx, fw_id);
    if (image_offset < 0) {
        return CPTRA_ERR_IMAGE_READ;
    }

    image_size = cptra_ime_image_size(&cptra_ctx, fw_id);
    if (image_size < 0) {
        return CPTRA_ERR_IMAGE_READ;
    }

    *ofst = image_offset;
    *size = image_size;

    return CPTRA_SUCCESS;
}

struct cptra_image_context *cptra_get_ctx(void)
{
    return &cptra_ctx;
}

