/*
 * Copyright (c) 2018 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "etnaviv_2d.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_screen.h"
#include "etnaviv_rs.h"

#include "pipe/p_state.h"
#include "util/u_format.h"

#include "hw/state_2d.xml.h"
#include "hw/common.xml.h"

#include <assert.h>
#include <math.h>

#define EMIT_STATE(state_name, src_value) \
   etna_coalsence_emit(stream, &coalesce, VIVS_##state_name, src_value)

#define EMIT_STATE_RELOC(state_name, src_value) \
   etna_coalsence_emit_reloc(stream, &coalesce, VIVS_##state_name, src_value)

/* stolen from xf86-video-armada */
#define KERNEL_ROWS     17
#define KERNEL_INDICES  9
#define KERNEL_SIZE     (KERNEL_ROWS * KERNEL_INDICES)
#define KERNEL_STATE_SZ ((KERNEL_SIZE + 1) / 2)

static bool filter_kernel_initialized;
static uint32_t filter_kernel[KERNEL_STATE_SZ];

static inline float
sinc (float x)
{
  return x != 0.0 ? sinf (x) / x : 1.0;
}

static void
etnaviv_init_filter_kernel(void)
{
   unsigned row, idx, i;
   int16_t kernel_val[KERNEL_STATE_SZ * 2];
   float row_ofs = 0.5;
   float radius = 4.0;

   /* Compute lanczos filter kernel */
   for (row = i = 0; row < KERNEL_ROWS; row++) {
      float kernel[KERNEL_INDICES] = { 0.0 };
      float sum = 0.0;

      for (idx = 0; idx < KERNEL_INDICES; idx++) {
         float x = idx - 4.0 + row_ofs;

         if (fabs (x) <= radius)
            kernel[idx] = sinc (M_PI * x) * sinc (M_PI * x / radius);

         sum += kernel[idx];
       }

       /* normalise the row */
       if (sum)
          for (idx = 0; idx < KERNEL_INDICES; idx++)
             kernel[idx] /= sum;

       /* convert to 1.14 format */
       for (idx = 0; idx < KERNEL_INDICES; idx++) {
          int val = kernel[idx] * (float) (1 << 14);

          if (val < -0x8000)
             val = -0x8000;
          else if (val > 0x7fff)
             val = 0x7fff;

          kernel_val[i++] = val;
       }

       row_ofs -= 1.0 / ((KERNEL_ROWS - 1) * 2);
   }

   kernel_val[KERNEL_SIZE] = 0;

   /* Now convert the kernel values into state values */
   for (i = 0; i < KERNEL_STATE_SZ * 2; i += 2)
      filter_kernel[i / 2] =
         VIVS_DE_FILTER_KERNEL_COEFFICIENT0 (kernel_val[i]) |
         VIVS_DE_FILTER_KERNEL_COEFFICIENT1 (kernel_val[i + 1]);
}

bool etna_try_2d_blit(struct pipe_context *pctx,
                      const struct pipe_blit_info *blit_info)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_screen *screen = ctx->screen;
   struct etna_cmd_stream *stream = ctx->stream2d;
   struct etna_coalesce coalesce;
   struct etna_reloc ry, ru, rv, rdst;
   struct pipe_resource *res_y, *res_u, *res_v, *res_dst;
   struct etna_bo *temp_bo = NULL;
   uint32_t src_format;
   bool ext_blt = VIV_2D_FEATURE(screen, chipMinorFeatures2, 2D_TILING);
   uint32_t dst_config;

   assert(util_format_is_yuv(blit_info->src.format));
   assert(blit_info->dst.format == PIPE_FORMAT_R8G8B8A8_UNORM);

   if (!stream)
      return FALSE;

  if (unlikely(!ext_blt && !filter_kernel_initialized)) {
      etnaviv_init_filter_kernel();
      filter_kernel_initialized = true;
  }

   switch (blit_info->src.format) {
   case PIPE_FORMAT_NV12:
      src_format = DE_FORMAT_NV12;
      break;
   case PIPE_FORMAT_YUYV:
      src_format = DE_FORMAT_YUY2;
      break;
   default:
      return FALSE;
   }

   if (!ext_blt) {
      struct etna_resource *dst = etna_resource(blit_info->dst.resource);
      unsigned int bo_size = dst->levels[0].stride * dst->levels[0].padded_height;

      temp_bo = etna_bo_new(screen->dev, bo_size, DRM_ETNA_GEM_CACHE_WC);
      if (!temp_bo)
         return FALSE;
   }

   res_y = blit_info->src.resource;
   res_u = res_y->next ? res_y->next : res_y;
   res_v = res_u->next ? res_u->next : res_u;

   ry.bo = etna_resource(res_y)->bo;
   ry.offset = etna_resource(res_y)->offset;
   ru.bo = etna_resource(res_u)->bo;
   ru.offset = etna_resource(res_u)->offset;
   rv.bo = etna_resource(res_v)->bo;
   rv.offset = etna_resource(res_v)->offset;

   ry.flags = ru.flags = rv.flags = ETNA_RELOC_READ;

   res_dst = blit_info->dst.resource;
   rdst.bo = ext_blt ? etna_resource(res_dst)->bo : temp_bo;
   rdst.flags = ETNA_RELOC_WRITE;
   rdst.offset = 0;

   assert(etna_resource(res_dst)->layout == ETNA_LAYOUT_TILED);

   etna_coalesce_start(stream, &coalesce);

   EMIT_STATE_RELOC(DE_SRC_ADDRESS, &ry);
   EMIT_STATE(DE_SRC_STRIDE, etna_resource(res_y)->levels[0].stride);

   EMIT_STATE_RELOC(DE_UPLANE_ADDRESS, &ru);
   EMIT_STATE(DE_UPLANE_STRIDE, etna_resource(res_u)->levels[0].stride);
   EMIT_STATE_RELOC(DE_VPLANE_ADDRESS, &rv);
   EMIT_STATE(DE_VPLANE_STRIDE, etna_resource(res_v)->levels[0].stride);

   /* Source configuration */
   EMIT_STATE(DE_SRC_ROTATION_CONFIG, 0);
   EMIT_STATE(DE_SRC_CONFIG,
              VIVS_DE_SRC_CONFIG_SOURCE_FORMAT(src_format) |
              VIVS_DE_SRC_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB));
   EMIT_STATE(DE_SRC_ORIGIN, 0);
   EMIT_STATE(DE_SRC_SIZE, 0);
   EMIT_STATE(DE_SRC_COLOR_BG, 0);
   EMIT_STATE(DE_SRC_COLOR_FG, 0);
   EMIT_STATE(DE_STRETCH_FACTOR_LOW,
              VIVS_DE_STRETCH_FACTOR_LOW_X(1 << 16));
   EMIT_STATE(DE_STRETCH_FACTOR_HIGH,
              VIVS_DE_STRETCH_FACTOR_HIGH_Y(1 << 16));

   /* Destination address and stride */
   EMIT_STATE_RELOC(DE_DEST_ADDRESS, &rdst);
   EMIT_STATE(DE_DEST_STRIDE, etna_resource(res_dst)->levels[0].stride);

   /* Drawing engine configuration */
   EMIT_STATE(DE_DEST_ROTATION_CONFIG, 0);
   dst_config = VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
                VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ABGR);
   if (ext_blt) {
      dst_config |= VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT |
                    VIVS_DE_DEST_CONFIG_TILED_ENABLE;
   } else {
      dst_config |= VIVS_DE_DEST_CONFIG_COMMAND_HOR_FILTER_BLT;
   }
   EMIT_STATE(DE_DEST_CONFIG, dst_config);
   EMIT_STATE(DE_ROP,
              VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
              VIVS_DE_ROP_TYPE_ROP4);
   EMIT_STATE(DE_CLIP_TOP_LEFT,
              VIVS_DE_CLIP_TOP_LEFT_X(0) |
              VIVS_DE_CLIP_TOP_LEFT_Y(0));
   EMIT_STATE(DE_CLIP_BOTTOM_RIGHT,
              VIVS_DE_CLIP_BOTTOM_RIGHT_X(blit_info->dst.box.width) |
              VIVS_DE_CLIP_BOTTOM_RIGHT_Y(blit_info->dst.box.height));
   EMIT_STATE(DE_CONFIG, 0);
   EMIT_STATE(DE_SRC_ORIGIN_FRACTION, 0);
   EMIT_STATE(DE_ALPHA_CONTROL, 0);
   EMIT_STATE(DE_ALPHA_MODES, 0);
   EMIT_STATE(DE_DEST_ROTATION_HEIGHT, 0);
   EMIT_STATE(DE_SRC_ROTATION_HEIGHT, 0);
   EMIT_STATE(DE_ROT_ANGLE, 0);

   if (!ext_blt) {
      EMIT_STATE(DE_VR_SOURCE_IMAGE_LOW,
                 VIVS_DE_VR_SOURCE_IMAGE_LOW_LEFT (0) |
                 VIVS_DE_VR_SOURCE_IMAGE_LOW_TOP (0));
      EMIT_STATE(DE_VR_SOURCE_IMAGE_HIGH,
                 VIVS_DE_VR_SOURCE_IMAGE_HIGH_RIGHT(blit_info->src.box.width) |
                 VIVS_DE_VR_SOURCE_IMAGE_HIGH_BOTTOM(blit_info->src.box.height));
      EMIT_STATE(DE_VR_SOURCE_ORIGIN_LOW, VIVS_DE_VR_SOURCE_ORIGIN_LOW_X(1));
      EMIT_STATE(DE_VR_SOURCE_ORIGIN_HIGH, VIVS_DE_VR_SOURCE_ORIGIN_HIGH_Y(1));
      EMIT_STATE(DE_VR_TARGET_WINDOW_LOW,
                 VIVS_DE_VR_TARGET_WINDOW_LOW_LEFT(0) |
                 VIVS_DE_VR_TARGET_WINDOW_LOW_TOP(0));
      EMIT_STATE(DE_VR_TARGET_WINDOW_HIGH,
                 VIVS_DE_VR_TARGET_WINDOW_HIGH_RIGHT(blit_info->dst.box.width) |
                 VIVS_DE_VR_TARGET_WINDOW_HIGH_BOTTOM(blit_info->dst.box.height));
   }

   etna_coalesce_end(stream, &coalesce);

   if (ext_blt) {
      etna_cmd_stream_emit(stream, VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |
                           VIV_FE_DRAW_2D_HEADER_COUNT(1));
      etna_cmd_stream_emit(stream, 0xdeadbeef);

      etna_cmd_stream_emit(stream, VIV_FE_DRAW_2D_TOP_LEFT_X(0) |
                           VIV_FE_DRAW_2D_TOP_LEFT_Y(0));
      etna_cmd_stream_emit(stream,
                           VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(blit_info->dst.box.width) |
                           VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(blit_info->dst.box.height));

      etna_set_state(stream, 1, 0);
      etna_set_state(stream, 1, 0);
      etna_set_state(stream, 1, 0);
   } else {
      etna_set_state_multi(stream, VIVS_DE_FILTER_KERNEL(0), KERNEL_STATE_SZ,
                           filter_kernel);

      etna_set_state(stream, VIVS_DE_VR_CONFIG,
                     VIVS_DE_VR_CONFIG_START_HORIZONTAL_BLIT);
   }

   etna_set_state(stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);

   /* Flush now, this avoid the need to track cross-dependencies between
    * 2D and 3D GPU. The 3D GPU will only read from the 2D prepared buffers,
    * so the kernel is taking care of any needed synchronization.
    */
   etna_cmd_stream_flush(ctx->stream2d);

   if (!ext_blt) {
      struct etna_resource *dst = etna_resource(res_dst);
      struct compiled_rs_state tile_blit;

      etna_compile_rs_state(ctx, &tile_blit, &(struct rs_state) {
            .source_format = RS_FORMAT_X8R8G8B8,
            .source_tiling = ETNA_LAYOUT_LINEAR,
            .source = temp_bo,
            .source_offset = 0,
            .source_stride = dst->levels[0].stride,
            .source_padded_width = dst->levels[0].padded_width,
            .source_padded_height = dst->levels[0].padded_height,
            .source_ts_valid = 0,
            .dest_format = RS_FORMAT_X8R8G8B8,
            .dest_tiling = ETNA_LAYOUT_TILED,
            .dest = dst->bo,
            .dest_offset = 0,
            .dest_stride = dst->levels[0].stride,
            .dest_padded_height = dst->levels[0].padded_height,
            .downsample_x = 0,
            .downsample_y = 0,
            .swap_rb = 0,
            .dither = {0xffffffff, 0xffffffff},
            .clear_mode = VIVS_RS_CLEAR_CONTROL_MODE_DISABLED,
            .width = blit_info->dst.box.width,
            .height = blit_info->dst.box.height,
         });

      /* The combined color/depth cache flush is required to avoid pixels stuck
       * in the caches being flushed to the RS target. This seems to be some bug
       * found on at least GC2000, with no other known workaround.
       */
      etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE,
                     VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_DEPTH);
      etna_stall(ctx->stream, SYNC_RECIPIENT_RA, SYNC_RECIPIENT_PE);
      etna_set_state(ctx->stream, VIVS_TS_MEM_CONFIG, 0);
      ctx->dirty |= ETNA_DIRTY_TS;
      etna_submit_rs_state(ctx, &tile_blit);

      /* flush now, so we can get rid of the temp BO right here */
      etna_cmd_stream_flush(ctx->stream);
      etna_bo_del(temp_bo);
   }

   return TRUE;
}
