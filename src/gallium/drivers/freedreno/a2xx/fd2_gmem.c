/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2012 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"

#include "freedreno_draw.h"
#include "freedreno_state.h"
#include "freedreno_resource.h"

#include "fd2_gmem.h"
#include "fd2_context.h"
#include "fd2_emit.h"
#include "fd2_program.h"
#include "fd2_util.h"
#include "fd2_zsa.h"

static uint32_t fmt2swap(enum pipe_format format)
{
	switch (format) {
	case PIPE_FORMAT_B8G8R8A8_UNORM:
	case PIPE_FORMAT_B8G8R8X8_UNORM:
	case PIPE_FORMAT_B5G6R5_UNORM:
	case PIPE_FORMAT_B5G5R5A1_UNORM:
	case PIPE_FORMAT_B5G5R5X1_UNORM:
	case PIPE_FORMAT_B4G4R4A4_UNORM:
	case PIPE_FORMAT_B4G4R4X4_UNORM:
	/* TODO probably some more.. */
		return 1;
	default:
		return 0;
	}
}

/* transfer from gmem to system memory (ie. normal RAM) */

static void
emit_gmem2mem_surf(struct fd_batch *batch, uint32_t base,
		struct pipe_surface *psurf)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct fd_resource *rsc = fd_resource(psurf->texture);
	uint32_t swap = fmt2swap(psurf->format);
	if (psurf->u.tex.level != 0) // TODO: handle non-zero levels
		return;

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
	OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(swap) |
			A2XX_RB_COLOR_INFO_BASE(base) |
			A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(psurf->format)));

	OUT_PKT3(ring, CP_SET_CONSTANT, 5);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COPY_CONTROL));
	OUT_RING(ring, 0x00000000);             /* RB_COPY_CONTROL */
	OUT_RELOCW(ring, rsc->bo, 0, 0, 0);     /* RB_COPY_DEST_BASE */
	OUT_RING(ring, rsc->slices[0].pitch >> 5); /* RB_COPY_DEST_PITCH */
	OUT_RING(ring,                          /* RB_COPY_DEST_INFO */
			A2XX_RB_COPY_DEST_INFO_FORMAT(fd2_pipe2color(psurf->format)) |
			A2XX_RB_COPY_DEST_INFO_LINEAR |
			A2XX_RB_COPY_DEST_INFO_SWAP(swap) |
			A2XX_RB_COPY_DEST_INFO_WRITE_RED |
			A2XX_RB_COPY_DEST_INFO_WRITE_GREEN |
			A2XX_RB_COPY_DEST_INFO_WRITE_BLUE |
			A2XX_RB_COPY_DEST_INFO_WRITE_ALPHA);

	if (!is_a20x(batch->ctx->screen)) {
		OUT_WFI (ring);

		OUT_PKT3(ring, CP_SET_CONSTANT, 3);
		OUT_RING(ring, CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
		OUT_RING(ring, 3);                 /* VGT_MAX_VTX_INDX */
		OUT_RING(ring, 0);                 /* VGT_MIN_VTX_INDX */
	}

	fd_draw(batch, ring, DI_PT_RECTLIST, IGNORE_VISIBILITY,
			DI_SRC_SEL_AUTO_INDEX, 3, 0, INDEX_SIZE_IGN, 0, 0, NULL);
}

static void
fd2_emit_tile_gmem2mem(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_context *ctx = batch->ctx;
	struct fd2_context *fd2_ctx = fd2_context(ctx);
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;

	fd2_emit_vertex_bufs(ring, 0x9c, (struct fd2_vertex_buf[]) {
			{ .prsc = fd2_ctx->solid_vertexbuf, .size = 48 },
		}, 1);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
	OUT_RING(ring, 0x00000000);          /* PA_SC_WINDOW_OFFSET */

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_VGT_INDX_OFFSET));
	OUT_RING(ring, 0);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
	OUT_RING(ring, 0x0000028f);

	fd2_program_emit(batch, ring, &ctx->solid_prog);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_MASK));
	OUT_RING(ring, 0x0000ffff);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_DEPTHCONTROL));
	OUT_RING(ring, A2XX_RB_DEPTHCONTROL_EARLY_Z_ENABLE);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
	OUT_RING(ring, A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST |  /* PA_SU_SC_MODE_CNTL */
			A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
			A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

	OUT_PKT3(ring, CP_SET_CONSTANT, 3);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
	OUT_RING(ring, xy2d(0, 0));                       /* PA_SC_WINDOW_SCISSOR_TL */
	OUT_RING(ring, xy2d(pfb->width, pfb->height));    /* PA_SC_WINDOW_SCISSOR_BR */

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VTE_CNTL));
	OUT_RING(ring, A2XX_PA_CL_VTE_CNTL_VTX_W0_FMT |
			A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_CLIP_CNTL));
	OUT_RING(ring, 0x00000000);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_MODECONTROL));
	OUT_RING(ring, A2XX_RB_MODECONTROL_EDRAM_MODE(EDRAM_COPY));

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COPY_DEST_OFFSET));
	OUT_RING(ring, A2XX_RB_COPY_DEST_OFFSET_X(tile->xoff) |
			A2XX_RB_COPY_DEST_OFFSET_Y(tile->yoff));

	if (batch->resolve & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL))
		emit_gmem2mem_surf(batch, tile->bin_w * tile->bin_h, pfb->zsbuf);

	if (batch->resolve & FD_BUFFER_COLOR)
		emit_gmem2mem_surf(batch, 0, pfb->cbufs[0]);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_MODECONTROL));
	OUT_RING(ring, A2XX_RB_MODECONTROL_EDRAM_MODE(COLOR_DEPTH));
}

/* transfer from system memory to gmem */

static void
emit_mem2gmem_surf(struct fd_batch *batch, uint32_t base,
		struct pipe_surface *psurf)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct fd_resource *rsc = fd_resource(psurf->texture);
	uint32_t swiz;
	if (psurf->u.tex.level != 0) // TODO: handle non-zero levels
		return;

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
	OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(fmt2swap(psurf->format)) |
			A2XX_RB_COLOR_INFO_BASE(base) |
			A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(psurf->format)));

	swiz = fd2_tex_swiz(psurf->format, PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
			PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W);

	/* emit fb as a texture: */
	OUT_PKT3(ring, CP_SET_CONSTANT, 7);
	OUT_RING(ring, 0x00010000);
	OUT_RING(ring, A2XX_SQ_TEX_0_CLAMP_X(SQ_TEX_WRAP) |
			A2XX_SQ_TEX_0_CLAMP_Y(SQ_TEX_WRAP) |
			A2XX_SQ_TEX_0_CLAMP_Z(SQ_TEX_WRAP) |
			A2XX_SQ_TEX_0_PITCH(rsc->slices[0].pitch));
	OUT_RELOC(ring, rsc->bo, 0,
			fd2_pipe2surface(psurf->format) | 0x800, 0);
	OUT_RING(ring, A2XX_SQ_TEX_2_WIDTH(psurf->width - 1) |
			A2XX_SQ_TEX_2_HEIGHT(psurf->height - 1));
	OUT_RING(ring, 0x01000000 | // XXX
			swiz |
			A2XX_SQ_TEX_3_XY_MAG_FILTER(SQ_TEX_FILTER_POINT) |
			A2XX_SQ_TEX_3_XY_MIN_FILTER(SQ_TEX_FILTER_POINT));
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000200);

	if (!is_a20x(batch->ctx->screen)) {
		OUT_PKT3(ring, CP_SET_CONSTANT, 3);
		OUT_RING(ring, CP_REG(REG_A2XX_VGT_MAX_VTX_INDX));
		OUT_RING(ring, 3);                 /* VGT_MAX_VTX_INDX */
		OUT_RING(ring, 0);                 /* VGT_MIN_VTX_INDX */
	}

	fd_draw(batch, ring, DI_PT_RECTLIST, IGNORE_VISIBILITY,
			DI_SRC_SEL_AUTO_INDEX, 3, 0, INDEX_SIZE_IGN, 0, 0, NULL);
}

static void
fd2_emit_tile_mem2gmem(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_context *ctx = batch->ctx;
	struct fd2_context *fd2_ctx = fd2_context(ctx);
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	unsigned bin_w = tile->bin_w;
	unsigned bin_h = tile->bin_h;
	float x0, y0, x1, y1;

	fd2_emit_vertex_bufs(ring, 0x9c, (struct fd2_vertex_buf[]) {
			{ .prsc = fd2_ctx->solid_vertexbuf, .size = 48, .offset = 0x30 },
			{ .prsc = fd2_ctx->solid_vertexbuf, .size = 32, .offset = 0x60 },
		}, 2);

	/* write texture coordinates to vertexbuf: */
	x0 = ((float)tile->xoff) / ((float)pfb->width);
	x1 = ((float)tile->xoff + bin_w) / ((float)pfb->width);
	y0 = ((float)tile->yoff) / ((float)pfb->height);
	y1 = ((float)tile->yoff + bin_h) / ((float)pfb->height);
	OUT_PKT3(ring, CP_MEM_WRITE, 9);
	OUT_RELOC(ring, fd_resource(fd2_ctx->solid_vertexbuf)->bo, 0x60, 0, 0);
	OUT_RING(ring, fui(x0));
	OUT_RING(ring, fui(y0));
	OUT_RING(ring, fui(x1));
	OUT_RING(ring, fui(y0));
	OUT_RING(ring, fui(x0));
	OUT_RING(ring, fui(y1));
	OUT_RING(ring, fui(x1));
	OUT_RING(ring, fui(y1));

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_VGT_INDX_OFFSET));
	OUT_RING(ring, 0);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
	OUT_RING(ring, 0x0000003b);

	fd2_program_emit(batch, ring, &ctx->blit_prog[0]);

	OUT_PKT0(ring, REG_A2XX_TC_CNTL_STATUS, 1);
	OUT_RING(ring, A2XX_TC_CNTL_STATUS_L2_INVALIDATE);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_DEPTHCONTROL));
	OUT_RING(ring, A2XX_RB_DEPTHCONTROL_EARLY_Z_ENABLE);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SU_SC_MODE_CNTL));
	OUT_RING(ring, A2XX_PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST |
			A2XX_PA_SU_SC_MODE_CNTL_FRONT_PTYPE(PC_DRAW_TRIANGLES) |
			A2XX_PA_SU_SC_MODE_CNTL_BACK_PTYPE(PC_DRAW_TRIANGLES));

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_AA_MASK));
	OUT_RING(ring, 0x0000ffff);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COLORCONTROL));
	OUT_RING(ring, A2XX_RB_COLORCONTROL_ALPHA_FUNC(PIPE_FUNC_ALWAYS) |
			A2XX_RB_COLORCONTROL_BLEND_DISABLE |
			A2XX_RB_COLORCONTROL_ROP_CODE(12) |
			A2XX_RB_COLORCONTROL_DITHER_MODE(DITHER_DISABLE) |
			A2XX_RB_COLORCONTROL_DITHER_TYPE(DITHER_PIXEL));

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_BLEND_CONTROL));
	OUT_RING(ring, A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND(FACTOR_ONE) |
			A2XX_RB_BLEND_CONTROL_COLOR_COMB_FCN(BLEND2_DST_PLUS_SRC) |
			A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND(FACTOR_ZERO) |
			A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND(FACTOR_ONE) |
			A2XX_RB_BLEND_CONTROL_ALPHA_COMB_FCN(BLEND2_DST_PLUS_SRC) |
			A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND(FACTOR_ZERO));

	OUT_PKT3(ring, CP_SET_CONSTANT, 3);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_SCISSOR_TL));
	OUT_RING(ring, A2XX_PA_SC_WINDOW_OFFSET_DISABLE |
			xy2d(0,0));                     /* PA_SC_WINDOW_SCISSOR_TL */
	OUT_RING(ring, xy2d(bin_w, bin_h));     /* PA_SC_WINDOW_SCISSOR_BR */

	OUT_PKT3(ring, CP_SET_CONSTANT, 5);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VPORT_XSCALE));
	OUT_RING(ring, fui((float)bin_w/2.0));  /* PA_CL_VPORT_XSCALE */
	OUT_RING(ring, fui((float)bin_w/2.0));  /* PA_CL_VPORT_XOFFSET */
	OUT_RING(ring, fui(-(float)bin_h/2.0)); /* PA_CL_VPORT_YSCALE */
	OUT_RING(ring, fui((float)bin_h/2.0));  /* PA_CL_VPORT_YOFFSET */

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_VTE_CNTL));
	OUT_RING(ring, A2XX_PA_CL_VTE_CNTL_VTX_XY_FMT |
			A2XX_PA_CL_VTE_CNTL_VTX_Z_FMT |       // XXX check this???
			A2XX_PA_CL_VTE_CNTL_VPORT_X_SCALE_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_X_OFFSET_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_Y_SCALE_ENA |
			A2XX_PA_CL_VTE_CNTL_VPORT_Y_OFFSET_ENA);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_CL_CLIP_CNTL));
	OUT_RING(ring, 0x00000000);

	if (fd_gmem_needs_restore(batch, tile, FD_BUFFER_DEPTH | FD_BUFFER_STENCIL))
		emit_mem2gmem_surf(batch, bin_w * bin_h, pfb->zsbuf);

	if (fd_gmem_needs_restore(batch, tile, FD_BUFFER_COLOR))
		emit_mem2gmem_surf(batch, 0, pfb->cbufs[0]);

	/* TODO blob driver seems to toss in a CACHE_FLUSH after each DRAW_INDX.. */
}

/* before first tile */
static void
fd2_emit_tile_init(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	enum pipe_format format = pipe_surface_format(pfb->cbufs[0]);
	uint32_t reg;
	int i;

	fd2_emit_restore(ctx, ring);

	OUT_PKT3(ring, CP_SET_CONSTANT, 4);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_SURFACE_INFO));
	OUT_RING(ring, gmem->bin_w);                 /* RB_SURFACE_INFO */
	OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(fmt2swap(format)) |
			A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));
	reg = A2XX_RB_DEPTH_INFO_DEPTH_BASE(align(gmem->bin_w * gmem->bin_h, 4));
	if (pfb->zsbuf)
		reg |= A2XX_RB_DEPTH_INFO_DEPTH_FORMAT(fd_pipe2depth(pfb->zsbuf->format));
	OUT_RING(ring, reg);                         /* RB_DEPTH_INFO */

	if (is_a20x(ctx->screen) && !(fd_mesa_debug & FD_DBG_NOBIN)) {
		/* patch out unneeded memory exports by setting EXEC_END cf */
		for (i = 0; i < fd_patch_num_elements(&batch->draw_patches); i++) {
			struct fd_cs_patch *patch = fd_patch_element(&batch->draw_patches, i);
			*patch->cs = patch->val;

			instr_cf_t *cf = (instr_cf_t*) patch->cs;
			if (cf->opc == ALLOC)
				cf++;
			assert(cf->opc == EXEC);
			assert(cf[ctx->screen->num_vsc_pipes*2-2].opc == EXEC_END);
			cf[2*(ctx->num_vsc_pipe-1)].opc = EXEC_END;
		}

		/* initialize shader constants for the binning memexport */
		OUT_PKT3(ring, CP_SET_CONSTANT, 1 + ctx->num_vsc_pipe * 4);
		OUT_RING(ring, 0x0000000C);

		for (i = 0; i < ctx->num_vsc_pipe; i++) {
			struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[i];

			/* XXX we know how large this needs to be..
			 * should do some sort of realloc
			 * should be ctx->batch->num_vertices bytes large
			 * with fixed 256kib it will break with more than 256k vertices
			 */
			if (!pipe->bo) {
				pipe->bo = fd_bo_new(ctx->dev, 0x40000,
					DRM_FREEDRENO_GEM_TYPE_KMEM);
			}

			/* memory export address (export32):
			 * .x: (base_address >> 2) | 0x40000000 (?)
			 * .y: index (float) - set by shader
			 * .z: 0x4B00D000 (?)
			 * .w: 0x4B000000 (?) | max_index (?)
			*/
			OUT_RELOCW(ring, pipe->bo, 0, 0x40000000, -2);
			OUT_RING(ring, 0x00000000);
			OUT_RING(ring, 0x4B00D000);
			OUT_RING(ring, 0x4B000000 | 0x40000);
		}

		OUT_PKT3(ring, CP_SET_CONSTANT, 1 + ctx->num_vsc_pipe * 8);
		OUT_RING(ring, 0x0000018C);

		for (i = 0; i < ctx->num_vsc_pipe; i++) {
			struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[i];
			float off_x, off_y, mul_x, mul_y;

			/* const to tranform from [-1,1] to bin coordinates for this pipe
			 * for x/y, [0,256/2040] = 0, [256/2040,512/2040] = 1, etc
			 * 8 possible values on x/y axis,
			 * to clip at binning stage: only use center 6x6
			 * TODO: set the z parameters too so that hw binning
			 * can clip primitives in Z too
			 * TODO: how does pointsize fit into this?
			 */

			mul_x = 1.0f / (float) (gmem->bin_w * 8);
			mul_y = 1.0f / (float) (gmem->bin_h * 8);
			off_x = -pipe->x * (1.0/8.0f) + 0.125f;
			off_y = -pipe->y * (1.0/8.0f) + 0.125f;

			OUT_RING(ring, fui(off_x * (256.0f/255.0f)));
			OUT_RING(ring, fui(off_y * (256.0f/255.0f)));
			OUT_RING(ring, 0x3f000000);
			OUT_RING(ring, fui(0.0f));

			OUT_RING(ring, fui(mul_x * (256.0f/255.0f)));
			OUT_RING(ring, fui(mul_y * (256.0f/255.0f)));
			OUT_RING(ring, fui(0.0f));
			OUT_RING(ring, fui(0.0f));
		}

		OUT_PKT3(ring, CP_SET_CONSTANT, 2);
		OUT_RING(ring, CP_REG(REG_A2XX_VGT_VERTEX_REUSE_BLOCK_CNTL));
		OUT_RING(ring, 0);

		ctx->emit_ib(ring, batch->binning);
	}

	util_dynarray_resize(&batch->draw_patches, 0);
}

/* before mem2gmem */
static void
fd2_emit_tile_prep(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	enum pipe_format format = pipe_surface_format(pfb->cbufs[0]);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
	OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(1) | /* RB_COLOR_INFO */
			A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

	/* setup screen scissor for current tile (same for mem2gmem): */
	OUT_PKT3(ring, CP_SET_CONSTANT, 3);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_SCREEN_SCISSOR_TL));
	OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_TL_X(0) |
			A2XX_PA_SC_SCREEN_SCISSOR_TL_Y(0));
	OUT_RING(ring, A2XX_PA_SC_SCREEN_SCISSOR_BR_X(tile->bin_w) |
			A2XX_PA_SC_SCREEN_SCISSOR_BR_Y(tile->bin_h));
}

/* before IB to rendering cmds: */
static void
fd2_emit_tile_renderprep(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	enum pipe_format format = pipe_surface_format(pfb->cbufs[0]);

	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_RB_COLOR_INFO));
	OUT_RING(ring, A2XX_RB_COLOR_INFO_SWAP(fmt2swap(format)) |
			A2XX_RB_COLOR_INFO_FORMAT(fd2_pipe2color(format)));

	/* setup window scissor and offset for current tile (different
	 * from mem2gmem):
	 */
	OUT_PKT3(ring, CP_SET_CONSTANT, 2);
	OUT_RING(ring, CP_REG(REG_A2XX_PA_SC_WINDOW_OFFSET));
	OUT_RING(ring, A2XX_PA_SC_WINDOW_OFFSET_X(-tile->xoff) |
			A2XX_PA_SC_WINDOW_OFFSET_Y(-tile->yoff));

	if (is_a20x(ctx->screen) && !(fd_mesa_debug & FD_DBG_NOBIN)) {
		struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[tile->p];

		OUT_PKT3(ring, CP_SET_CONSTANT, 2);
		OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MIN));
		OUT_RING(ring, tile->n);

		OUT_PKT3(ring, CP_SET_CONSTANT, 2);
		OUT_RING(ring, CP_REG(REG_A2XX_VGT_CURRENT_BIN_ID_MAX));
		OUT_RING(ring, tile->n);

		/* TODO only emit this when tile->p changes */
		OUT_PKT3(ring, CP_SET_DRAW_INIT_FLAGS, 1);
		OUT_RELOC(ring, pipe->bo, 0, 0, 0);
	}
}

void
fd2_gmem_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	ctx->emit_tile_init = fd2_emit_tile_init;
	ctx->emit_tile_prep = fd2_emit_tile_prep;
	ctx->emit_tile_mem2gmem = fd2_emit_tile_mem2gmem;
	ctx->emit_tile_renderprep = fd2_emit_tile_renderprep;
	ctx->emit_tile_gmem2mem = fd2_emit_tile_gmem2mem;
}
