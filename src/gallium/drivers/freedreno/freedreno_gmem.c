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
#include "util/u_format.h"

#include "freedreno_gmem.h"
#include "freedreno_context.h"
#include "freedreno_fence.h"
#include "freedreno_resource.h"
#include "freedreno_query_hw.h"
#include "freedreno_util.h"

/*
 * GMEM is the small (ie. 256KiB for a200, 512KiB for a220, etc) tile buffer
 * inside the GPU.  All rendering happens to GMEM.  Larger render targets
 * are split into tiles that are small enough for the color (and depth and/or
 * stencil, if enabled) buffers to fit within GMEM.  Before rendering a tile,
 * if there was not a clear invalidating the previous tile contents, we need
 * to restore the previous tiles contents (system mem -> GMEM), and after all
 * the draw calls, before moving to the next tile, we need to save the tile
 * contents (GMEM -> system mem).
 *
 * The code in this file handles dealing with GMEM and tiling.
 *
 * The structure of the ringbuffer ends up being:
 *
 *     +--<---<-- IB ---<---+---<---+---<---<---<--+
 *     |                    |       |              |
 *     v                    ^       ^              ^
 *   ------------------------------------------------------
 *     | clear/draw cmds | Tile0 | Tile1 | .... | TileN |
 *   ------------------------------------------------------
 *                       ^
 *                       |
 *                       address submitted in issueibcmds
 *
 * Where the per-tile section handles scissor setup, mem2gmem restore (if
 * needed), IB to draw cmds earlier in the ringbuffer, and then gmem2mem
 * resolve.
 */

static uint32_t bin_width(struct fd_screen *screen)
{
	if (is_a4xx(screen) || is_a5xx(screen))
		return 1024;
	if (is_a3xx(screen))
		return 992;
	return 512;
}

static uint32_t
total_size(uint8_t cbuf_cpp[], uint8_t zsbuf_cpp[2],
		   uint32_t bin_w, uint32_t bin_h, struct fd_gmem_stateobj *gmem)
{
	uint32_t total = 0, i;

	for (i = 0; i < MAX_RENDER_TARGETS; i++) {
		if (cbuf_cpp[i]) {
			gmem->cbuf_base[i] = align(total, 0x4000);
			total = gmem->cbuf_base[i] + cbuf_cpp[i] * bin_w * bin_h;
		}
	}

	if (zsbuf_cpp[0]) {
		gmem->zsbuf_base[0] = align(total, 0x4000);
		total = gmem->zsbuf_base[0] + zsbuf_cpp[0] * bin_w * bin_h;
	}

	if (zsbuf_cpp[1]) {
		gmem->zsbuf_base[1] = align(total, 0x4000);
		total = gmem->zsbuf_base[1] + zsbuf_cpp[1] * bin_w * bin_h;
	}

	return total;
}

static int32_t
step_bin_up(int32_t margin, uint32_t *bin_w, uint32_t *bin_n, uint32_t align)
{
	/* get the next width,num point, increasing width */
	uint32_t w = *bin_w, n = *bin_n;

	assert(n > 1);

	do {
		w += align;
		margin += n * align;
	} while (margin < w);

	do {
		n--;
		margin -= w;
	} while (margin >= w);

	*bin_w = w;
	*bin_n = n;
	return margin;
}

static int32_t
step_bin_down(int32_t margin, uint32_t *bin_w, uint32_t *bin_n, uint32_t align)
{
	/* get the next width,num point, decreasing width */
	uint32_t w = *bin_w, n = *bin_n;

	do {
		w -= align;
		margin -= n * align;
	} while (margin >= 0);

	assert(w);

	do {
		n++;
		margin += w;
	} while (margin < 0);

	*bin_w = w;
	*bin_n = n;
	return margin;
}

static void
calculate_tiles(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	struct pipe_scissor_state *scissor = &batch->max_scissor;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	const uint32_t gmem_alignw = ctx->screen->gmem_alignw;
	const uint32_t gmem_alignh = ctx->screen->gmem_alignh;
	const unsigned npipes = ctx->screen->num_vsc_pipes;
	const uint32_t gmem_size = ctx->screen->gmemsize_bytes;
	uint32_t minx, miny, width, height;
	uint32_t nbins_x, nbins_y, nbx, nby;
	uint32_t bin_w, bin_h, bw, bh;
	int32_t margin_x, margin_y;
	uint32_t max_width = bin_width(ctx->screen);
	uint8_t cbuf_cpp[MAX_RENDER_TARGETS] = {0}, zsbuf_cpp[2] = {0};
	uint32_t i, j, t, xoff, yoff;
	uint32_t tpp_x, tpp_y;
	bool has_zs = !!(batch->resolve & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL));
	int tile_n[npipes];

	if (has_zs) {
		struct fd_resource *rsc = fd_resource(pfb->zsbuf->texture);
		zsbuf_cpp[0] = rsc->cpp;
		if (rsc->stencil)
			zsbuf_cpp[1] = rsc->stencil->cpp;
	}
	for (i = 0; i < pfb->nr_cbufs; i++) {
		if (pfb->cbufs[i])
			cbuf_cpp[i] = util_format_get_blocksize(pfb->cbufs[i]->format);
		else
			cbuf_cpp[i] = 4;
		/* if MSAA, color buffers are super-sampled in GMEM: */
		cbuf_cpp[i] *= pfb->samples;
	}

	if (!memcmp(gmem->zsbuf_cpp, zsbuf_cpp, sizeof(zsbuf_cpp)) &&
		!memcmp(gmem->cbuf_cpp, cbuf_cpp, sizeof(cbuf_cpp)) &&
		!memcmp(&gmem->scissor, scissor, sizeof(gmem->scissor))) {
		/* everything is up-to-date */
		return;
	}

	if ((fd_mesa_debug & FD_DBG_NOSCIS) || scissor->minx == 65535) {
		minx = 0;
		miny = 0;
		width = pfb->width;
		height = pfb->height;
	} else {
		/* round down to multiple of alignment: */
		minx = scissor->minx & ~(gmem_alignw - 1);
		miny = scissor->miny & ~(gmem_alignh - 1);
		width = scissor->maxx - minx;
		height = scissor->maxy - miny;
	}

	if (fd_mesa_debug & FD_DBG_MSGS) {
		debug_printf("binning input: cbuf cpp:");
		for (i = 0; i < pfb->nr_cbufs; i++)
			debug_printf(" %d", cbuf_cpp[i]);
		debug_printf(", zsbuf cpp: %d; %dx%d\n",
				zsbuf_cpp[0], width, height);
	}

#define div_round_up(v, a)  (((v) + (a) - 1) / (a))
	/* algorithm : decrease nbins_x while increasing nbins_y accordingly,
	 * keeping the best result to minimize the number of bins
	 * for cases with same # of bins, nbins_x + nbins_y is mimimized
	 */

	/* force cmp < 0 on 1st iter */
	nbins_x = 0xffff;
	nbins_y = 1;

	bw = gmem_alignw;
	nbx = div_round_up(width, bw);
	margin_x = bw * nbx - width;

	bh = align(height, gmem_alignh);
	nby = 1;
	margin_y = bh * nby - height;

	while (bw <= max_width) {
		int cmp;

		while (total_size(cbuf_cpp, zsbuf_cpp, bw, bh, gmem) > gmem_size)
			margin_y = step_bin_down(margin_y, &bh, &nby, gmem_alignh);

		cmp = nbx * nby - nbins_x * nbins_y;
		if (cmp < 0 || (cmp == 0 && nbx + nby < nbins_x + nbins_y)) {
			nbins_x = nbx;
			nbins_y = nby;
			bin_w = bw;
			bin_h = bh;
		}

		if (nbx <= 1)
			break;

		margin_x = step_bin_up(margin_x, &bw, &nbx, gmem_alignw);
	}

	/* needed to update offsets */
	total_size(cbuf_cpp, zsbuf_cpp, bin_w, bin_h, gmem);

	DBG("using %d bins of size %dx%d", nbins_x*nbins_y, bin_w, bin_h);

	gmem->scissor = *scissor;
	memcpy(gmem->cbuf_cpp, cbuf_cpp, sizeof(cbuf_cpp));
	memcpy(gmem->zsbuf_cpp, zsbuf_cpp, sizeof(zsbuf_cpp));
	gmem->bin_h = bin_h;
	gmem->bin_w = bin_w;
	gmem->nbins_x = nbins_x;
	gmem->nbins_y = nbins_y;
	gmem->minx = minx;
	gmem->miny = miny;
	gmem->width = width;
	gmem->height = height;

	/*
	 * Assign tiles and pipes:
	 *
	 * At some point it might be worth playing with different
	 * strategies and seeing if that makes much impact on
	 * performance.
	 */

	/* figure out number of tiles per pipe: */
	if (is_a20x(ctx->screen)) {
		/* for a20x we want to minimize the number of "pipes"
		 * binning data has 3 bits for x/y (8x8) but the edges are used to
		 * cull off-screen vertices with hw binning, so we have 6x6 pipes
		 */
		tpp_x = 6;
		tpp_y = 6;
	} else {
		tpp_x = tpp_y = 1;
		while (div_round_up(nbins_y, tpp_y) > 8)
			tpp_y += 2;
		while ((div_round_up(nbins_y, tpp_y) *
				div_round_up(nbins_x, tpp_x)) > 8)
			tpp_x += 1;
	}

	gmem->maxpw = tpp_x;
	gmem->maxph = tpp_y;

	/* configure pipes: */
	xoff = yoff = 0;
	for (i = 0; i < npipes; i++) {
		struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[i];

		if (xoff >= nbins_x) {
			xoff = 0;
			yoff += tpp_y;
		}

		if (yoff >= nbins_y) {
			break;
		}

		pipe->x = xoff;
		pipe->y = yoff;
		pipe->w = MIN2(tpp_x, nbins_x - xoff);
		pipe->h = MIN2(tpp_y, nbins_y - yoff);

		xoff += tpp_x;
	}
	/* number of pipes to use (for a20x)
	 * at least 1 pipe is needed
	 */
	ctx->num_vsc_pipe = i ? i : 1;

	for (; i < npipes; i++) {
		struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[i];
		pipe->x = pipe->y = pipe->w = pipe->h = 0;
	}

#if 0 /* debug */
	printf("%dx%d ... tpp=%dx%d\n", nbins_x, nbins_y, tpp_x, tpp_y);
	for (i = 0; i < 8; i++) {
		struct fd_vsc_pipe *pipe = &ctx->pipe[i];
		printf("pipe[%d]: %ux%u @ %u,%u\n", i,
				pipe->w, pipe->h, pipe->x, pipe->y);
	}
#endif

	/* configure tiles: */
	t = 0;
	yoff = miny;
	memset(tile_n, 0, sizeof(tile_n));
	for (i = 0; i < nbins_y; i++) {
		uint32_t bw, bh;

		xoff = minx;

		/* clip bin height: */
		bh = MIN2(bin_h, miny + height - yoff);

		for (j = 0; j < nbins_x; j++) {
			struct fd_tile *tile = &ctx->tile[t];
			uint32_t p;

			assert(t < ARRAY_SIZE(ctx->tile));

			/* pipe number: */
			p = ((i / tpp_y) * div_round_up(nbins_x, tpp_x)) + (j / tpp_x);
			assert(p < ctx->num_vsc_pipe);

			/* clip bin width: */
			bw = MIN2(bin_w, minx + width - xoff);
			tile->n = !is_a20x(ctx->screen) ? tile_n[p]++ :
				((i % tpp_y + 1) << 3 | (j % tpp_x + 1));
			tile->p = p;
			tile->bin_w = bw;
			tile->bin_h = bh;
			tile->xoff = xoff;
			tile->yoff = yoff;

			t++;

			xoff += bw;
		}

		yoff += bh;
	}

#if 0 /* debug */
	t = 0;
	for (i = 0; i < nbins_y; i++) {
		for (j = 0; j < nbins_x; j++) {
			struct fd_tile *tile = &ctx->tile[t++];
			printf("|p:%u n:%u|", tile->p, tile->n);
		}
		printf("\n");
	}
#endif
}

static void
render_tiles(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	int i;

	ctx->emit_tile_init(batch);

	if (batch->restore)
		ctx->stats.batch_restore++;

	for (i = 0; i < (gmem->nbins_x * gmem->nbins_y); i++) {
		struct fd_tile *tile = &ctx->tile[i];

		DBG("bin_h=%d, yoff=%d, bin_w=%d, xoff=%d",
			tile->bin_h, tile->yoff, tile->bin_w, tile->xoff);

		ctx->emit_tile_prep(batch, tile);

		if (batch->restore) {
			ctx->emit_tile_mem2gmem(batch, tile);
		}

		ctx->emit_tile_renderprep(batch, tile);

		if (ctx->query_prepare_tile)
			ctx->query_prepare_tile(batch, i, batch->gmem);

		/* emit IB to drawcmds: */
		ctx->emit_ib(batch->gmem, batch->draw);
		fd_reset_wfi(batch);

		/* emit gmem2mem to transfer tile back to system memory: */
		ctx->emit_tile_gmem2mem(batch, tile);
	}

	if (ctx->emit_tile_fini)
		ctx->emit_tile_fini(batch);
}

static void
render_sysmem(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;

	ctx->emit_sysmem_prep(batch);

	if (ctx->query_prepare_tile)
		ctx->query_prepare_tile(batch, 0, batch->gmem);

	/* emit IB to drawcmds: */
	ctx->emit_ib(batch->gmem, batch->draw);
	fd_reset_wfi(batch);

	if (ctx->emit_sysmem_fini)
		ctx->emit_sysmem_fini(batch);
}

static void
flush_ring(struct fd_batch *batch)
{
	/* for compute/blit batch, there is no batch->gmem, only batch->draw: */
	struct fd_ringbuffer *ring = batch->nondraw ? batch->draw : batch->gmem;
	uint32_t timestamp;
	int out_fence_fd = -1;

	fd_ringbuffer_flush2(ring, batch->in_fence_fd,
			batch->needs_out_fence_fd ? &out_fence_fd : NULL);

	timestamp = fd_ringbuffer_timestamp(ring);
	fd_fence_populate(batch->fence, timestamp, out_fence_fd);
}

void
fd_gmem_render_tiles(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	bool sysmem = false;

	if (ctx->emit_sysmem_prep && !batch->nondraw) {
		if (batch->cleared || batch->gmem_reason ||
				((batch->num_draws > 5) && !batch->blit) ||
				(pfb->samples > 1)) {
			DBG("GMEM: cleared=%x, gmem_reason=%x, num_draws=%u, samples=%u",
				batch->cleared, batch->gmem_reason, batch->num_draws,
				pfb->samples);
		} else if (!(fd_mesa_debug & FD_DBG_NOBYPASS)) {
			sysmem = true;
		}

		/* For ARB_framebuffer_no_attachments: */
		if ((pfb->nr_cbufs == 0) && !pfb->zsbuf) {
			sysmem = true;
		}
	}

	fd_reset_wfi(batch);

	ctx->stats.batch_total++;

	if (batch->nondraw) {
		DBG("%p: rendering non-draw", batch);
		ctx->stats.batch_nondraw++;
	} else if (sysmem) {
		DBG("%p: rendering sysmem %ux%u (%s/%s)",
			batch, pfb->width, pfb->height,
			util_format_short_name(pipe_surface_format(pfb->cbufs[0])),
			util_format_short_name(pipe_surface_format(pfb->zsbuf)));
		if (ctx->query_prepare)
			ctx->query_prepare(batch, 1);
		render_sysmem(batch);
		ctx->stats.batch_sysmem++;
	} else {
		struct fd_gmem_stateobj *gmem = &ctx->gmem;
		calculate_tiles(batch);
		DBG("%p: rendering %dx%d tiles %ux%u (%s/%s)",
			batch, pfb->width, pfb->height, gmem->nbins_x, gmem->nbins_y,
			util_format_short_name(pipe_surface_format(pfb->cbufs[0])),
			util_format_short_name(pipe_surface_format(pfb->zsbuf)));
		if (ctx->query_prepare)
			ctx->query_prepare(batch, gmem->nbins_x * gmem->nbins_y);
		render_tiles(batch);
		ctx->stats.batch_gmem++;
	}

	flush_ring(batch);
}

/* tile needs restore if it isn't completely contained within the
 * cleared scissor:
 */
static bool
skip_restore(struct pipe_scissor_state *scissor, struct fd_tile *tile)
{
	unsigned minx = tile->xoff;
	unsigned maxx = tile->xoff + tile->bin_w;
	unsigned miny = tile->yoff;
	unsigned maxy = tile->yoff + tile->bin_h;
	return (minx >= scissor->minx) && (maxx <= scissor->maxx) &&
			(miny >= scissor->miny) && (maxy <= scissor->maxy);
}

/* When deciding whether a tile needs mem2gmem, we need to take into
 * account the scissor rect(s) that were cleared.  To simplify we only
 * consider the last scissor rect for each buffer, since the common
 * case would be a single clear.
 */
bool
fd_gmem_needs_restore(struct fd_batch *batch, struct fd_tile *tile,
		uint32_t buffers)
{
	if (!(batch->restore & buffers))
		return false;

	/* if buffers partially cleared, then slow-path to figure out
	 * if this particular tile needs restoring:
	 */
	if ((buffers & FD_BUFFER_COLOR) &&
			(batch->partial_cleared & FD_BUFFER_COLOR) &&
			skip_restore(&batch->cleared_scissor.color, tile))
		return false;
	if ((buffers & FD_BUFFER_DEPTH) &&
			(batch->partial_cleared & FD_BUFFER_DEPTH) &&
			skip_restore(&batch->cleared_scissor.depth, tile))
		return false;
	if ((buffers & FD_BUFFER_STENCIL) &&
			(batch->partial_cleared & FD_BUFFER_STENCIL) &&
			skip_restore(&batch->cleared_scissor.stencil, tile))
		return false;

	return true;
}
