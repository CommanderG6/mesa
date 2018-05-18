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

#include "pipe/p_state.h"
#include "util/u_format.h"

#include "hw/state_2d.xml.h"

#include <assert.h>

#define EMIT_STATE(state_name, src_value) \
   etna_coalsence_emit(stream, &coalesce, VIVS_##state_name, src_value)

#define EMIT_STATE_RELOC(state_name, src_value) \
   etna_coalsence_emit_reloc(stream, &coalesce, VIVS_##state_name, src_value)

bool etna_try_2d_blit(struct pipe_context *pctx,
                      const struct pipe_blit_info *blit_info)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_cmd_stream *stream = ctx->stream2d;
   struct etna_coalesce coalesce;
   struct etna_reloc ry, ru, rv, rdst;
   struct pipe_resource *res_y, *res_u, *res_v, *res_dst;
   uint32_t src_format;

   assert(util_format_is_yuv(blit_info->src.format));
   assert(blit_info->dst.format == PIPE_FORMAT_R8G8B8A8_UNORM);

   if (!stream)
      return FALSE;

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
   rdst.bo = etna_resource(res_dst)->bo;
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
   EMIT_STATE(DE_DEST_CONFIG,
              VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
              VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT |
              VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ABGR) |
              VIVS_DE_DEST_CONFIG_TILED_ENABLE);
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

   etna_coalesce_end(stream, &coalesce);

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

   etna_set_state(stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);

   /* Flush now, this avoid the need to track cross-dependencies between
    * 2D and 3D GPU. The 3D GPU will only read from the 2D prepared buffers,
    * so the kernel is taking care of any needed synchronization.
    */
   etna_cmd_stream_flush(ctx->stream2d);

   return TRUE;
}
