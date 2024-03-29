/*
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Authors:
 * Seung-Woo Kim <sw0312.kim@samsung.com>
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * Based on drivers/media/video/s5p-tv/mixer_reg.c
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include "drmP.h"

#include "regs-mixer.h"
#include "regs-vp.h"

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>

#include <drm/exynos_drm.h>

#include "exynos_drm_drv.h"
#include "exynos_drm_crtc.h"
#include "exynos_drm_display.h"

#include "exynos_hdmi.h"

#include <plat/map-base.h>
#ifdef CONFIG_EXYNOS_IOMMU
#include <mach/sysmmu.h>
#include <linux/of_platform.h>
#endif


#define get_mixer_context(dev)	platform_get_drvdata(to_platform_device(dev))

#define MIXER_WIN_NR		3
#define MIXER_DEFAULT_WIN	0

struct hdmi_win_data {
	dma_addr_t		dma_addr;
	dma_addr_t		chroma_dma_addr;
	uint32_t		pixel_format;
	unsigned int		bpp;
	unsigned int		crtc_x;
	unsigned int		crtc_y;
	unsigned int		crtc_width;
	unsigned int		crtc_height;
	unsigned int		fb_x;
	unsigned int		fb_y;
	unsigned int		fb_width;
	unsigned int		fb_height;
	unsigned int		mode_width;
	unsigned int		mode_height;
	unsigned int		scan_flags;
	bool			updated;
};

struct mixer_resources {
	struct device		*dev;
	int			irq;
	void __iomem		*mixer_regs;
	void __iomem		*vp_regs;
	spinlock_t		reg_slock;
	wait_queue_head_t	event_queue;
	struct clk		*mixer;
	struct clk		*vp;
	struct clk		*sclk_mixer;
	struct clk		*sclk_hdmi;
	struct clk		*sclk_dac;
	unsigned int		is_soc_exynos5;
};

struct mixer_context {
	struct device		*dev;
	struct drm_device	*drm_dev;
	unsigned int		irq;
	int			pipe;
	bool			interlace;
	bool			is_mixer_powered_on;
	bool			enabled[MIXER_WIN_NR];

	struct mixer_resources	mixer_res;
	struct hdmi_win_data	win_data[MIXER_WIN_NR];
	unsigned long		event_flags;
	int			previous_dxy;
};

/* event flags used  */
enum mixer_status_flags {
	MXR_EVENT_VSYNC = 1,
};

static const u8 filter_y_horiz_tap8[] = {
	0,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	0,	0,	0,
	0,	2,	4,	5,	6,	6,	6,	6,
	6,	5,	5,	4,	3,	2,	1,	1,
	0,	-6,	-12,	-16,	-18,	-20,	-21,	-20,
	-20,	-18,	-16,	-13,	-10,	-8,	-5,	-2,
	127,	126,	125,	121,	114,	107,	99,	89,
	79,	68,	57,	46,	35,	25,	16,	8,
};

static const u8 filter_y_vert_tap4[] = {
	0,	-3,	-6,	-8,	-8,	-8,	-8,	-7,
	-6,	-5,	-4,	-3,	-2,	-1,	-1,	0,
	127,	126,	124,	118,	111,	102,	92,	81,
	70,	59,	48,	37,	27,	19,	11,	5,
	0,	5,	11,	19,	27,	37,	48,	59,
	70,	81,	92,	102,	111,	118,	124,	126,
	0,	0,	-1,	-1,	-2,	-3,	-4,	-5,
	-6,	-7,	-8,	-8,	-8,	-8,	-6,	-3,
};

static const u8 filter_cr_horiz_tap4[] = {
	0,	-3,	-6,	-8,	-8,	-8,	-8,	-7,
	-6,	-5,	-4,	-3,	-2,	-1,	-1,	0,
	127,	126,	124,	118,	111,	102,	92,	81,
	70,	59,	48,	37,	27,	19,	11,	5,
};

static void mixer_win_reset(struct mixer_context *mctx);

static inline u32 vp_reg_read(struct mixer_resources *res, u32 reg_id)
{
	return readl(res->vp_regs + reg_id);
}

static inline void vp_reg_write(struct mixer_resources *res, u32 reg_id,
				 u32 val)
{
	writel(val, res->vp_regs + reg_id);
}

static inline void vp_reg_writemask(struct mixer_resources *res, u32 reg_id,
				 u32 val, u32 mask)
{
	u32 old = vp_reg_read(res, reg_id);

	val = (val & mask) | (old & ~mask);
	writel(val, res->vp_regs + reg_id);
}

static inline u32 mixer_reg_read(struct mixer_resources *res, u32 reg_id)
{
	return readl(res->mixer_regs + reg_id);
}

static inline void mixer_reg_write(struct mixer_resources *res, u32 reg_id,
				 u32 val)
{
	writel(val, res->mixer_regs + reg_id);
}

static inline void mixer_reg_writemask(struct mixer_resources *res,
				 u32 reg_id, u32 val, u32 mask)
{
	u32 old = mixer_reg_read(res, reg_id);

	val = (val & mask) | (old & ~mask);
	writel(val, res->mixer_regs + reg_id);
}

enum exynos_mixer_mode_type exynos_mixer_get_mode_type(int width, int height)
{
	if (width >= 464 && width <= 720 && height <= 480)
		return EXYNOS_MIXER_MODE_SD_NTSC;
	else if (width >= 464 && width <= 720 && height <= 576)
		return EXYNOS_MIXER_MODE_SD_PAL;
	else if (width >= 1024 && width <= 1280 && height <= 720)
		return EXYNOS_MIXER_MODE_HD_720;
	else if ((width == 1440 && height == 900) ||
		(width >= 1664 && width <= 1920 && height <= 1080))
		return EXYNOS_MIXER_MODE_HD_1080;
	else
		return EXYNOS_MIXER_MODE_INVALID;
}

static void mixer_regs_dump(struct mixer_context *mctx)
{
#define DUMPREG(reg_id) \
do { \
	DRM_DEBUG_KMS(#reg_id " = %08x\n", \
		(u32)readl(mctx->mixer_res.mixer_regs + reg_id)); \
} while (0)

	DUMPREG(MXR_STATUS);
	DUMPREG(MXR_CFG);
	DUMPREG(MXR_INT_EN);
	DUMPREG(MXR_INT_STATUS);

	DUMPREG(MXR_LAYER_CFG);
	DUMPREG(MXR_VIDEO_CFG);

	DUMPREG(MXR_GRAPHIC0_CFG);
	DUMPREG(MXR_GRAPHIC0_BASE);
	DUMPREG(MXR_GRAPHIC0_SPAN);
	DUMPREG(MXR_GRAPHIC0_WH);
	DUMPREG(MXR_GRAPHIC0_SXY);
	DUMPREG(MXR_GRAPHIC0_DXY);

	DUMPREG(MXR_GRAPHIC1_CFG);
	DUMPREG(MXR_GRAPHIC1_BASE);
	DUMPREG(MXR_GRAPHIC1_SPAN);
	DUMPREG(MXR_GRAPHIC1_WH);
	DUMPREG(MXR_GRAPHIC1_SXY);
	DUMPREG(MXR_GRAPHIC1_DXY);
#undef DUMPREG
}

static void vp_regs_dump(struct mixer_context *mctx)
{
#define DUMPREG(reg_id) \
do { \
	DRM_DEBUG_KMS(#reg_id " = %08x\n", \
		(u32) readl(mctx->mixer_res.vp_regs + reg_id)); \
} while (0)

	DUMPREG(VP_ENABLE);
	DUMPREG(VP_SRESET);
	DUMPREG(VP_SHADOW_UPDATE);
	DUMPREG(VP_FIELD_ID);
	DUMPREG(VP_MODE);
	DUMPREG(VP_IMG_SIZE_Y);
	DUMPREG(VP_IMG_SIZE_C);
	DUMPREG(VP_PER_RATE_CTRL);
	DUMPREG(VP_TOP_Y_PTR);
	DUMPREG(VP_BOT_Y_PTR);
	DUMPREG(VP_TOP_C_PTR);
	DUMPREG(VP_BOT_C_PTR);
	DUMPREG(VP_ENDIAN_MODE);
	DUMPREG(VP_SRC_H_POSITION);
	DUMPREG(VP_SRC_V_POSITION);
	DUMPREG(VP_SRC_WIDTH);
	DUMPREG(VP_SRC_HEIGHT);
	DUMPREG(VP_DST_H_POSITION);
	DUMPREG(VP_DST_V_POSITION);
	DUMPREG(VP_DST_WIDTH);
	DUMPREG(VP_DST_HEIGHT);
	DUMPREG(VP_H_RATIO);
	DUMPREG(VP_V_RATIO);

#undef DUMPREG
}

static inline void vp_filter_set(struct mixer_resources *res,
		int reg_id, const u8 *data, unsigned int size)
{
	/* assure 4-byte align */
	BUG_ON(size & 3);
	for (; size; size -= 4, reg_id += 4, data += 4) {
		u32 val = (data[0] << 24) |  (data[1] << 16) |
			(data[2] << 8) | data[3];
		vp_reg_write(res, reg_id, val);
	}
}

static void vp_default_filter(struct mixer_resources *res)
{
	vp_filter_set(res, VP_POLY8_Y0_LL,
		filter_y_horiz_tap8, sizeof filter_y_horiz_tap8);
	vp_filter_set(res, VP_POLY4_Y0_LL,
		filter_y_vert_tap4, sizeof filter_y_vert_tap4);
	vp_filter_set(res, VP_POLY4_C0_LL,
		filter_cr_horiz_tap4, sizeof filter_cr_horiz_tap4);
}

static void mixer_vsync_set_update(struct mixer_context *mctx, bool enable)
{
	struct mixer_resources *res = &mctx->mixer_res;

	/* block update on vsync */
	mixer_reg_writemask(res, MXR_STATUS, enable ?
			MXR_STATUS_SYNC_ENABLE : 0, MXR_STATUS_SYNC_ENABLE);

	if (!(res->is_soc_exynos5))
		vp_reg_write(res, VP_SHADOW_UPDATE, enable ?
				VP_SHADOW_UPDATE_ENABLE : 0);
}

static void mixer_cfg_scan(struct mixer_context *mctx, u32 width, u32 height)
{
	struct mixer_resources *res = &mctx->mixer_res;
	enum exynos_mixer_mode_type mode_type;
	u32 val;

	/* choosing between interlace and progressive mode */
	val = (mctx->interlace ? MXR_CFG_SCAN_INTERLACE :
				MXR_CFG_SCAN_PROGRASSIVE);

	/* choosing between proper HD and SD mode */
	mode_type = exynos_mixer_get_mode_type(width, height);
	switch (mode_type) {
	case EXYNOS_MIXER_MODE_SD_NTSC:
		val |= MXR_CFG_SCAN_NTSC | MXR_CFG_SCAN_SD;
		break;
	case EXYNOS_MIXER_MODE_SD_PAL:
		val |= MXR_CFG_SCAN_PAL | MXR_CFG_SCAN_SD;
		break;
	case EXYNOS_MIXER_MODE_HD_720:
		val |= MXR_CFG_SCAN_HD_720 | MXR_CFG_SCAN_HD;
		break;
	case EXYNOS_MIXER_MODE_HD_1080:
		val |= MXR_CFG_SCAN_HD_1080 | MXR_CFG_SCAN_HD;
		break;
	default:
		DRM_ERROR("Invalid mixer config %dx%d\n", width, height);
		return;
	}

	mixer_reg_writemask(res, MXR_CFG, val, MXR_CFG_SCAN_MASK);
}

static void mixer_set_layer_offset(struct mixer_context *mctx, u32 offset)
{
	struct mixer_resources *res = &mctx->mixer_res;
	int current_dxy = mixer_reg_read(res, MXR_GRAPHIC1_DXY);

	if (mctx->previous_dxy != current_dxy) {
		current_dxy += MXR_GRP_DXY_DX(offset);
		mixer_reg_write(res, MXR_GRAPHIC1_DXY, current_dxy);
		mctx->previous_dxy = current_dxy;
	}

	mixer_reg_write(res, MXR_GRAPHIC0_DXY, MXR_GRP_DXY_DX(offset));
}

static void mixer_cfg_rgb_fmt(struct mixer_context *mctx, unsigned int height)
{
	struct mixer_resources *res = &mctx->mixer_res;
	u32 val;

	if (height == 480) {
		val = MXR_CFG_RGB601_0_255;
	} else if (height == 576) {
		val = MXR_CFG_RGB601_0_255;
	} else if (height == 720) {
		val = MXR_CFG_RGB709_16_235;
		mixer_reg_write(res, MXR_CM_COEFF_Y,
				(1 << 30) | (94 << 20) | (314 << 10) |
				(32 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CB,
				(972 << 20) | (851 << 10) | (225 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CR,
				(225 << 20) | (820 << 10) | (1004 << 0));
	} else if (height == 1080) {
		val = MXR_CFG_RGB709_16_235;
		mixer_reg_write(res, MXR_CM_COEFF_Y,
				(1 << 30) | (94 << 20) | (314 << 10) |
				(32 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CB,
				(972 << 20) | (851 << 10) | (225 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CR,
				(225 << 20) | (820 << 10) | (1004 << 0));
	} else {
		val = MXR_CFG_RGB709_16_235;
		mixer_reg_write(res, MXR_CM_COEFF_Y,
				(1 << 30) | (94 << 20) | (314 << 10) |
				(32 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CB,
				(972 << 20) | (851 << 10) | (225 << 0));
		mixer_reg_write(res, MXR_CM_COEFF_CR,
				(225 << 20) | (820 << 10) | (1004 << 0));
	}

	mixer_reg_writemask(res, MXR_CFG, val, MXR_CFG_RGB_FMT_MASK);
}

static void mixer_cfg_layer(struct mixer_context *mctx, int win, bool enable)
{
	struct mixer_resources *res = &mctx->mixer_res;
	u32 val = enable ? ~0 : 0;

	switch (win) {
	case 0:
		mixer_reg_writemask(res, MXR_CFG, val, MXR_CFG_GRP0_ENABLE);
		break;
	case 1:
		mixer_reg_writemask(res, MXR_CFG, val, MXR_CFG_GRP1_ENABLE);
		break;
	case 2:
		vp_reg_writemask(res, VP_ENABLE, val, VP_ENABLE_ON);
		mixer_reg_writemask(res, MXR_CFG, val, MXR_CFG_VP_ENABLE);
		break;
	}
}

static void mixer_run(struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;

	mixer_reg_writemask(res, MXR_STATUS, ~0, MXR_STATUS_REG_RUN);

	mixer_regs_dump(mctx);
}

static int mixer_wait_for_vsync(struct mixer_context *mctx)
{
	int ret;

	mctx->event_flags |= MXR_EVENT_VSYNC;

	ret = wait_event_timeout(mctx->mixer_res.event_queue,
	((mctx->event_flags & MXR_EVENT_VSYNC) == 0), msecs_to_jiffies(1000));
	if (ret > 0)
		return 0;

	return -ETIME;
}

static int mixer_get_layer_update_count(struct mixer_context *ctx)
{
	struct mixer_resources *res = &ctx->mixer_res;
	u32 val;

	if (!res->is_soc_exynos5)
		return 0;

	val = mixer_reg_read(res, MXR_CFG);

	return (val & MXR_CFG_LAYER_UPDATE_COUNT_MASK) >>
			MXR_CFG_LAYER_UPDATE_COUNT0;
}

static void mixer_layer_update(struct mixer_context *ctx)
{
	struct mixer_resources *res = &ctx->mixer_res;

	if (!res->is_soc_exynos5)
		return;

	mixer_reg_writemask(res, MXR_CFG, ~0, MXR_CFG_LAYER_UPDATE);
}

static void vp_video_buffer(struct mixer_context *mctx, int win)
{
	struct mixer_resources *res = &mctx->mixer_res;
	unsigned long flags;
	struct hdmi_win_data *win_data;
	unsigned int full_width, full_height, width, height;
	unsigned int x_ratio, y_ratio;
	unsigned int src_x_offset, src_y_offset, dst_x_offset, dst_y_offset;
	unsigned int mode_width, mode_height;
	unsigned int buf_num;
	dma_addr_t luma_addr[2], chroma_addr[2];
	bool tiled_mode = false;
	bool crcb_mode = false;
	u32 val;

	win_data = &mctx->win_data[win];

	switch (win_data->pixel_format) {
	case DRM_FORMAT_NV12MT:
		tiled_mode = true;
	case DRM_FORMAT_NV12M:
		crcb_mode = false;
		buf_num = 2;
		break;
	/* TODO: single buffer format NV12, NV21 */
	default:
		/* ignore pixel format at disable time */
		if (!win_data->dma_addr)
			break;

		DRM_ERROR("pixel format for vp is wrong [%d].\n",
				win_data->pixel_format);
		return;
	}

	full_width = win_data->fb_width;
	full_height = win_data->fb_height;
	width = win_data->crtc_width;
	height = win_data->crtc_height;
	mode_width = win_data->mode_width;
	mode_height = win_data->mode_height;

	/* scaling feature: (src << 16) / dst */
	x_ratio = (width << 16) / width;
	y_ratio = (height << 16) / height;

	src_x_offset = win_data->fb_x;
	src_y_offset = win_data->fb_y;
	dst_x_offset = win_data->crtc_x;
	dst_y_offset = win_data->crtc_y;

	if (buf_num == 2) {
		luma_addr[0] = win_data->dma_addr;
		chroma_addr[0] = win_data->chroma_dma_addr;
	} else {
		luma_addr[0] = win_data->dma_addr;
		chroma_addr[0] = win_data->dma_addr
			+ (full_width * full_height);
	}

	if (win_data->scan_flags & DRM_MODE_FLAG_INTERLACE) {
		mctx->interlace = true;
		if (tiled_mode) {
			luma_addr[1] = luma_addr[0] + 0x40;
			chroma_addr[1] = chroma_addr[0] + 0x40;
		} else {
			luma_addr[1] = luma_addr[0] + full_width;
			chroma_addr[1] = chroma_addr[0] + full_width;
		}
	} else {
		mctx->interlace = false;
		luma_addr[1] = 0;
		chroma_addr[1] = 0;
	}

	spin_lock_irqsave(&res->reg_slock, flags);
	mixer_vsync_set_update(mctx, false);

	mctx->enabled[win] = true;

	/* interlace or progressive scan mode */
	val = (mctx->interlace ? ~0 : 0);
	vp_reg_writemask(res, VP_MODE, val, VP_MODE_LINE_SKIP);

	/* setup format */
	val = (crcb_mode ? VP_MODE_NV21 : VP_MODE_NV12);
	val |= (tiled_mode ? VP_MODE_MEM_TILED : VP_MODE_MEM_LINEAR);
	vp_reg_writemask(res, VP_MODE, val, VP_MODE_FMT_MASK);

	/* setting size of input image */
	vp_reg_write(res, VP_IMG_SIZE_Y, VP_IMG_HSIZE(full_width) |
		VP_IMG_VSIZE(full_height));
	/* chroma height has to reduced by 2 to avoid chroma distorions */
	vp_reg_write(res, VP_IMG_SIZE_C, VP_IMG_HSIZE(full_width) |
		VP_IMG_VSIZE(full_height / 2));

	vp_reg_write(res, VP_SRC_WIDTH, width);
	vp_reg_write(res, VP_SRC_HEIGHT, height);
	vp_reg_write(res, VP_SRC_H_POSITION,
			VP_SRC_H_POSITION_VAL(src_x_offset));
	vp_reg_write(res, VP_SRC_V_POSITION, src_y_offset);

	vp_reg_write(res, VP_DST_WIDTH, width);
	vp_reg_write(res, VP_DST_H_POSITION, dst_x_offset);
	if (mctx->interlace) {
		vp_reg_write(res, VP_DST_HEIGHT, height / 2);
		vp_reg_write(res, VP_DST_V_POSITION, dst_y_offset / 2);
	} else {
		vp_reg_write(res, VP_DST_HEIGHT, height);
		vp_reg_write(res, VP_DST_V_POSITION, dst_y_offset);
	}

	vp_reg_write(res, VP_H_RATIO, x_ratio);
	vp_reg_write(res, VP_V_RATIO, y_ratio);

	vp_reg_write(res, VP_ENDIAN_MODE, VP_ENDIAN_MODE_LITTLE);

	/* set buffer address to vp */
	vp_reg_write(res, VP_TOP_Y_PTR, luma_addr[0]);
	vp_reg_write(res, VP_BOT_Y_PTR, luma_addr[1]);
	vp_reg_write(res, VP_TOP_C_PTR, chroma_addr[0]);
	vp_reg_write(res, VP_BOT_C_PTR, chroma_addr[1]);

	mixer_cfg_scan(mctx, mode_width, mode_height);
	mixer_cfg_rgb_fmt(mctx, mode_height);
	mixer_cfg_layer(mctx, win, true);
	mixer_run(mctx);

	mixer_vsync_set_update(mctx, true);
	spin_unlock_irqrestore(&res->reg_slock, flags);

	vp_regs_dump(mctx);
}

static void mixer_graph_buffer(struct mixer_context *mctx, int win)
{
	struct mixer_resources *res = &mctx->mixer_res;
	unsigned long flags;
	struct hdmi_win_data *win_data;
	unsigned int full_width, width, height;
	unsigned int x_ratio, y_ratio;
	unsigned int src_x_offset, src_y_offset, dst_x_offset, dst_y_offset;
	unsigned int mode_width, mode_height;
	dma_addr_t dma_addr;
	unsigned int fmt;
	u32 val;

	win_data = &mctx->win_data[win];

	#define RGB565 4
	#define ARGB1555 5
	#define ARGB4444 6
	#define ARGB8888 7

	switch (win_data->bpp) {
	case 16:
		fmt = ARGB4444;
		break;
	case 32:
		fmt = ARGB8888;
		break;
	default:
		fmt = ARGB8888;
	}

	dma_addr = win_data->dma_addr;
	full_width = win_data->fb_width;
	width = win_data->crtc_width;
	height = win_data->crtc_height;
	mode_width = win_data->mode_width;
	mode_height = win_data->mode_height;

	/* 2x scaling feature */
	x_ratio = 0;
	y_ratio = 0;

	src_x_offset = win_data->fb_x;
	src_y_offset = win_data->fb_y;
	dst_x_offset = win_data->crtc_x;
	dst_y_offset = win_data->crtc_y;

	/* converting dma address base and source offset */
	dma_addr = dma_addr
		+ (src_x_offset * win_data->bpp >> 3)
		+ (src_y_offset * full_width * win_data->bpp >> 3);
	src_x_offset = 0;
	src_y_offset = 0;

	if (win_data->scan_flags & DRM_MODE_FLAG_INTERLACE)
		mctx->interlace = true;
	else
		mctx->interlace = false;

	spin_lock_irqsave(&res->reg_slock, flags);
	mixer_vsync_set_update(mctx, false);

	mctx->enabled[win] = true;

	/* setup format */
	mixer_reg_writemask(res, MXR_GRAPHIC_CFG(win),
		MXR_GRP_CFG_FORMAT_VAL(fmt), MXR_GRP_CFG_FORMAT_MASK);

	/* setup geometry */
	mixer_reg_write(res, MXR_GRAPHIC_SPAN(win), full_width);

	val  = MXR_GRP_WH_WIDTH(width);
	val |= MXR_GRP_WH_HEIGHT(height);
	val |= MXR_GRP_WH_H_SCALE(x_ratio);
	val |= MXR_GRP_WH_V_SCALE(y_ratio);
	mixer_reg_write(res, MXR_GRAPHIC_WH(win), val);

	/* setup offsets in source image */
	val  = MXR_GRP_SXY_SX(src_x_offset);
	val |= MXR_GRP_SXY_SY(src_y_offset);
	mixer_reg_write(res, MXR_GRAPHIC_SXY(win), val);

	/* setup offsets in display image */
	val  = MXR_GRP_DXY_DX(dst_x_offset);
	val |= MXR_GRP_DXY_DY(dst_y_offset);
	mixer_reg_write(res, MXR_GRAPHIC_DXY(win), val);

	/* set buffer address to mixer */
	mixer_reg_write(res, MXR_GRAPHIC_BASE(win), dma_addr);

	mixer_cfg_scan(mctx, mode_width, mode_height);

	/* Workaround 4 implementation for 1440x900 resolution support */
	if (res->is_soc_exynos5) {
		if (mode_width == 1440 && mode_height == 900)
			mixer_set_layer_offset(mctx, 224);
	}

	mixer_cfg_rgb_fmt(mctx, mode_height);
	mixer_cfg_layer(mctx, win, true);
	mixer_cfg_layer(mctx, MIXER_DEFAULT_WIN, true);

	/* Only allow one update per vsync */
	if (!win_data->updated)
		mixer_layer_update(mctx);

	win_data->updated = true;
	mixer_run(mctx);

	mixer_vsync_set_update(mctx, true);
	spin_unlock_irqrestore(&res->reg_slock, flags);
}

static void vp_win_reset(struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;
	int tries = 100;

	vp_reg_write(res, VP_SRESET, VP_SRESET_PROCESSING);
	for (tries = 100; tries; --tries) {
		/* waiting until VP_SRESET_PROCESSING is 0 */
		if (~vp_reg_read(res, VP_SRESET) & VP_SRESET_PROCESSING)
			break;
		mdelay(10);
	}
	WARN(tries == 0, "failed to reset Video Processor\n");
}

static int mixer_enable_vblank(void *ctx, int pipe)
{
	struct mixer_context *mctx = ctx;
	struct mixer_resources *res = &mctx->mixer_res;

	DRM_DEBUG_KMS("pipe: %d\n", pipe);

	/*
	 * TODO (seanpaul): Right now, this is an expected code path since we
	 * call enable_vblank in the poweron routine; pipe might not be
	 * initialized the first time we run it. We should refactor things such
	 * that this isn't the case and we can either BUG_ON or DRM_ERROR here.
	 */
	if (pipe < 0)
		return -EINVAL;

	if (!mctx->is_mixer_powered_on)
		return -EPERM;

	mctx->pipe = pipe;

	/* enable vsync interrupt */
	mixer_reg_writemask(res, MXR_INT_EN, MXR_INT_EN_VSYNC,
			MXR_INT_EN_VSYNC);

	return 0;
}

static void mixer_disable_vblank(void *ctx)
{
	struct mixer_context *mctx = ctx;
	struct mixer_resources *res = &mctx->mixer_res;

	DRM_DEBUG_KMS("pipe: %d\n", mctx->pipe);

	if (!mctx->is_mixer_powered_on)
		return;

	/* disable vsync interrupt */
	mixer_reg_writemask(res, MXR_INT_EN, 0, MXR_INT_EN_VSYNC);
}

static void mixer_win_mode_set(void *ctx,
			      struct exynos_drm_overlay *overlay)
{
	struct mixer_context *mctx = ctx;
	struct hdmi_win_data *win_data;
	int win;

	if (!overlay) {
		DRM_ERROR("overlay is NULL\n");
		return;
	}

	DRM_DEBUG_KMS("set [%d]x[%d] at (%d,%d) to [%d]x[%d] at (%d,%d)\n",
				 overlay->fb_width, overlay->fb_height,
				 overlay->fb_x, overlay->fb_y,
				 overlay->crtc_width, overlay->crtc_height,
				 overlay->crtc_x, overlay->crtc_y);

	win = overlay->zpos;
	if (win == DEFAULT_ZPOS)
		win = MIXER_DEFAULT_WIN;

	if (win < 0 || win > MIXER_WIN_NR) {
		DRM_ERROR("overlay plane[%d] is wrong\n", win);
		return;
	}

	win_data = &mctx->win_data[win];

	win_data->dma_addr = overlay->dma_addr[0];
	win_data->chroma_dma_addr = overlay->dma_addr[1];
	win_data->pixel_format = overlay->pixel_format;
	win_data->bpp = overlay->bpp;

	win_data->crtc_x = overlay->crtc_x;
	win_data->crtc_y = overlay->crtc_y;
	win_data->crtc_width = overlay->crtc_width;
	win_data->crtc_height = overlay->crtc_height;

	win_data->fb_x = overlay->fb_x;
	win_data->fb_y = overlay->fb_y;
	win_data->fb_width = overlay->fb_pitch / (overlay->bpp >> 3);
	win_data->fb_height = overlay->fb_height;

	win_data->mode_width = overlay->mode_width;
	win_data->mode_height = overlay->mode_height;

	win_data->scan_flags = overlay->scan_flag;
}

static void mixer_win_commit(void *ctx, int zpos)
{
	struct mixer_context *mctx = ctx;
	struct mixer_resources *res = &mctx->mixer_res;
	int win = zpos;

	DRM_DEBUG_KMS("win: %d\n", win);

	if (win == DEFAULT_ZPOS)
		win = MIXER_DEFAULT_WIN;

	if (win < 0 || win > MIXER_WIN_NR) {
		DRM_ERROR("overlay plane[%d] is wrong\n", win);
		return;
	}

	if (!mctx->is_mixer_powered_on) {
		DRM_DEBUG_KMS("not powered on\n");
		return;
	}

	if (!(res->is_soc_exynos5)) {
		if (win > 1)
			vp_video_buffer(mctx, win);
		else
			mixer_graph_buffer(mctx, win);
	}
	else
		mixer_graph_buffer(mctx, win);
}

static void mixer_apply(void *ctx)
{
	struct mixer_context *mctx = ctx;
	int i;

	DRM_DEBUG_KMS("\n");

	for (i = 0; i < MIXER_WIN_NR; i++) {
		if (!mctx->enabled[i])
			continue;

		mixer_win_commit(ctx, i);
	}
}

static void mixer_win_disable(void *ctx, int zpos)
{
	struct mixer_context *mctx = ctx;
	struct mixer_resources *res = &mctx->mixer_res;
	unsigned long flags;
	int win = zpos;

	DRM_DEBUG_KMS("win: %d\n", win);

	if (win == DEFAULT_ZPOS)
		win = MIXER_DEFAULT_WIN;

	if (win < 0 || win > MIXER_WIN_NR) {
		DRM_ERROR("overlay plane[%d] is wrong\n", win);
		return;
	}

	if (!mctx->is_mixer_powered_on)
		return;

	mixer_wait_for_vsync(mctx);

	spin_lock_irqsave(&res->reg_slock, flags);
	mixer_vsync_set_update(mctx, false);

	mctx->enabled[win] = false;
	mixer_cfg_layer(mctx, win, false);

	mixer_vsync_set_update(mctx, true);

	spin_unlock_irqrestore(&res->reg_slock, flags);
}

/* for pageflip event */
static irqreturn_t mixer_irq_handler(int irq, void *arg)
{
	struct mixer_context *mctx = arg;
	struct mixer_resources *res = &mctx->mixer_res;
	u32 val, base, shadow;
	bool flip_complete = false;
	int i;

	spin_lock(&res->reg_slock);

	WARN_ON(!mctx->is_mixer_powered_on);

	/* read interrupt status for handling and clearing flags for VSYNC */
	val = mixer_reg_read(res, MXR_INT_STATUS);

	/* handling VSYNC */
	if (val & MXR_INT_STATUS_VSYNC) {
		/* interlace scan need to check shadow register */
		if (mctx->interlace && !res->is_soc_exynos5) {
			base = mixer_reg_read(res, MXR_GRAPHIC_BASE(0));
			shadow = mixer_reg_read(res, MXR_GRAPHIC_BASE_S(0));
			if (base != shadow)
				goto out;

			base = mixer_reg_read(res, MXR_GRAPHIC_BASE(1));
			shadow = mixer_reg_read(res, MXR_GRAPHIC_BASE_S(1));
			if (base != shadow)
				goto out;
		}

		drm_handle_vblank(mctx->drm_dev, mctx->pipe);

		/* Bail out if a layer update is pending */
		if (mixer_get_layer_update_count(mctx))
			goto out;

		for (i = 0; i < MIXER_WIN_NR; i++)
			mctx->win_data[i].updated = false;

		flip_complete = true;

		if (mctx->event_flags & MXR_EVENT_VSYNC) {
			DRM_DEBUG_KMS("mctx->event_flags & MXR_EVENT_VSYNC");

			mctx->event_flags &= ~MXR_EVENT_VSYNC;
			wake_up(&mctx->mixer_res.event_queue);
		}
	}

out:
	/* clear interrupts */
	if (~val & MXR_INT_EN_VSYNC) {
		/* vsync interrupt use different bit for read and clear */
		val &= ~MXR_INT_EN_VSYNC;
		val |= MXR_INT_CLEAR_VSYNC;
	}
	mixer_reg_write(res, MXR_INT_STATUS, val);

	spin_unlock(&res->reg_slock);

	if (flip_complete)
		exynos_drm_crtc_finish_pageflip(mctx->drm_dev, mctx->pipe);

	return IRQ_HANDLED;
}

static void mixer_win_reset(struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;
	unsigned long flags;
	u32 val; /* value stored to register */

	DRM_DEBUG_KMS("\n");

	spin_lock_irqsave(&res->reg_slock, flags);
	mixer_reg_writemask(res, MXR_STATUS, ~0, MXR_STATUS_SOFT_RESET);
	mixer_vsync_set_update(mctx, false);

	mixer_reg_writemask(res, MXR_CFG, MXR_CFG_DST_HDMI, MXR_CFG_DST_MASK);

	/* set output in RGB888 mode */
	mixer_reg_writemask(res, MXR_CFG, MXR_CFG_OUT_RGB888, MXR_CFG_OUT_MASK);

	/* 16 beat burst in DMA */
	mixer_reg_writemask(res, MXR_STATUS, MXR_STATUS_16_BURST,
		MXR_STATUS_BURST_MASK);

	/* setting default layer priority: layer1 > layer0 > video
	 * because typical usage scenario would be
	 * layer1 - OSD
	 * layer0 - framebuffer
	 * video - video overlay
	 */
	val = MXR_LAYER_CFG_GRP1_VAL(3);
	val |= MXR_LAYER_CFG_GRP0_VAL(2);
	val |= MXR_LAYER_CFG_VP_VAL(1);
	mixer_reg_write(res, MXR_LAYER_CFG, val);

	/* setting background color */
	mixer_reg_write(res, MXR_BG_COLOR0, 0x008080);
	mixer_reg_write(res, MXR_BG_COLOR1, 0x008080);
	mixer_reg_write(res, MXR_BG_COLOR2, 0x008080);

	/* setting graphical layers */

	val  = MXR_GRP_CFG_COLOR_KEY_DISABLE; /* no blank key */
	val |= MXR_GRP_CFG_WIN_BLEND_EN;
	val |= MXR_GRP_CFG_ALPHA_VAL(0xff); /* non-transparent alpha */

	/* the same configuration for both layers */
	mixer_reg_write(res, MXR_GRAPHIC_CFG(0), val);

	val |= MXR_GRP_CFG_BLEND_PRE_MUL;
	val |= MXR_GRP_CFG_PIXEL_BLEND_EN;
	mixer_reg_write(res, MXR_GRAPHIC_CFG(1), val);

	if (!(res->is_soc_exynos5)) {
		/* configuration of Video Processor for Exynos4 soc */
		vp_win_reset(mctx);
		vp_default_filter(res);
	}

	/* disable all layers */
	mixer_reg_writemask(res, MXR_CFG, 0, MXR_CFG_GRP0_ENABLE);
	mixer_reg_writemask(res, MXR_CFG, 0, MXR_CFG_GRP1_ENABLE);
	mixer_reg_writemask(res, MXR_CFG, 0, MXR_CFG_VP_ENABLE);

	mixer_vsync_set_update(mctx, true);
	spin_unlock_irqrestore(&res->reg_slock, flags);
}

static void mixer_resource_poweron(struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;

	DRM_DEBUG_KMS("is_mixer_powered_on: %d\n", mctx->is_mixer_powered_on);

	if (mctx->is_mixer_powered_on)
		return;

	clk_enable(res->mixer);
	if (!(res->is_soc_exynos5)) {
		clk_enable(res->vp);
		clk_enable(res->sclk_mixer);
	}

	mctx->is_mixer_powered_on = true;

	mixer_win_reset(mctx);
	mixer_enable_vblank(mctx, mctx->pipe);
	mixer_apply(mctx);
}

static void mixer_resource_poweroff(struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;

	DRM_DEBUG_KMS("is_mixer_powered_on: %d\n", mctx->is_mixer_powered_on);

	if (!mctx->is_mixer_powered_on)
		return;

	clk_disable(res->mixer);
	if (!(res->is_soc_exynos5)) {
		clk_disable(res->vp);
		clk_disable(res->sclk_mixer);
	}
	mixer_win_reset(mctx);
	mctx->is_mixer_powered_on = false;
}

static int mixer_dpms(void *ctx, int mode)
{
	struct mixer_context *mctx = ctx;

	DRM_DEBUG_KMS("[DPMS:%s]\n", drm_get_dpms_name(mode));

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		mixer_resource_poweron(mctx);
		break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		mixer_resource_poweroff(mctx);
		break;
	default:
		DRM_DEBUG_KMS("unknown dpms mode: %d\n", mode);
		break;
	}

	return 0;
}

static int mixer_subdrv_probe(void *ctx, struct drm_device *drm_dev)
{
	struct mixer_context *mctx = ctx;

	DRM_DEBUG("[DEV:%s]\n", drm_dev->devname);

	mctx->drm_dev = drm_dev;

	return 0;
}

static struct exynos_controller_ops mixer_ops = {
	/* manager */
	.subdrv_probe		= mixer_subdrv_probe,
	.enable_vblank		= mixer_enable_vblank,
	.disable_vblank		= mixer_disable_vblank,
	.dpms			= mixer_dpms,

	/* overlay */
	.mode_set		= mixer_win_mode_set,
	.win_commit		= mixer_win_commit,
	.win_disable		= mixer_win_disable,
};

#ifdef CONFIG_EXYNOS_IOMMU
static int iommu_init(struct platform_device *pdev)
{
	struct platform_device *pds;

	DRM_DEBUG("[PDEV:%s]\n", pdev->name);

	pds = find_sysmmu_dt(pdev, "sysmmu");
	if (pds == NULL) {
		printk(KERN_ERR "No sysmmu found  :\n");
		return -EINVAL;
	}

	platform_set_sysmmu(&pds->dev, &pdev->dev);
	/*
	 * The ordering in Makefile warrants that this is initialized after
	 * FIMD, so only just ensure that it works as expected and we are
	 * reusing the mapping originally created in exynos_drm_fimd.c.
	 */
	WARN_ON(!exynos_drm_common_mapping);
	exynos_drm_common_mapping = s5p_create_iommu_mapping(&pdev->dev,
					0, 0, 0, exynos_drm_common_mapping);
	if(exynos_drm_common_mapping == NULL) {
		printk(KERN_ERR"Failed to create iommu mapping for Mixer\n");
		return -EINVAL;
	}

	return 0;
}

static void iommu_deinit(struct platform_device *pdev)
{
	s5p_destroy_iommu_mapping(&pdev->dev);
	DRM_DEBUG("released the IOMMU mapping\n");
}
#endif

static int __devinit mixer_resources_init_exynos(
			struct mixer_context *mctx,
			struct platform_device *pdev,
			int is_exynos5)
{
	struct device *dev = &pdev->dev;
	struct mixer_resources *mixer_res = &mctx->mixer_res;
	struct resource *res;
	int ret;

	DRM_DEBUG("[PDEV:%s] is_exynos5: %d\n", pdev->name, is_exynos5);

	mixer_res->is_soc_exynos5 = is_exynos5;
	mixer_res->dev = dev;
	spin_lock_init(&mixer_res->reg_slock);

	if(is_exynos5)
		init_waitqueue_head(&mixer_res->event_queue);

	mixer_res->mixer = clk_get(dev, "mixer");
	if (IS_ERR_OR_NULL(mixer_res->mixer)) {
		dev_err(dev, "failed to get clock 'mixer'\n");
		ret = -ENODEV;
		goto fail;
	}
	if(!is_exynos5) {
		mixer_res->vp = clk_get(dev, "vp");
		if (IS_ERR_OR_NULL(mixer_res->vp)) {
			dev_err(dev, "failed to get clock 'vp'\n");
			ret = -ENODEV;
			goto fail;
		}
		mixer_res->sclk_mixer = clk_get(dev, "sclk_mixer");
		if (IS_ERR_OR_NULL(mixer_res->sclk_mixer)) {
			dev_err(dev, "failed to get clock 'sclk_mixer'\n");
			ret = -ENODEV;
			goto fail;
		}
	}
	mixer_res->sclk_hdmi = clk_get(dev, "sclk_hdmi");
	if (IS_ERR_OR_NULL(mixer_res->sclk_hdmi)) {
		dev_err(dev, "failed to get clock 'sclk_hdmi'\n");
		ret = -ENODEV;
		goto fail;
	}
	if(!is_exynos5) {
		mixer_res->sclk_dac = clk_get(dev, "sclk_dac");
		if (IS_ERR_OR_NULL(mixer_res->sclk_dac)) {
			dev_err(dev, "failed to get clock 'sclk_dac'\n");
			ret = -ENODEV;
			goto fail;
		}
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mxr");
	}
	else
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (res == NULL) {
		dev_err(dev, "get memory resource failed.\n");
		ret = -ENXIO;
		goto fail;
	}

	if(!is_exynos5)
		clk_set_parent(mixer_res->sclk_mixer, mixer_res->sclk_hdmi);

	mixer_res->mixer_regs = ioremap(res->start, resource_size(res));
	if (mixer_res->mixer_regs == NULL) {
		dev_err(dev, "register mapping failed.\n");
		ret = -ENXIO;
		goto fail;
	}

	if(!is_exynos5) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vp");
		if (res == NULL) {
			dev_err(dev, "get memory resource failed.\n");
			ret = -ENXIO;
			goto fail_vp_regs;
		}

		mixer_res->vp_regs = ioremap(res->start, resource_size(res));
		if (mixer_res->vp_regs == NULL) {
			dev_err(dev, "register mapping failed.\n");
			ret = -ENXIO;
			goto fail_vp_regs;
		}

		res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "irq");
		if (res == NULL) {
			dev_err(dev, "get interrupt resource failed.\n");
			ret = -ENXIO;
			goto fail_vp_regs;
		}
	}else {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
		if (res == NULL) {
			dev_err(dev, "get interrupt resource failed.\n");
			ret = -ENXIO;
			goto fail_mixer_regs;
		}
	}

	ret = request_irq(res->start, mixer_irq_handler, 0, "drm_mixer", mctx);
	if (ret) {
		dev_err(dev, "request interrupt failed.\n");
		goto fail_mixer_regs;
	}
	mixer_res->irq = res->start;

#ifdef CONFIG_EXYNOS_IOMMU
	ret = iommu_init(pdev);
	if(ret) {
		dev_err(dev, "iommu init failed.\n");
		goto fail_mixer_regs;
	}
#endif
	return 0;

fail_vp_regs:
	iounmap(mixer_res->vp_regs);

fail_mixer_regs:
	iounmap(mixer_res->mixer_regs);

fail:
	if (!IS_ERR_OR_NULL(mixer_res->sclk_dac))
		clk_put(mixer_res->sclk_dac);
	if (!IS_ERR_OR_NULL(mixer_res->sclk_hdmi))
		clk_put(mixer_res->sclk_hdmi);
	if (!IS_ERR_OR_NULL(mixer_res->sclk_mixer))
		clk_put(mixer_res->sclk_mixer);
	if (!IS_ERR_OR_NULL(mixer_res->vp))
		clk_put(mixer_res->vp);
	if (!IS_ERR_OR_NULL(mixer_res->mixer))
		clk_put(mixer_res->mixer);
	mixer_res->dev = NULL;
	return ret;
}

static void mixer_resources_cleanup(struct device *dev,
		struct mixer_context *mctx)
{
	struct mixer_resources *res = &mctx->mixer_res;

	DRM_DEBUG("\n");

	disable_irq(res->irq);
	free_irq(res->irq, dev);

	iounmap(res->vp_regs);
	iounmap(res->mixer_regs);
}

static int __devinit mixer_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_drm_hdmi_pdata *pdata;
	struct mixer_context *mctx;
	int ret;

	DRM_DEBUG("[PDEV:%s]\n", pdev->name);

	dev_info(dev, "probe start\n");

	mctx = kzalloc(sizeof(*mctx), GFP_KERNEL);
	if (!mctx) {
		DRM_ERROR("failed to alloc mixer context.\n");
		return -ENOMEM;
	}

	mctx->dev = &pdev->dev;
	mctx->pipe = -1;

	platform_set_drvdata(pdev, mctx);

	/* Get from Platform soc deatils */
	pdata = pdev->dev.platform_data;

	/* acquire resources: regs, irqs, clocks */
	ret = mixer_resources_init_exynos(mctx, pdev, pdata->is_soc_exynos5);
	if (ret)
		goto fail;

	mctx->is_mixer_powered_on = false;
	pm_runtime_enable(dev);

	exynos_display_attach_controller(EXYNOS_DRM_DISPLAY_TYPE_MIXER,
			&mixer_ops, mctx);

	return 0;


fail:
	dev_info(dev, "probe failed\n");
	return ret;
}

static int mixer_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mixer_context *mctx = platform_get_drvdata(pdev);

	DRM_DEBUG("[PDEV:%s]\n", pdev->name);

	dev_info(dev, "remove successful\n");

	mixer_resource_poweroff(mctx);
	mixer_resources_cleanup(dev, mctx);

#ifdef CONFIG_EXYNOS_IOMMU
	iommu_deinit(pdev);
#endif

	kfree(mctx);

	return 0;
}

struct platform_driver mixer_driver = {
	.driver = {
		.name = "s5p-mixer",
		.owner = THIS_MODULE,
	},
	.probe = mixer_probe,
	.remove = __devexit_p(mixer_remove),
};
