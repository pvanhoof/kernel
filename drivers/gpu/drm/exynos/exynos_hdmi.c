/*
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Authors:
 * Seung-Woo Kim <sw0312.kim@samsung.com>
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * Based on drivers/media/video/s5p-tv/hdmi_drv.c
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include "drmP.h"
#include "drm_edid.h"
#include "drm_crtc_helper.h"
#include "drm_crtc.h"

#include "regs-hdmi.h"

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_i2c.h>
#include <linux/of_gpio.h>
#include <plat/gpio-cfg.h>

#include <drm/exynos_drm.h>

#include "exynos_drm_drv.h"
#include "exynos_drm_display.h"

#include "exynos_hdmi.h"

#define get_hdmi_context(dev)	platform_get_drvdata(to_platform_device(dev))

struct hdmi_resources {
	struct clk			*hdmi;
	struct clk			*sclk_hdmi;
	struct clk			*sclk_pixel;
	struct clk			*sclk_hdmiphy;
	struct clk			*hdmiphy;
	struct regulator_bulk_data	*regul_bulk;
	int				regul_count;
};

struct hdmi_tg_regs {
	u8 cmd[1];
	u8 h_fsz[2];
	u8 hact_st[2];
	u8 hact_sz[2];
	u8 v_fsz[2];
	u8 vsync[2];
	u8 vsync2[2];
	u8 vact_st[2];
	u8 vact_sz[2];
	u8 field_chg[2];
	u8 vact_st2[2];
	u8 vact_st3[2];
	u8 vact_st4[2];
	u8 vsync_top_hdmi[2];
	u8 vsync_bot_hdmi[2];
	u8 field_top_hdmi[2];
	u8 field_bot_hdmi[2];
	u8 tg_3d[1];
};

struct hdmi_core_regs {
	u8 h_blank[2];
	u8 v2_blank[2];
	u8 v1_blank[2];
	u8 v_line[2];
	u8 h_line[2];
	u8 hsync_pol[1];
	u8 vsync_pol[1];
	u8 int_pro_mode[1];
	u8 v_blank_f0[2];
	u8 v_blank_f1[2];
	u8 h_sync_start[2];
	u8 h_sync_end[2];
	u8 v_sync_line_bef_2[2];
	u8 v_sync_line_bef_1[2];
	u8 v_sync_line_aft_2[2];
	u8 v_sync_line_aft_1[2];
	u8 v_sync_line_aft_pxl_2[2];
	u8 v_sync_line_aft_pxl_1[2];
	u8 v_blank_f2[2]; /* for 3D mode */
	u8 v_blank_f3[2]; /* for 3D mode */
	u8 v_blank_f4[2]; /* for 3D mode */
	u8 v_blank_f5[2]; /* for 3D mode */
	u8 v_sync_line_aft_3[2];
	u8 v_sync_line_aft_4[2];
	u8 v_sync_line_aft_5[2];
	u8 v_sync_line_aft_6[2];
	u8 v_sync_line_aft_pxl_3[2];
	u8 v_sync_line_aft_pxl_4[2];
	u8 v_sync_line_aft_pxl_5[2];
	u8 v_sync_line_aft_pxl_6[2];
	u8 vact_space_1[2];
	u8 vact_space_2[2];
	u8 vact_space_3[2];
	u8 vact_space_4[2];
	u8 vact_space_5[2];
	u8 vact_space_6[2];
};

struct hdmi_mode_conf {
	int pixel_clock;
	struct hdmi_core_regs core;
	struct hdmi_tg_regs tg;
	u8 vic;
};

struct hdmi_context {
	struct device			*dev;
	struct drm_device		*drm_dev;
	struct fb_videomode		*default_timing;
	struct hdmi_mode_conf		mode_conf;
	unsigned int			is_v13:1;
	unsigned int			default_win;
	unsigned int			default_bpp;
	bool				hpd_handle;
	bool				enabled;
	bool				has_hdmi_sink;
	bool				has_hdmi_audio;
	bool				is_soc_exynos5;
	bool				is_hdmi_powered_on;
	bool				video_enabled;

	struct resource			*regs_res;
	void __iomem			*regs;
	unsigned int			external_irq;
	unsigned int			internal_irq;
	unsigned int			curr_irq;
	struct workqueue_struct		*wq;
	struct work_struct		hotplug_work;

	struct i2c_client		*ddc_port;
	struct i2c_client		*hdmiphy_port;
	int				hpd_gpio;
	/* current hdmiphy conf index */
	int cur_conf;

	struct hdmi_resources		res;
};

/* HDMI Version 1.3 */
static const u8 hdmiphy_v13_conf27[32] = {
	0x01, 0x05, 0x00, 0xD8, 0x10, 0x1C, 0x30, 0x40,
	0x6B, 0x10, 0x02, 0x51, 0xDF, 0xF2, 0x54, 0x87,
	0x84, 0x00, 0x30, 0x38, 0x00, 0x08, 0x10, 0xE0,
	0x22, 0x40, 0xE3, 0x26, 0x00, 0x00, 0x00, 0x00,
};

static const u8 hdmiphy_v13_conf27_027[32] = {
	0x01, 0x05, 0x00, 0xD4, 0x10, 0x9C, 0x09, 0x64,
	0x6B, 0x10, 0x02, 0x51, 0xDF, 0xF2, 0x54, 0x87,
	0x84, 0x00, 0x30, 0x38, 0x00, 0x08, 0x10, 0xE0,
	0x22, 0x40, 0xE3, 0x26, 0x00, 0x00, 0x00, 0x00,
};

static const u8 hdmiphy_v13_conf74_175[32] = {
	0x01, 0x05, 0x00, 0xD8, 0x10, 0x9C, 0xef, 0x5B,
	0x6D, 0x10, 0x01, 0x51, 0xef, 0xF3, 0x54, 0xb9,
	0x84, 0x00, 0x30, 0x38, 0x00, 0x08, 0x10, 0xE0,
	0x22, 0x40, 0xa5, 0x26, 0x01, 0x00, 0x00, 0x00,
};

static const u8 hdmiphy_v13_conf74_25[32] = {
	0x01, 0x05, 0x00, 0xd8, 0x10, 0x9c, 0xf8, 0x40,
	0x6a, 0x10, 0x01, 0x51, 0xff, 0xf1, 0x54, 0xba,
	0x84, 0x00, 0x10, 0x38, 0x00, 0x08, 0x10, 0xe0,
	0x22, 0x40, 0xa4, 0x26, 0x01, 0x00, 0x00, 0x00,
};

static const u8 hdmiphy_v13_conf148_5[32] = {
	0x01, 0x05, 0x00, 0xD8, 0x10, 0x9C, 0xf8, 0x40,
	0x6A, 0x18, 0x00, 0x51, 0xff, 0xF1, 0x54, 0xba,
	0x84, 0x00, 0x10, 0x38, 0x00, 0x08, 0x10, 0xE0,
	0x22, 0x40, 0xa4, 0x26, 0x02, 0x00, 0x00, 0x00,
};

struct hdmi_v13_tg_regs {
	u8 cmd;
	u8 h_fsz_l;
	u8 h_fsz_h;
	u8 hact_st_l;
	u8 hact_st_h;
	u8 hact_sz_l;
	u8 hact_sz_h;
	u8 v_fsz_l;
	u8 v_fsz_h;
	u8 vsync_l;
	u8 vsync_h;
	u8 vsync2_l;
	u8 vsync2_h;
	u8 vact_st_l;
	u8 vact_st_h;
	u8 vact_sz_l;
	u8 vact_sz_h;
	u8 field_chg_l;
	u8 field_chg_h;
	u8 vact_st2_l;
	u8 vact_st2_h;
	u8 vsync_top_hdmi_l;
	u8 vsync_top_hdmi_h;
	u8 vsync_bot_hdmi_l;
	u8 vsync_bot_hdmi_h;
	u8 field_top_hdmi_l;
	u8 field_top_hdmi_h;
	u8 field_bot_hdmi_l;
	u8 field_bot_hdmi_h;
};

struct hdmi_v13_core_regs {
	u8 h_blank[2];
	u8 v_blank[3];
	u8 h_v_line[3];
	u8 vsync_pol[1];
	u8 int_pro_mode[1];
	u8 v_blank_f[3];
	u8 h_sync_gen[3];
	u8 v_sync_gen1[3];
	u8 v_sync_gen2[3];
	u8 v_sync_gen3[3];
};

struct hdmi_v13_preset_conf {
	struct hdmi_v13_core_regs core;
	struct hdmi_v13_tg_regs tg;
};

struct hdmi_v13_conf {
	int width;
	int height;
	int vrefresh;
	bool interlace;
	const u8 *hdmiphy_data;
	const struct hdmi_v13_preset_conf *conf;
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_480p = {
	.core = {
		.h_blank = {0x8a, 0x00},
		.v_blank = {0x0d, 0x6a, 0x01},
		.h_v_line = {0x0d, 0xa2, 0x35},
		.vsync_pol = {0x01},
		.int_pro_mode = {0x00},
		.v_blank_f = {0x00, 0x00, 0x00},
		.h_sync_gen = {0x0e, 0x30, 0x11},
		.v_sync_gen1 = {0x0f, 0x90, 0x00},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x5a, 0x03, /* h_fsz */
		0x8a, 0x00, 0xd0, 0x02, /* hact */
		0x0d, 0x02, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x2d, 0x00, 0xe0, 0x01, /* vact */
		0x33, 0x02, /* field_chg */
		0x49, 0x02, /* vact_st2 */
		0x01, 0x00, 0x33, 0x02, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_720p60 = {
	.core = {
		.h_blank = {0x72, 0x01},
		.v_blank = {0xee, 0xf2, 0x00},
		.h_v_line = {0xee, 0x22, 0x67},
		.vsync_pol = {0x00},
		.int_pro_mode = {0x00},
		.v_blank_f = {0x00, 0x00, 0x00}, /* don't care */
		.h_sync_gen = {0x6c, 0x50, 0x02},
		.v_sync_gen1 = {0x0a, 0x50, 0x00},
		.v_sync_gen2 = {0x01, 0x10, 0x00},
		.v_sync_gen3 = {0x01, 0x10, 0x00},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x72, 0x06, /* h_fsz */
		0x71, 0x01, 0x01, 0x05, /* hact */
		0xee, 0x02, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x1e, 0x00, 0xd0, 0x02, /* vact */
		0x33, 0x02, /* field_chg */
		0x49, 0x02, /* vact_st2 */
		0x01, 0x00, 0x01, 0x00, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_1080i50 = {
	.core = {
		.h_blank = {0xd0, 0x02},
		.v_blank = {0x32, 0xB2, 0x00},
		.h_v_line = {0x65, 0x04, 0xa5},
		.vsync_pol = {0x00},
		.int_pro_mode = {0x01},
		.v_blank_f = {0x49, 0x2A, 0x23},
		.h_sync_gen = {0x0E, 0xEA, 0x08},
		.v_sync_gen1 = {0x07, 0x20, 0x00},
		.v_sync_gen2 = {0x39, 0x42, 0x23},
		.v_sync_gen3 = {0x38, 0x87, 0x73},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x50, 0x0A, /* h_fsz */
		0xCF, 0x02, 0x81, 0x07, /* hact */
		0x65, 0x04, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x16, 0x00, 0x1c, 0x02, /* vact */
		0x33, 0x02, /* field_chg */
		0x49, 0x02, /* vact_st2 */
		0x01, 0x00, 0x33, 0x02, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_1080p50 = {
	.core = {
		.h_blank = {0xd0, 0x02},
		.v_blank = {0x65, 0x6c, 0x01},
		.h_v_line = {0x65, 0x04, 0xa5},
		.vsync_pol = {0x00},
		.int_pro_mode = {0x00},
		.v_blank_f = {0x00, 0x00, 0x00}, /* don't care */
		.h_sync_gen = {0x0e, 0xea, 0x08},
		.v_sync_gen1 = {0x09, 0x40, 0x00},
		.v_sync_gen2 = {0x01, 0x10, 0x00},
		.v_sync_gen3 = {0x01, 0x10, 0x00},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x50, 0x0A, /* h_fsz */
		0xCF, 0x02, 0x81, 0x07, /* hact */
		0x65, 0x04, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x2d, 0x00, 0x38, 0x04, /* vact */
		0x33, 0x02, /* field_chg */
		0x48, 0x02, /* vact_st2 */
		0x01, 0x00, 0x01, 0x00, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_1080i60 = {
	.core = {
		.h_blank = {0x18, 0x01},
		.v_blank = {0x32, 0xB2, 0x00},
		.h_v_line = {0x65, 0x84, 0x89},
		.vsync_pol = {0x00},
		.int_pro_mode = {0x01},
		.v_blank_f = {0x49, 0x2A, 0x23},
		.h_sync_gen = {0x56, 0x08, 0x02},
		.v_sync_gen1 = {0x07, 0x20, 0x00},
		.v_sync_gen2 = {0x39, 0x42, 0x23},
		.v_sync_gen3 = {0xa4, 0x44, 0x4a},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x98, 0x08, /* h_fsz */
		0x17, 0x01, 0x81, 0x07, /* hact */
		0x65, 0x04, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x16, 0x00, 0x1c, 0x02, /* vact */
		0x33, 0x02, /* field_chg */
		0x49, 0x02, /* vact_st2 */
		0x01, 0x00, 0x33, 0x02, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_preset_conf hdmi_v13_conf_1080p60 = {
	.core = {
		.h_blank = {0x18, 0x01},
		.v_blank = {0x65, 0x6c, 0x01},
		.h_v_line = {0x65, 0x84, 0x89},
		.vsync_pol = {0x00},
		.int_pro_mode = {0x00},
		.v_blank_f = {0x00, 0x00, 0x00}, /* don't care */
		.h_sync_gen = {0x56, 0x08, 0x02},
		.v_sync_gen1 = {0x09, 0x40, 0x00},
		.v_sync_gen2 = {0x01, 0x10, 0x00},
		.v_sync_gen3 = {0x01, 0x10, 0x00},
		/* other don't care */
	},
	.tg = {
		0x00, /* cmd */
		0x98, 0x08, /* h_fsz */
		0x17, 0x01, 0x81, 0x07, /* hact */
		0x65, 0x04, /* v_fsz */
		0x01, 0x00, 0x33, 0x02, /* vsync */
		0x2d, 0x00, 0x38, 0x04, /* vact */
		0x33, 0x02, /* field_chg */
		0x48, 0x02, /* vact_st2 */
		0x01, 0x00, 0x01, 0x00, /* vsync top/bot */
		0x01, 0x00, 0x33, 0x02, /* field top/bot */
	},
};

static const struct hdmi_v13_conf hdmi_v13_confs[] = {
	{ 1280, 720, 60, false, hdmiphy_v13_conf74_25, &hdmi_v13_conf_720p60 },
	{ 1280, 720, 50, false, hdmiphy_v13_conf74_25, &hdmi_v13_conf_720p60 },
	{ 720, 480, 60, false, hdmiphy_v13_conf27_027, &hdmi_v13_conf_480p },
	{ 1920, 1080, 50, true, hdmiphy_v13_conf74_25, &hdmi_v13_conf_1080i50 },
	{ 1920, 1080, 50, false, hdmiphy_v13_conf148_5,
				 &hdmi_v13_conf_1080p50 },
	{ 1920, 1080, 60, true, hdmiphy_v13_conf74_25, &hdmi_v13_conf_1080i60 },
	{ 1920, 1080, 60, false, hdmiphy_v13_conf148_5,
				 &hdmi_v13_conf_1080p60 },
};

struct hdmiphy_config {
	int pixel_clock;
	u8 conf[32];
};

static const struct hdmiphy_config phy_configs[] = {
	{
		.pixel_clock = 25200000,
		.conf = {
			0x01, 0x51, 0x2A, 0x75, 0x40, 0x01, 0x00, 0x08,
			0x82, 0x80, 0xfc, 0xd8, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xf4, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 27000000,
		.conf = {
			0x01, 0xd1, 0x22, 0x51, 0x40, 0x08, 0xfc, 0x20,
			0x98, 0xa0, 0xcb, 0xd8, 0x45, 0xa0, 0xac, 0x80,
			0x06, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xe4, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 27027000,
		.conf = {
			0x01, 0xd1, 0x2d, 0x72, 0x40, 0x64, 0x12, 0x08,
			0x43, 0xa0, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xe3, 0x24, 0x00, 0x00, 0x00, 0x01, 0x00,
		},
	},
	{
		.pixel_clock = 36000000,
		.conf = {
			0x01, 0x51, 0x2d, 0x55, 0x40, 0x01, 0x00, 0x08,
			0x82, 0x80, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xab, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 40000000,
		.conf = {
			0x01, 0x51, 0x32, 0x55, 0x40, 0x01, 0x00, 0x08,
			0x82, 0x80, 0x2c, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0x9a, 0x24, 0x00, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 65000000,
		.conf = {
			0x01, 0xd1, 0x36, 0x34, 0x40, 0x1e, 0x0a, 0x08,
			0x82, 0xa0, 0x45, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xbd, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 74176000,
		.conf = {
			0x01, 0xd1, 0x3e, 0x35, 0x40, 0x5b, 0xde, 0x08,
			0x82, 0xa0, 0x73, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x56, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xa6, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 74250000,
		.conf = {
			0x01, 0xd1, 0x1f, 0x10, 0x40, 0x40, 0xf8, 0x08,
			0x81, 0xa0, 0xba, 0xd8, 0x45, 0xa0, 0xac, 0x80,
			0x3c, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xa5, 0x24, 0x01, 0x00, 0x00, 0x01, 0x00,
		},
	},
	{
		.pixel_clock = 83500000,
		.conf = {
			0x01, 0xd1, 0x23, 0x11, 0x40, 0x0c, 0xfb, 0x08,
			0x85, 0xa0, 0xd1, 0xd8, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0x93, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 106500000,
		.conf = {
			0x01, 0xd1, 0x2c, 0x12, 0x40, 0x0c, 0x09, 0x08,
			0x84, 0xa0, 0x0a, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0x73, 0x24, 0x01, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 108000000,
		.conf = {
			0x01, 0x51, 0x2d, 0x15, 0x40, 0x01, 0x00, 0x08,
			0x82, 0x80, 0x0e, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0xc7, 0x25, 0x03, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 146250000,
		.conf = {
			0x01, 0xd1, 0x3d, 0x15, 0x40, 0x18, 0xfd, 0x08,
			0x83, 0xa0, 0x6e, 0xd9, 0x45, 0xa0, 0xac, 0x80,
			0x08, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0x50, 0x25, 0x03, 0x00, 0x00, 0x01, 0x80,
		},
	},
	{
		.pixel_clock = 148500000,
		.conf = {
			0x01, 0xd1, 0x1f, 0x00, 0x40, 0x40, 0xf8, 0x08,
			0x81, 0xa0, 0xba, 0xd8, 0x45, 0xa0, 0xac, 0x80,
			0x3c, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x86,
			0x54, 0x4b, 0x25, 0x03, 0x00, 0x00, 0x01, 0x00,
		},
	},
};

struct hdmi_infoframe {
	enum HDMI_PACKET_TYPE type;
	u8 ver;
	u8 len;
};

static inline u32 hdmi_reg_read(struct hdmi_context *hdata, u32 reg_id)
{
	return readl(hdata->regs + reg_id);
}

static inline void hdmi_reg_writeb(struct hdmi_context *hdata,
				 u32 reg_id, u8 value)
{
	writeb(value, hdata->regs + reg_id);
}

static inline void hdmi_reg_writemask(struct hdmi_context *hdata,
				 u32 reg_id, u32 value, u32 mask)
{
	u32 old = readl(hdata->regs + reg_id);
	value = (value & mask) | (old & ~mask);
	writel(value, hdata->regs + reg_id);
}

static void hdmi_cfg_hpd(struct hdmi_context *hdata, bool external)
{
	if (external) {
		s3c_gpio_cfgpin(hdata->hpd_gpio, S3C_GPIO_SFN(0xf));
		s3c_gpio_setpull(hdata->hpd_gpio, S3C_GPIO_PULL_DOWN);
	} else {
		s3c_gpio_cfgpin(hdata->hpd_gpio, S3C_GPIO_SFN(3));
		s3c_gpio_setpull(hdata->hpd_gpio, S3C_GPIO_PULL_NONE);
	}
}

static int hdmi_get_hpd(struct hdmi_context *hdata)
{
	return gpio_get_value(hdata->hpd_gpio);
}

static void hdmi_v13_regs_dump(struct hdmi_context *hdata, char *prefix)
{
#define DUMPREG(reg_id) \
	DRM_DEBUG_KMS("%s:" #reg_id " = %08x\n", prefix, \
	readl(hdata->regs + reg_id))
	DRM_DEBUG_KMS("%s: ---- CONTROL REGISTERS ----\n", prefix);
	DUMPREG(HDMI_INTC_FLAG);
	DUMPREG(HDMI_INTC_CON);
	DUMPREG(HDMI_HPD_STATUS);
	DUMPREG(HDMI_V13_PHY_RSTOUT);
	DUMPREG(HDMI_V13_PHY_VPLL);
	DUMPREG(HDMI_V13_PHY_CMU);
	DUMPREG(HDMI_V13_CORE_RSTOUT);

	DRM_DEBUG_KMS("%s: ---- CORE REGISTERS ----\n", prefix);
	DUMPREG(HDMI_CON_0);
	DUMPREG(HDMI_CON_1);
	DUMPREG(HDMI_CON_2);
	DUMPREG(HDMI_SYS_STATUS);
	DUMPREG(HDMI_V13_PHY_STATUS);
	DUMPREG(HDMI_STATUS_EN);
	DUMPREG(HDMI_HPD);
	DUMPREG(HDMI_MODE_SEL);
	DUMPREG(HDMI_V13_HPD_GEN);
	DUMPREG(HDMI_V13_DC_CONTROL);
	DUMPREG(HDMI_V13_VIDEO_PATTERN_GEN);

	DRM_DEBUG_KMS("%s: ---- CORE SYNC REGISTERS ----\n", prefix);
	DUMPREG(HDMI_H_BLANK_0);
	DUMPREG(HDMI_H_BLANK_1);
	DUMPREG(HDMI_V13_V_BLANK_0);
	DUMPREG(HDMI_V13_V_BLANK_1);
	DUMPREG(HDMI_V13_V_BLANK_2);
	DUMPREG(HDMI_V13_H_V_LINE_0);
	DUMPREG(HDMI_V13_H_V_LINE_1);
	DUMPREG(HDMI_V13_H_V_LINE_2);
	DUMPREG(HDMI_VSYNC_POL);
	DUMPREG(HDMI_INT_PRO_MODE);
	DUMPREG(HDMI_V13_V_BLANK_F_0);
	DUMPREG(HDMI_V13_V_BLANK_F_1);
	DUMPREG(HDMI_V13_V_BLANK_F_2);
	DUMPREG(HDMI_V13_H_SYNC_GEN_0);
	DUMPREG(HDMI_V13_H_SYNC_GEN_1);
	DUMPREG(HDMI_V13_H_SYNC_GEN_2);
	DUMPREG(HDMI_V13_V_SYNC_GEN_1_0);
	DUMPREG(HDMI_V13_V_SYNC_GEN_1_1);
	DUMPREG(HDMI_V13_V_SYNC_GEN_1_2);
	DUMPREG(HDMI_V13_V_SYNC_GEN_2_0);
	DUMPREG(HDMI_V13_V_SYNC_GEN_2_1);
	DUMPREG(HDMI_V13_V_SYNC_GEN_2_2);
	DUMPREG(HDMI_V13_V_SYNC_GEN_3_0);
	DUMPREG(HDMI_V13_V_SYNC_GEN_3_1);
	DUMPREG(HDMI_V13_V_SYNC_GEN_3_2);

	DRM_DEBUG_KMS("%s: ---- TG REGISTERS ----\n", prefix);
	DUMPREG(HDMI_TG_CMD);
	DUMPREG(HDMI_TG_H_FSZ_L);
	DUMPREG(HDMI_TG_H_FSZ_H);
	DUMPREG(HDMI_TG_HACT_ST_L);
	DUMPREG(HDMI_TG_HACT_ST_H);
	DUMPREG(HDMI_TG_HACT_SZ_L);
	DUMPREG(HDMI_TG_HACT_SZ_H);
	DUMPREG(HDMI_TG_V_FSZ_L);
	DUMPREG(HDMI_TG_V_FSZ_H);
	DUMPREG(HDMI_TG_VSYNC_L);
	DUMPREG(HDMI_TG_VSYNC_H);
	DUMPREG(HDMI_TG_VSYNC2_L);
	DUMPREG(HDMI_TG_VSYNC2_H);
	DUMPREG(HDMI_TG_VACT_ST_L);
	DUMPREG(HDMI_TG_VACT_ST_H);
	DUMPREG(HDMI_TG_VACT_SZ_L);
	DUMPREG(HDMI_TG_VACT_SZ_H);
	DUMPREG(HDMI_TG_FIELD_CHG_L);
	DUMPREG(HDMI_TG_FIELD_CHG_H);
	DUMPREG(HDMI_TG_VACT_ST2_L);
	DUMPREG(HDMI_TG_VACT_ST2_H);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_H);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_H);
#undef DUMPREG
}

static void hdmi_v14_regs_dump(struct hdmi_context *hdata, char *prefix)
{
	int i;

#define DUMPREG(reg_id) \
	DRM_DEBUG_KMS("%s:" #reg_id " = %08x\n", prefix, \
	readl(hdata->regs + reg_id))

	DRM_DEBUG_KMS("%s: ---- CONTROL REGISTERS ----\n", prefix);
	DUMPREG(HDMI_INTC_CON);
	DUMPREG(HDMI_INTC_FLAG);
	DUMPREG(HDMI_HPD_STATUS);
	DUMPREG(HDMI_INTC_CON_1);
	DUMPREG(HDMI_INTC_FLAG_1);
	DUMPREG(HDMI_PHY_STATUS_0);
	DUMPREG(HDMI_PHY_STATUS_PLL);
	DUMPREG(HDMI_PHY_CON_0);
	DUMPREG(HDMI_PHY_RSTOUT);
	DUMPREG(HDMI_PHY_VPLL);
	DUMPREG(HDMI_PHY_CMU);
	DUMPREG(HDMI_CORE_RSTOUT);

	DRM_DEBUG_KMS("%s: ---- CORE REGISTERS ----\n", prefix);
	DUMPREG(HDMI_CON_0);
	DUMPREG(HDMI_CON_1);
	DUMPREG(HDMI_CON_2);
	DUMPREG(HDMI_SYS_STATUS);
	DUMPREG(HDMI_PHY_STATUS_0);
	DUMPREG(HDMI_STATUS_EN);
	DUMPREG(HDMI_HPD);
	DUMPREG(HDMI_MODE_SEL);
	DUMPREG(HDMI_ENC_EN);
	DUMPREG(HDMI_DC_CONTROL);
	DUMPREG(HDMI_VIDEO_PATTERN_GEN);

	DRM_DEBUG_KMS("%s: ---- CORE SYNC REGISTERS ----\n", prefix);
	DUMPREG(HDMI_H_BLANK_0);
	DUMPREG(HDMI_H_BLANK_1);
	DUMPREG(HDMI_V2_BLANK_0);
	DUMPREG(HDMI_V2_BLANK_1);
	DUMPREG(HDMI_V1_BLANK_0);
	DUMPREG(HDMI_V1_BLANK_1);
	DUMPREG(HDMI_V_LINE_0);
	DUMPREG(HDMI_V_LINE_1);
	DUMPREG(HDMI_H_LINE_0);
	DUMPREG(HDMI_H_LINE_1);
	DUMPREG(HDMI_HSYNC_POL);

	DUMPREG(HDMI_VSYNC_POL);
	DUMPREG(HDMI_INT_PRO_MODE);
	DUMPREG(HDMI_V_BLANK_F0_0);
	DUMPREG(HDMI_V_BLANK_F0_1);
	DUMPREG(HDMI_V_BLANK_F1_0);
	DUMPREG(HDMI_V_BLANK_F1_1);

	DUMPREG(HDMI_H_SYNC_START_0);
	DUMPREG(HDMI_H_SYNC_START_1);
	DUMPREG(HDMI_H_SYNC_END_0);
	DUMPREG(HDMI_H_SYNC_END_1);

	DUMPREG(HDMI_V_SYNC_LINE_BEF_2_0);
	DUMPREG(HDMI_V_SYNC_LINE_BEF_2_1);
	DUMPREG(HDMI_V_SYNC_LINE_BEF_1_0);
	DUMPREG(HDMI_V_SYNC_LINE_BEF_1_1);

	DUMPREG(HDMI_V_SYNC_LINE_AFT_2_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_2_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_1_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_1_1);

	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_2_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_2_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_1_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_1_1);

	DUMPREG(HDMI_V_BLANK_F2_0);
	DUMPREG(HDMI_V_BLANK_F2_1);
	DUMPREG(HDMI_V_BLANK_F3_0);
	DUMPREG(HDMI_V_BLANK_F3_1);
	DUMPREG(HDMI_V_BLANK_F4_0);
	DUMPREG(HDMI_V_BLANK_F4_1);
	DUMPREG(HDMI_V_BLANK_F5_0);
	DUMPREG(HDMI_V_BLANK_F5_1);

	DUMPREG(HDMI_V_SYNC_LINE_AFT_3_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_3_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_4_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_4_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_5_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_5_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_6_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_6_1);

	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_3_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_3_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_4_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_4_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_5_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_5_1);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_6_0);
	DUMPREG(HDMI_V_SYNC_LINE_AFT_PXL_6_1);

	DUMPREG(HDMI_VACT_SPACE_1_0);
	DUMPREG(HDMI_VACT_SPACE_1_1);
	DUMPREG(HDMI_VACT_SPACE_2_0);
	DUMPREG(HDMI_VACT_SPACE_2_1);
	DUMPREG(HDMI_VACT_SPACE_3_0);
	DUMPREG(HDMI_VACT_SPACE_3_1);
	DUMPREG(HDMI_VACT_SPACE_4_0);
	DUMPREG(HDMI_VACT_SPACE_4_1);
	DUMPREG(HDMI_VACT_SPACE_5_0);
	DUMPREG(HDMI_VACT_SPACE_5_1);
	DUMPREG(HDMI_VACT_SPACE_6_0);
	DUMPREG(HDMI_VACT_SPACE_6_1);

	DRM_DEBUG_KMS("%s: ---- TG REGISTERS ----\n", prefix);
	DUMPREG(HDMI_TG_CMD);
	DUMPREG(HDMI_TG_H_FSZ_L);
	DUMPREG(HDMI_TG_H_FSZ_H);
	DUMPREG(HDMI_TG_HACT_ST_L);
	DUMPREG(HDMI_TG_HACT_ST_H);
	DUMPREG(HDMI_TG_HACT_SZ_L);
	DUMPREG(HDMI_TG_HACT_SZ_H);
	DUMPREG(HDMI_TG_V_FSZ_L);
	DUMPREG(HDMI_TG_V_FSZ_H);
	DUMPREG(HDMI_TG_VSYNC_L);
	DUMPREG(HDMI_TG_VSYNC_H);
	DUMPREG(HDMI_TG_VSYNC2_L);
	DUMPREG(HDMI_TG_VSYNC2_H);
	DUMPREG(HDMI_TG_VACT_ST_L);
	DUMPREG(HDMI_TG_VACT_ST_H);
	DUMPREG(HDMI_TG_VACT_SZ_L);
	DUMPREG(HDMI_TG_VACT_SZ_H);
	DUMPREG(HDMI_TG_FIELD_CHG_L);
	DUMPREG(HDMI_TG_FIELD_CHG_H);
	DUMPREG(HDMI_TG_VACT_ST2_L);
	DUMPREG(HDMI_TG_VACT_ST2_H);
	DUMPREG(HDMI_TG_VACT_ST3_L);
	DUMPREG(HDMI_TG_VACT_ST3_H);
	DUMPREG(HDMI_TG_VACT_ST4_L);
	DUMPREG(HDMI_TG_VACT_ST4_H);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_H);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_H);
	DUMPREG(HDMI_TG_3D);

	DRM_DEBUG_KMS("%s: ---- PACKET REGISTERS ----\n", prefix);
	DUMPREG(HDMI_AVI_CON);
	DUMPREG(HDMI_AVI_HEADER0);
	DUMPREG(HDMI_AVI_HEADER1);
	DUMPREG(HDMI_AVI_HEADER2);
	DUMPREG(HDMI_AVI_CHECK_SUM);
	DUMPREG(HDMI_VSI_CON);
	DUMPREG(HDMI_VSI_HEADER0);
	DUMPREG(HDMI_VSI_HEADER1);
	DUMPREG(HDMI_VSI_HEADER2);
	for (i = 0; i < 7; ++i)
		DUMPREG(HDMI_VSI_DATA(i));

#undef DUMPREG
}

static void hdmi_regs_dump(struct hdmi_context *hdata, char *prefix)
{
	if (hdata->is_v13)
		hdmi_v13_regs_dump(hdata, prefix);
	else
		hdmi_v14_regs_dump(hdata, prefix);
}

static int hdmi_v13_conf_index(struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_v13_confs); ++i)
		if (hdmi_v13_confs[i].width == mode->hdisplay &&
				hdmi_v13_confs[i].height == mode->vdisplay &&
				hdmi_v13_confs[i].vrefresh == mode->vrefresh &&
				hdmi_v13_confs[i].interlace ==
				((mode->flags & DRM_MODE_FLAG_INTERLACE) ?
				 true : false))
			return i;

	return -EINVAL;
}

static bool hdmi_is_connected(void *ctx)
{
	struct hdmi_context *hdata = ctx;
	if (hdata->is_hdmi_powered_on) {
		if (!hdmi_reg_read(hdata, HDMI_HPD_STATUS)) {
			DRM_DEBUG_KMS("hdmi is not connected\n");
			return false;
		}
	} else if (!hdmi_get_hpd(hdata)) {
			DRM_DEBUG_KMS("hdmi is not connected\n");
			return false;
	}

	return true;
}

static struct edid *hdmi_get_edid(void *ctx, struct drm_connector *connector)
{
	struct hdmi_context *hdata = ctx;
	struct edid *edid;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s]\n", DRM_BASE_ID(connector),
			drm_get_connector_name(connector));

	if (!hdata->ddc_port)
		return ERR_PTR(-ENODEV);

	edid = drm_get_edid(connector, hdata->ddc_port->adapter);
	if (!edid)
		return ERR_PTR(-ENODEV);

	/*
	 * TODO : Need to call this in exynos_drm_connector.c, do a drm_get_edid
	 * to get the edid and then call drm_detect_hdmi_monitor.
	 */
	hdata->has_hdmi_sink = drm_detect_hdmi_monitor(edid);
	hdata->has_hdmi_audio = drm_detect_monitor_audio(edid);
	DRM_DEBUG_KMS("%s monitor\n", hdata->has_hdmi_sink ? "hdmi" : "dvi");

	return edid;
}

static int hdmi_v13_check_timing(struct fb_videomode *check_timing)
{
	int i;

	DRM_DEBUG_KMS("valid mode : xres=%d, yres=%d, refresh=%d, intl=%d\n",
			check_timing->xres, check_timing->yres,
			check_timing->refresh, (check_timing->vmode &
			FB_VMODE_INTERLACED) ? true : false);

	for (i = 0; i < ARRAY_SIZE(hdmi_v13_confs); ++i)
		if (hdmi_v13_confs[i].width == check_timing->xres &&
			hdmi_v13_confs[i].height == check_timing->yres &&
			hdmi_v13_confs[i].vrefresh == check_timing->refresh &&
			hdmi_v13_confs[i].interlace ==
			((check_timing->vmode & FB_VMODE_INTERLACED) ?
			 true : false))
				return 0;

	/* TODO */

	return -EINVAL;
}

static int find_hdmiphy_conf(int pixel_clock)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(phy_configs); i++) {
		if (phy_configs[i].pixel_clock == pixel_clock)
			return i;
	}
	DRM_DEBUG_KMS("Could not find phy config for %d\n", pixel_clock);
	return -EINVAL;
}

static int hdmi_v14_check_timing(struct fb_videomode *mode)
{
	int ret;
	enum exynos_mixer_mode_type mode_type;

	/* Make sure the mixer can generate this mode */
	mode_type = exynos_mixer_get_mode_type(mode->xres, mode->yres);
	if (mode_type == EXYNOS_MIXER_MODE_INVALID)
		return -EINVAL;

	ret = find_hdmiphy_conf(mode->pixclock);
	return ret < 0 ? ret : 0;
}

static u8 hdmi_chksum(struct hdmi_context *hdata,
			u32 start, u8 len, u32 hdr_sum)
{
	int i;
	/* hdr_sum : header0 + header1 + header2
	* start : start address of packet byte1
	* len : packet bytes - 1 */
	for (i = 0; i < len; ++i)
		hdr_sum += hdmi_reg_read(hdata, start + i * 4);

	return (u8)(0x100 - (hdr_sum & 0xff));
}

void hdmi_reg_infoframe(struct hdmi_context *hdata,
			struct hdmi_infoframe *infoframe)
{
	u32 hdr_sum;
	u8 chksum;
	u32 aspect_ratio;
	u32 mod;

	/* TODO: stringify HDMI_PACKET_TYPE */
	DRM_DEBUG_KMS("type: %d ver: %d len: %d\n", infoframe->type,
			infoframe->ver, infoframe->len);
	mod = hdmi_reg_read(hdata, HDMI_MODE_SEL);
	if (!hdata->has_hdmi_sink) {
		hdmi_reg_writeb(hdata, HDMI_VSI_CON,
				HDMI_VSI_CON_DO_NOT_TRANSMIT);
		hdmi_reg_writeb(hdata, HDMI_AVI_CON,
				HDMI_AVI_CON_DO_NOT_TRANSMIT);
		hdmi_reg_writeb(hdata, HDMI_AUI_CON, HDMI_AUI_CON_NO_TRAN);
		return;
	}

	switch (infoframe->type) {

	case HDMI_PACKET_TYPE_AVI:
		hdmi_reg_writeb(hdata, HDMI_AVI_CON, HDMI_AVI_CON_EVERY_VSYNC);
		hdmi_reg_writeb(hdata, HDMI_AVI_HEADER0, infoframe->type);
		hdmi_reg_writeb(hdata, HDMI_AVI_HEADER1, infoframe->ver);
		hdmi_reg_writeb(hdata, HDMI_AVI_HEADER2, infoframe->len);
		hdr_sum = infoframe->type + infoframe->ver + infoframe->len;
		/* Output format zero hardcoded ,RGB YBCR selection */
		hdmi_reg_writeb(hdata, HDMI_AVI_BYTE(1), 0 << 5 |
			AVI_ACTIVE_FORMAT_VALID | AVI_UNDERSCANNED_DISPLAY_VALID);

		aspect_ratio = AVI_PIC_ASPECT_RATIO_16_9;

		hdmi_reg_writeb(hdata, HDMI_AVI_BYTE(2), aspect_ratio |
				AVI_SAME_AS_PIC_ASPECT_RATIO);
		hdmi_reg_writeb(hdata, HDMI_AVI_BYTE(4), hdata->mode_conf.vic);

		chksum = hdmi_chksum(hdata, HDMI_AVI_BYTE(1),
					infoframe->len, hdr_sum);
		DRM_DEBUG_KMS("AVI checksum = 0x%x\n", chksum);
		hdmi_reg_writeb(hdata, HDMI_AVI_CHECK_SUM, chksum);
		break;

	case HDMI_PACKET_TYPE_AUI:
		hdmi_reg_writeb(hdata, HDMI_AUI_CON, 0x02);
		hdmi_reg_writeb(hdata, HDMI_AUI_HEADER0, infoframe->type);
		hdmi_reg_writeb(hdata, HDMI_AUI_HEADER1, infoframe->ver);
		hdmi_reg_writeb(hdata, HDMI_AUI_HEADER2, infoframe->len);
		hdr_sum = infoframe->type + infoframe->ver + infoframe->len;
		chksum = hdmi_chksum(hdata, HDMI_AUI_BYTE(1),
					infoframe->len, hdr_sum);
		DRM_DEBUG_KMS("AUI checksum = 0x%x\n", chksum);
		hdmi_reg_writeb(hdata, HDMI_AUI_CHECK_SUM, chksum);
		break;

	default:
		break;
	}
}

static int hdmi_check_timing(void *ctx, void *timing)
{
	struct hdmi_context *hdata = ctx;
	struct fb_videomode *check_timing = timing;

	DRM_DEBUG_KMS("[%d]x[%d] [%d]Hz [%x]\n", check_timing->xres,
			check_timing->yres, check_timing->refresh,
			check_timing->vmode);

	if (hdata->is_v13)
		return hdmi_v13_check_timing(check_timing);
	else
		return hdmi_v14_check_timing(check_timing);
}

static void hdmi_set_acr(u32 freq, u8 *acr)
{
	u32 n, cts;

	switch (freq) {
	case 32000:
		n = 4096;
		cts = 27000;
		break;
	case 44100:
		n = 6272;
		cts = 30000;
		break;
	case 88200:
		n = 12544;
		cts = 30000;
		break;
	case 176400:
		n = 25088;
		cts = 30000;
		break;
	case 48000:
		n = 6144;
		cts = 27000;
		break;
	case 96000:
		n = 12288;
		cts = 27000;
		break;
	case 192000:
		n = 24576;
		cts = 27000;
		break;
	default:
		n = 0;
		cts = 0;
		break;
	}

	acr[1] = cts >> 16;
	acr[2] = cts >> 8 & 0xff;
	acr[3] = cts & 0xff;

	acr[4] = n >> 16;
	acr[5] = n >> 8 & 0xff;
	acr[6] = n & 0xff;
}

static void hdmi_reg_acr(struct hdmi_context *hdata, u8 *acr)
{
	hdmi_reg_writeb(hdata, HDMI_ACR_N0, acr[6]);
	hdmi_reg_writeb(hdata, HDMI_ACR_N1, acr[5]);
	hdmi_reg_writeb(hdata, HDMI_ACR_N2, acr[4]);
	hdmi_reg_writeb(hdata, HDMI_ACR_MCTS0, acr[3]);
	hdmi_reg_writeb(hdata, HDMI_ACR_MCTS1, acr[2]);
	hdmi_reg_writeb(hdata, HDMI_ACR_MCTS2, acr[1]);
	hdmi_reg_writeb(hdata, HDMI_ACR_CTS0, acr[3]);
	hdmi_reg_writeb(hdata, HDMI_ACR_CTS1, acr[2]);
	hdmi_reg_writeb(hdata, HDMI_ACR_CTS2, acr[1]);

	if (hdata->is_v13)
		hdmi_reg_writeb(hdata, HDMI_V13_ACR_CON, 4);
	else
		hdmi_reg_writeb(hdata, HDMI_ACR_CON, 4);
}

static void hdmi_audio_init(struct hdmi_context *hdata)
{
	u32 sample_rate, bits_per_sample, frame_size_code;
	u32 data_num, bit_ch, sample_frq;
	u32 val;
	u8 acr[7];

	sample_rate = 44100;
	bits_per_sample = 16;
	frame_size_code = 0;

	switch (bits_per_sample) {
	case 20:
		data_num = 2;
		bit_ch  = 1;
		break;
	case 24:
		data_num = 3;
		bit_ch  = 1;
		break;
	default:
		data_num = 1;
		bit_ch  = 0;
		break;
	}

	hdmi_set_acr(sample_rate, acr);
	hdmi_reg_acr(hdata, acr);

	hdmi_reg_writeb(hdata, HDMI_I2S_MUX_CON, HDMI_I2S_IN_DISABLE
				| HDMI_I2S_AUD_I2S | HDMI_I2S_CUV_I2S_ENABLE
				| HDMI_I2S_MUX_ENABLE);

	hdmi_reg_writeb(hdata, HDMI_I2S_MUX_CH, HDMI_I2S_CH0_EN
			| HDMI_I2S_CH1_EN | HDMI_I2S_CH2_EN);

	hdmi_reg_writeb(hdata, HDMI_I2S_MUX_CUV, HDMI_I2S_CUV_RL_EN);

	sample_frq = (sample_rate == 44100) ? 0 :
			(sample_rate == 48000) ? 2 :
			(sample_rate == 32000) ? 3 :
			(sample_rate == 96000) ? 0xa : 0x0;

	hdmi_reg_writeb(hdata, HDMI_I2S_CLK_CON, HDMI_I2S_CLK_DIS);
	hdmi_reg_writeb(hdata, HDMI_I2S_CLK_CON, HDMI_I2S_CLK_EN);

	val = hdmi_reg_read(hdata, HDMI_I2S_DSD_CON) | 0x01;
	hdmi_reg_writeb(hdata, HDMI_I2S_DSD_CON, val);

	/* Configuration I2S input ports. Configure I2S_PIN_SEL_0~4 */
	hdmi_reg_writeb(hdata, HDMI_I2S_PIN_SEL_0, HDMI_I2S_SEL_SCLK(5)
			| HDMI_I2S_SEL_LRCK(6));
	hdmi_reg_writeb(hdata, HDMI_I2S_PIN_SEL_1, HDMI_I2S_SEL_SDATA1(1)
			| HDMI_I2S_SEL_SDATA2(4));
	hdmi_reg_writeb(hdata, HDMI_I2S_PIN_SEL_2, HDMI_I2S_SEL_SDATA3(1)
			| HDMI_I2S_SEL_SDATA2(2));
	hdmi_reg_writeb(hdata, HDMI_I2S_PIN_SEL_3, HDMI_I2S_SEL_DSD(0));

	/* I2S_CON_1 & 2 */
	hdmi_reg_writeb(hdata, HDMI_I2S_CON_1, HDMI_I2S_SCLK_FALLING_EDGE
			| HDMI_I2S_L_CH_LOW_POL);
	hdmi_reg_writeb(hdata, HDMI_I2S_CON_2, HDMI_I2S_MSB_FIRST_MODE
			| HDMI_I2S_SET_BIT_CH(bit_ch)
			| HDMI_I2S_SET_SDATA_BIT(data_num)
			| HDMI_I2S_BASIC_FORMAT);

	/* Configure register related to CUV information */
	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_0, HDMI_I2S_CH_STATUS_MODE_0
			| HDMI_I2S_2AUD_CH_WITHOUT_PREEMPH
			| HDMI_I2S_COPYRIGHT
			| HDMI_I2S_LINEAR_PCM
			| HDMI_I2S_CONSUMER_FORMAT);
	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_1, HDMI_I2S_CD_PLAYER);
	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_2, HDMI_I2S_SET_SOURCE_NUM(0));
	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_3, HDMI_I2S_CLK_ACCUR_LEVEL_2
			| HDMI_I2S_SET_SMP_FREQ(sample_frq));
	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_4,
			HDMI_I2S_ORG_SMP_FREQ_44_1
			| HDMI_I2S_WORD_LEN_MAX24_24BITS
			| HDMI_I2S_WORD_LEN_MAX_24BITS);

	hdmi_reg_writeb(hdata, HDMI_I2S_CH_ST_CON, HDMI_I2S_CH_STATUS_RELOAD);
}

static void hdmi_audio_control(struct hdmi_context *hdata, bool onoff)
{
	if (!hdata->has_hdmi_audio)
		return;

	hdmi_reg_writeb(hdata, HDMI_AUI_CON, onoff ? 2 : 0);
	hdmi_reg_writemask(hdata, HDMI_CON_0, onoff ?
			HDMI_ASP_EN : HDMI_ASP_DIS, HDMI_ASP_MASK);
}

static void hdmi_conf_reset(struct hdmi_context *hdata)
{
	u32 reg;

	/* disable hpd handle for drm */
	hdata->hpd_handle = false;

	if (hdata->is_v13)
		reg = HDMI_V13_CORE_RSTOUT;
	else
		reg = HDMI_CORE_RSTOUT;

	/* resetting HDMI core */
	hdmi_reg_writemask(hdata, reg,  0, HDMI_CORE_SW_RSTOUT);
	mdelay(10);
	hdmi_reg_writemask(hdata, reg, ~0, HDMI_CORE_SW_RSTOUT);
	mdelay(10);

	/* enable hpd handle for drm */
	hdata->hpd_handle = true;
}

static void hdmi_enable_video(struct hdmi_context *hdata)
{
	if (hdata->is_v13)
		return;

	hdata->video_enabled = true;
	hdmi_reg_writemask(hdata, HDMI_CON_0, 0, HDMI_BLUE_SCR_EN);
}

static void hdmi_disable_video(struct hdmi_context *hdata)
{
	if (hdata->is_v13)
		return;

	/* Set the blue screen color to black */
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_R_0, 0);
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_R_1, 0);
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_G_0, 0);
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_G_1, 0);
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_B_0, 0);
	hdmi_reg_writeb(hdata, HDMI_BLUE_SCREEN_B_1, 0);

	/* Enable the "blue screen", which effectively disconnects the mixer */
	hdata->video_enabled = false;
	hdmi_reg_writemask(hdata, HDMI_CON_0, ~0, HDMI_BLUE_SCR_EN);
}

static void hdmi_conf_init(struct hdmi_context *hdata)
{
	struct hdmi_infoframe infoframe;
	/* disable hpd handle for drm */
	hdata->hpd_handle = false;

	/* enable HPD interrupts */
	hdmi_reg_writemask(hdata, HDMI_INTC_CON, 0, HDMI_INTC_EN_GLOBAL |
		HDMI_INTC_EN_HPD_PLUG | HDMI_INTC_EN_HPD_UNPLUG);
	mdelay(10);
	hdmi_reg_writemask(hdata, HDMI_INTC_CON, ~0, HDMI_INTC_EN_GLOBAL |
		HDMI_INTC_EN_HPD_PLUG | HDMI_INTC_EN_HPD_UNPLUG);

	/* choose HDMI mode */
	hdmi_reg_writemask(hdata, HDMI_MODE_SEL,
		HDMI_MODE_HDMI_EN, HDMI_MODE_MASK);

	if (hdata->video_enabled)
		hdmi_enable_video(hdata);
	else
		hdmi_disable_video(hdata);

	if (!hdata->has_hdmi_sink) {
		/* choose DVI mode */
		hdmi_reg_writemask(hdata, HDMI_MODE_SEL,
				HDMI_MODE_DVI_EN, HDMI_MODE_MASK);
		hdmi_reg_writeb(hdata, HDMI_CON_2,
				HDMI_VID_PREAMBLE_DIS | HDMI_GUARD_BAND_DIS);
	}

	if (hdata->is_v13) {
		/* choose bluescreen (fecal) color */
		hdmi_reg_writeb(hdata, HDMI_V13_BLUE_SCREEN_0, 0x12);
		hdmi_reg_writeb(hdata, HDMI_V13_BLUE_SCREEN_1, 0x34);
		hdmi_reg_writeb(hdata, HDMI_V13_BLUE_SCREEN_2, 0x56);

		/* enable AVI packet every vsync, fixes purple line problem */
		hdmi_reg_writeb(hdata, HDMI_V13_AVI_CON, 0x02);
		/* force RGB, look to CEA-861-D, table 7 for more detail */
		hdmi_reg_writeb(hdata, HDMI_V13_AVI_BYTE(0), 0 << 5);
		hdmi_reg_writemask(hdata, HDMI_CON_1, 0x10 << 5, 0x11 << 5);

		hdmi_reg_writeb(hdata, HDMI_V13_SPD_CON, 0x02);
		hdmi_reg_writeb(hdata, HDMI_V13_AUI_CON, 0x02);
		hdmi_reg_writeb(hdata, HDMI_V13_ACR_CON, 0x04);
	} else {
		/* enable AVI packet every vsync, fixes purple line problem */
		hdmi_reg_writemask(hdata, HDMI_CON_1, 2, 3 << 5);

		infoframe.type = HDMI_PACKET_TYPE_AVI;
		infoframe.ver = HDMI_AVI_VERSION;
		infoframe.len = HDMI_AVI_LENGTH;
		hdmi_reg_infoframe(hdata, &infoframe);

		infoframe.type = HDMI_PACKET_TYPE_AUI;
		infoframe.ver = HDMI_AUI_VERSION;
		infoframe.len = HDMI_AUI_LENGTH;
		hdmi_reg_infoframe(hdata, &infoframe);

	}

	/* enable hpd handle for drm */
	hdata->hpd_handle = true;
}

static void hdmi_v13_timing_apply(struct hdmi_context *hdata)
{
	const struct hdmi_v13_preset_conf *conf =
		hdmi_v13_confs[hdata->cur_conf].conf;
	const struct hdmi_v13_core_regs *core = &conf->core;
	const struct hdmi_v13_tg_regs *tg = &conf->tg;
	int tries;

	/* setting core registers */
	hdmi_reg_writeb(hdata, HDMI_H_BLANK_0, core->h_blank[0]);
	hdmi_reg_writeb(hdata, HDMI_H_BLANK_1, core->h_blank[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_0, core->v_blank[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_1, core->v_blank[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_2, core->v_blank[2]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_V_LINE_0, core->h_v_line[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_V_LINE_1, core->h_v_line[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_V_LINE_2, core->h_v_line[2]);
	hdmi_reg_writeb(hdata, HDMI_VSYNC_POL, core->vsync_pol[0]);
	hdmi_reg_writeb(hdata, HDMI_INT_PRO_MODE, core->int_pro_mode[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_F_0, core->v_blank_f[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_F_1, core->v_blank_f[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_BLANK_F_2, core->v_blank_f[2]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_SYNC_GEN_0, core->h_sync_gen[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_SYNC_GEN_1, core->h_sync_gen[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_H_SYNC_GEN_2, core->h_sync_gen[2]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_1_0, core->v_sync_gen1[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_1_1, core->v_sync_gen1[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_1_2, core->v_sync_gen1[2]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_2_0, core->v_sync_gen2[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_2_1, core->v_sync_gen2[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_2_2, core->v_sync_gen2[2]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_3_0, core->v_sync_gen3[0]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_3_1, core->v_sync_gen3[1]);
	hdmi_reg_writeb(hdata, HDMI_V13_V_SYNC_GEN_3_2, core->v_sync_gen3[2]);
	/* Timing generator registers */
	hdmi_reg_writeb(hdata, HDMI_TG_H_FSZ_L, tg->h_fsz_l);
	hdmi_reg_writeb(hdata, HDMI_TG_H_FSZ_H, tg->h_fsz_h);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_ST_L, tg->hact_st_l);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_ST_H, tg->hact_st_h);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_SZ_L, tg->hact_sz_l);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_SZ_H, tg->hact_sz_h);
	hdmi_reg_writeb(hdata, HDMI_TG_V_FSZ_L, tg->v_fsz_l);
	hdmi_reg_writeb(hdata, HDMI_TG_V_FSZ_H, tg->v_fsz_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_L, tg->vsync_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_H, tg->vsync_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC2_L, tg->vsync2_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC2_H, tg->vsync2_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST_L, tg->vact_st_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST_H, tg->vact_st_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_SZ_L, tg->vact_sz_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_SZ_H, tg->vact_sz_h);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_CHG_L, tg->field_chg_l);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_CHG_H, tg->field_chg_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST2_L, tg->vact_st2_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST2_H, tg->vact_st2_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_TOP_HDMI_L, tg->vsync_top_hdmi_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_TOP_HDMI_H, tg->vsync_top_hdmi_h);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_BOT_HDMI_L, tg->vsync_bot_hdmi_l);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_BOT_HDMI_H, tg->vsync_bot_hdmi_h);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_TOP_HDMI_L, tg->field_top_hdmi_l);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_TOP_HDMI_H, tg->field_top_hdmi_h);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_BOT_HDMI_L, tg->field_bot_hdmi_l);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_BOT_HDMI_H, tg->field_bot_hdmi_h);

	/* waiting for HDMIPHY's PLL to get to steady state */
	for (tries = 100; tries; --tries) {
		u32 val = hdmi_reg_read(hdata, HDMI_V13_PHY_STATUS);
		if (val & HDMI_PHY_STATUS_READY)
			break;
		mdelay(1);
	}
	/* steady state not achieved */
	if (tries == 0) {
		DRM_ERROR("hdmiphy's pll could not reach steady state.\n");
		hdmi_regs_dump(hdata, "timing apply");
	}

	clk_disable(hdata->res.sclk_hdmi);
	clk_set_parent(hdata->res.sclk_hdmi, hdata->res.sclk_hdmiphy);
	clk_enable(hdata->res.sclk_hdmi);

	/* enable HDMI and timing generator */
	hdmi_reg_writemask(hdata, HDMI_CON_0, ~0, HDMI_EN);
	if (core->int_pro_mode[0])
		hdmi_reg_writemask(hdata, HDMI_TG_CMD, ~0, HDMI_TG_EN |
				HDMI_FIELD_EN);
	else
		hdmi_reg_writemask(hdata, HDMI_TG_CMD, ~0, HDMI_TG_EN);
}

static void hdmi_v14_timing_apply(struct hdmi_context *hdata)
{
	struct hdmi_core_regs *core = &hdata->mode_conf.core;
	struct hdmi_tg_regs *tg = &hdata->mode_conf.tg;
	int tries;

	hdmi_reg_writeb(hdata, HDMI_H_BLANK_0, core->h_blank[0]);
	hdmi_reg_writeb(hdata, HDMI_H_BLANK_1, core->h_blank[1]);
	hdmi_reg_writeb(hdata, HDMI_V2_BLANK_0, core->v2_blank[0]);
	hdmi_reg_writeb(hdata, HDMI_V2_BLANK_1, core->v2_blank[1]);
	hdmi_reg_writeb(hdata, HDMI_V1_BLANK_0, core->v1_blank[0]);
	hdmi_reg_writeb(hdata, HDMI_V1_BLANK_1, core->v1_blank[1]);
	hdmi_reg_writeb(hdata, HDMI_V_LINE_0, core->v_line[0]);
	hdmi_reg_writeb(hdata, HDMI_V_LINE_1, core->v_line[1]);
	hdmi_reg_writeb(hdata, HDMI_H_LINE_0, core->h_line[0]);
	hdmi_reg_writeb(hdata, HDMI_H_LINE_1, core->h_line[1]);
	hdmi_reg_writeb(hdata, HDMI_HSYNC_POL, core->hsync_pol[0]);
	hdmi_reg_writeb(hdata, HDMI_VSYNC_POL, core->vsync_pol[0]);
	hdmi_reg_writeb(hdata, HDMI_INT_PRO_MODE, core->int_pro_mode[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F0_0, core->v_blank_f0[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F0_1, core->v_blank_f0[1]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F1_0, core->v_blank_f1[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F1_1, core->v_blank_f1[1]);
	hdmi_reg_writeb(hdata, HDMI_H_SYNC_START_0, core->h_sync_start[0]);
	hdmi_reg_writeb(hdata, HDMI_H_SYNC_START_1, core->h_sync_start[1]);
	hdmi_reg_writeb(hdata, HDMI_H_SYNC_END_0, core->h_sync_end[0]);
	hdmi_reg_writeb(hdata, HDMI_H_SYNC_END_1, core->h_sync_end[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_BEF_2_0,
			core->v_sync_line_bef_2[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_BEF_2_1,
			core->v_sync_line_bef_2[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_BEF_1_0,
			core->v_sync_line_bef_1[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_BEF_1_1,
			core->v_sync_line_bef_1[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_2_0,
			core->v_sync_line_aft_2[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_2_1,
			core->v_sync_line_aft_2[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_1_0,
			core->v_sync_line_aft_1[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_1_1,
			core->v_sync_line_aft_1[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_2_0,
			core->v_sync_line_aft_pxl_2[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_2_1,
			core->v_sync_line_aft_pxl_2[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_1_0,
			core->v_sync_line_aft_pxl_1[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_1_1,
			core->v_sync_line_aft_pxl_1[1]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F2_0, core->v_blank_f2[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F2_1, core->v_blank_f2[1]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F3_0, core->v_blank_f3[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F3_1, core->v_blank_f3[1]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F4_0, core->v_blank_f4[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F4_1, core->v_blank_f4[1]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F5_0, core->v_blank_f5[0]);
	hdmi_reg_writeb(hdata, HDMI_V_BLANK_F5_1, core->v_blank_f5[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_3_0,
			core->v_sync_line_aft_3[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_3_1,
			core->v_sync_line_aft_3[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_4_0,
			core->v_sync_line_aft_4[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_4_1,
			core->v_sync_line_aft_4[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_5_0,
			core->v_sync_line_aft_5[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_5_1,
			core->v_sync_line_aft_5[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_6_0,
			core->v_sync_line_aft_6[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_6_1,
			core->v_sync_line_aft_6[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_3_0,
			core->v_sync_line_aft_pxl_3[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_3_1,
			core->v_sync_line_aft_pxl_3[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_4_0,
			core->v_sync_line_aft_pxl_4[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_4_1,
			core->v_sync_line_aft_pxl_4[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_5_0,
			core->v_sync_line_aft_pxl_5[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_5_1,
			core->v_sync_line_aft_pxl_5[1]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_6_0,
			core->v_sync_line_aft_pxl_6[0]);
	hdmi_reg_writeb(hdata, HDMI_V_SYNC_LINE_AFT_PXL_6_1,
			core->v_sync_line_aft_pxl_6[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_1_0, core->vact_space_1[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_1_1, core->vact_space_1[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_2_0, core->vact_space_2[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_2_1, core->vact_space_2[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_3_0, core->vact_space_3[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_3_1, core->vact_space_3[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_4_0, core->vact_space_4[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_4_1, core->vact_space_4[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_5_0, core->vact_space_5[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_5_1, core->vact_space_5[1]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_6_0, core->vact_space_6[0]);
	hdmi_reg_writeb(hdata, HDMI_VACT_SPACE_6_1, core->vact_space_6[1]);

	hdmi_reg_writeb(hdata, HDMI_TG_H_FSZ_L, tg->h_fsz[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_H_FSZ_H, tg->h_fsz[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_ST_L, tg->hact_st[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_ST_H, tg->hact_st[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_SZ_L, tg->hact_sz[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_HACT_SZ_H, tg->hact_sz[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_V_FSZ_L, tg->v_fsz[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_V_FSZ_H, tg->v_fsz[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_L, tg->vsync[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_H, tg->vsync[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC2_L, tg->vsync2[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC2_H, tg->vsync2[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST_L, tg->vact_st[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST_H, tg->vact_st[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_SZ_L, tg->vact_sz[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_SZ_H, tg->vact_sz[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_CHG_L, tg->field_chg[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_CHG_H, tg->field_chg[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST2_L, tg->vact_st2[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST2_H, tg->vact_st2[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST3_L, tg->vact_st3[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST3_H, tg->vact_st3[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST4_L, tg->vact_st4[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VACT_ST4_H, tg->vact_st4[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_TOP_HDMI_L, tg->vsync_top_hdmi[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_TOP_HDMI_H, tg->vsync_top_hdmi[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_BOT_HDMI_L, tg->vsync_bot_hdmi[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_VSYNC_BOT_HDMI_H, tg->vsync_bot_hdmi[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_TOP_HDMI_L, tg->field_top_hdmi[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_TOP_HDMI_H, tg->field_top_hdmi[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_BOT_HDMI_L, tg->field_bot_hdmi[0]);
	hdmi_reg_writeb(hdata, HDMI_TG_FIELD_BOT_HDMI_H, tg->field_bot_hdmi[1]);
	hdmi_reg_writeb(hdata, HDMI_TG_3D, tg->tg_3d[0]);

	/* waiting for HDMIPHY's PLL to get to steady state */
	for (tries = 100; tries; --tries) {
		u32 val = hdmi_reg_read(hdata, HDMI_PHY_STATUS_0);
		if (val & HDMI_PHY_STATUS_READY)
			break;
		mdelay(1);
	}
	/* steady state not achieved */
	if (tries == 0) {
		DRM_ERROR("hdmiphy's pll could not reach steady state.\n");
		hdmi_regs_dump(hdata, "timing apply");
	}

	clk_disable(hdata->res.sclk_hdmi);
	clk_set_parent(hdata->res.sclk_hdmi, hdata->res.sclk_hdmiphy);
	clk_enable(hdata->res.sclk_hdmi);

	/* enable HDMI and timing generator */
	hdmi_reg_writemask(hdata, HDMI_CON_0, ~0, HDMI_EN);
	if (core->int_pro_mode[0])
		hdmi_reg_writemask(hdata, HDMI_TG_CMD, ~0, HDMI_TG_EN |
				HDMI_FIELD_EN);
	else
		hdmi_reg_writemask(hdata, HDMI_TG_CMD, ~0, HDMI_TG_EN);
}

static void hdmi_timing_apply(struct hdmi_context *hdata)
{
	if (hdata->is_v13)
		hdmi_v13_timing_apply(hdata);
	else
		hdmi_v14_timing_apply(hdata);
}

static void hdmiphy_conf_reset(struct hdmi_context *hdata)
{
	u8 buffer[2];
	u32 reg;

	clk_disable(hdata->res.sclk_hdmi);
	clk_set_parent(hdata->res.sclk_hdmi, hdata->res.sclk_pixel);
	clk_enable(hdata->res.sclk_hdmi);

	/* operation mode */
	buffer[0] = 0x1f;
	buffer[1] = 0x00;

	if (hdata->hdmiphy_port)
		i2c_master_send(hdata->hdmiphy_port, buffer, 2);

	if (hdata->is_v13)
		reg = HDMI_V13_PHY_RSTOUT;
	else
		reg = HDMI_PHY_RSTOUT;

	/* reset hdmiphy */
	hdmi_reg_writemask(hdata, reg, ~0, HDMI_PHY_SW_RSTOUT);
	mdelay(10);
	hdmi_reg_writemask(hdata, reg,  0, HDMI_PHY_SW_RSTOUT);
	mdelay(10);
}

static void hdmiphy_conf_apply(struct hdmi_context *hdata)
{
	const u8 *hdmiphy_data;
	u8 buffer[32];
	u8 operation[2];
	u8 read_buffer[32] = {0, };
	int ret;
	int i;

	DRM_DEBUG_KMS("\n");

	if (!hdata->hdmiphy_port) {
		DRM_ERROR("hdmiphy is not attached\n");
		return;
	}

	/* pixel clock */
	if (hdata->is_v13) {
		hdmiphy_data = hdmi_v13_confs[hdata->cur_conf].hdmiphy_data;
	} else {
		i = find_hdmiphy_conf(hdata->mode_conf.pixel_clock);
		hdmiphy_data = phy_configs[i].conf;
	}

	memcpy(buffer, hdmiphy_data, 32);
	ret = i2c_master_send(hdata->hdmiphy_port, buffer, 32);
	if (ret != 32) {
		DRM_ERROR("failed to configure HDMIPHY via I2C\n");
		return;
	}

	mdelay(10);

	/* operation mode */
	operation[0] = 0x1f;
	operation[1] = 0x80;

	ret = i2c_master_send(hdata->hdmiphy_port, operation, 2);
	if (ret != 2) {
		DRM_ERROR("failed to enable hdmiphy\n");
		return;
	}

	ret = i2c_master_recv(hdata->hdmiphy_port, read_buffer, 32);
	if (ret < 0) {
		DRM_ERROR("failed to read hdmiphy config\n");
		return;
	}

	for (i = 0; i < ret; i++)
		DRM_DEBUG_KMS("hdmiphy[0x%02x] write[0x%02x] - "
			"recv [0x%02x]\n", i, buffer[i], read_buffer[i]);
}

static void hdmi_conf_apply(struct hdmi_context *hdata)
{
	DRM_DEBUG_KMS("\n");

	hdmiphy_conf_reset(hdata);
	hdmiphy_conf_apply(hdata);

	hdmi_conf_reset(hdata);
	hdmi_conf_init(hdata);
	if (!hdata->is_soc_exynos5)
		hdmi_audio_init(hdata);

	/* setting core registers */
	hdmi_timing_apply(hdata);
	if (!hdata->is_soc_exynos5)
		hdmi_audio_control(hdata, true);

	hdmi_regs_dump(hdata, "start");
}

static void hdmi_mode_copy(struct drm_display_mode *dst,
	struct drm_display_mode *src)
{
	struct drm_mode_object base;

	DRM_DEBUG_KMS("[MODE:%d:%s]\n", DRM_BASE_ID(src), src->name);

	/* following information should be preserved,
	 * required for releasing the drm_display_mode node,
	 * duplicated to recieve adjustment info. */

	base.id = dst->base.id;
	base.type = dst->base.type;

	memcpy(dst, src, sizeof(struct drm_display_mode));

	dst->base.id = base.id;
	dst->base.type = base.type;
}

static void hdmi_mode_fixup(void *ctx, struct drm_connector *connector,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	struct drm_display_mode *m;
	struct hdmi_context *hdata = ctx;
	int index;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] [MODE:%d:%s]\n",
			DRM_BASE_ID(connector),
			drm_get_connector_name(connector),
			DRM_BASE_ID(mode), mode->name);

	drm_mode_set_crtcinfo(adjusted_mode, 0);

	if (hdata->is_v13)
		index = hdmi_v13_conf_index(adjusted_mode);
	else
		index = find_hdmiphy_conf(adjusted_mode->clock * 1000);

	/* just return if user desired mode exists. */
	if (index >= 0)
		return;

	/*
	 * otherwise, find the most suitable mode among modes and change it
	 * to adjusted_mode.
	 */
	list_for_each_entry(m, &connector->modes, head) {
		if (hdata->is_v13)
			index = hdmi_v13_conf_index(m);
		else
			index = find_hdmiphy_conf(m->clock * 1000);

		if (index >= 0) {
			DRM_INFO("desired mode doesn't exist so\n");
			DRM_INFO("use the most suitable mode among modes.\n");
			hdmi_mode_copy(adjusted_mode, m);
			break;
		}
	}
}

static void hdmi_set_reg(u8 *reg_pair, int num_bytes, u32 value)
{
	int i;
	BUG_ON(num_bytes > 4);
	for (i = 0; i < num_bytes; i++)
		reg_pair[i] = (value >> (8 * i)) & 0xff;
}

static void hdmi_v14_mode_set(struct hdmi_context *hdata,
			struct drm_display_mode *m)
{
	struct hdmi_core_regs *core = &hdata->mode_conf.core;
	struct hdmi_tg_regs *tg = &hdata->mode_conf.tg;

	DRM_DEBUG_KMS("[MODE:%d:%s]\n", DRM_BASE_ID(m), m->name);

	hdata->mode_conf.vic = drm_match_cea_mode(m);

	hdata->mode_conf.pixel_clock = m->clock * 1000;
	hdmi_set_reg(core->h_blank, 2, m->htotal - m->hdisplay);
	hdmi_set_reg(core->v_line, 2, m->vtotal);
	hdmi_set_reg(core->h_line, 2, m->htotal);
	hdmi_set_reg(core->hsync_pol, 1,
			(m->flags & DRM_MODE_FLAG_NHSYNC)  ? 1 : 0);
	hdmi_set_reg(core->vsync_pol, 1,
			(m->flags & DRM_MODE_FLAG_NVSYNC) ? 1 : 0);
	hdmi_set_reg(core->int_pro_mode, 1,
			(m->flags & DRM_MODE_FLAG_INTERLACE) ? 1 : 0);

	/*
	 * Quirk requirement for exynos 5 HDMI IP design,
	 * 2 pixels less than the actual calculation for hsync_start
	 * and end.
	 */

	/* Following values & calculations differ for different type of modes */
	if (m->flags & DRM_MODE_FLAG_INTERLACE) {
		/* Interlaced Mode */
		hdmi_set_reg(core->v_sync_line_bef_2, 2,
			(m->vsync_end - m->vdisplay) / 2);
		hdmi_set_reg(core->v_sync_line_bef_1, 2,
			(m->vsync_start - m->vdisplay) / 2);
		hdmi_set_reg(core->v2_blank, 2, m->vtotal / 2);
		hdmi_set_reg(core->v1_blank, 2, (m->vtotal - m->vdisplay) / 2);
		hdmi_set_reg(core->v_blank_f0, 2,
			(m->vtotal + ((m->vsync_end - m->vsync_start) * 4) + 5) / 2);
		hdmi_set_reg(core->v_blank_f1, 2, m->vtotal);
		hdmi_set_reg(core->v_sync_line_aft_2, 2, (m->vtotal / 2) + 7);
		hdmi_set_reg(core->v_sync_line_aft_1, 2, (m->vtotal / 2) + 2);
		hdmi_set_reg(core->v_sync_line_aft_pxl_2, 2,
			(m->htotal / 2) + (m->hsync_start - m->hdisplay));
		hdmi_set_reg(core->v_sync_line_aft_pxl_1, 2,
			(m->htotal / 2) + (m->hsync_start - m->hdisplay));
		hdmi_set_reg(tg->vact_st, 2, (m->vtotal - m->vdisplay) / 2);
		hdmi_set_reg(tg->vact_sz, 2, m->vdisplay / 2);
		hdmi_set_reg(tg->vact_st2, 2, 0x249);/* Reset value + 1*/
		hdmi_set_reg(tg->vact_st3, 2, 0x0);
		hdmi_set_reg(tg->vact_st4, 2, 0x0);
	} else {
		/* Progressive Mode */
		hdmi_set_reg(core->v_sync_line_bef_2, 2,
			m->vsync_end - m->vdisplay);
		hdmi_set_reg(core->v_sync_line_bef_1, 2,
			m->vsync_start - m->vdisplay);
		hdmi_set_reg(core->v2_blank, 2, m->vtotal);
		hdmi_set_reg(core->v1_blank, 2, m->vtotal - m->vdisplay);
		hdmi_set_reg(core->v_blank_f0, 2, 0xffff);
		hdmi_set_reg(core->v_blank_f1, 2, 0xffff);
		hdmi_set_reg(core->v_sync_line_aft_2, 2, 0xffff);
		hdmi_set_reg(core->v_sync_line_aft_1, 2, 0xffff);
		hdmi_set_reg(core->v_sync_line_aft_pxl_2, 2, 0xffff);
		hdmi_set_reg(core->v_sync_line_aft_pxl_1, 2, 0xffff);
		hdmi_set_reg(tg->vact_st, 2, m->vtotal - m->vdisplay);
		hdmi_set_reg(tg->vact_sz, 2, m->vdisplay);
		hdmi_set_reg(tg->vact_st2, 2, 0x248); /* Reset value */
		hdmi_set_reg(tg->vact_st3, 2, 0x47b); /* Reset value */
		hdmi_set_reg(tg->vact_st4, 2, 0x6ae); /* Reset value */
	}

	/* Following values & calculations are same irrespective of mode type */
	hdmi_set_reg(core->h_sync_start, 2, m->hsync_start - m->hdisplay - 2);
	hdmi_set_reg(core->h_sync_end, 2, m->hsync_end - m->hdisplay - 2);
	hdmi_set_reg(core->vact_space_1, 2, 0xffff);
	hdmi_set_reg(core->vact_space_2, 2, 0xffff);
	hdmi_set_reg(core->vact_space_3, 2, 0xffff);
	hdmi_set_reg(core->vact_space_4, 2, 0xffff);
	hdmi_set_reg(core->vact_space_5, 2, 0xffff);
	hdmi_set_reg(core->vact_space_6, 2, 0xffff);
	hdmi_set_reg(core->v_blank_f2, 2, 0xffff);
	hdmi_set_reg(core->v_blank_f3, 2, 0xffff);
	hdmi_set_reg(core->v_blank_f4, 2, 0xffff);
	hdmi_set_reg(core->v_blank_f5, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_3, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_4, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_5, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_6, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_pxl_3, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_pxl_4, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_pxl_5, 2, 0xffff);
	hdmi_set_reg(core->v_sync_line_aft_pxl_6, 2, 0xffff);

	/* Timing generator registers */
	hdmi_set_reg(tg->cmd, 1, 0x0);
	hdmi_set_reg(tg->h_fsz, 2, m->htotal);
	hdmi_set_reg(tg->hact_st, 2, m->htotal - m->hdisplay);
	hdmi_set_reg(tg->hact_sz, 2, m->hdisplay);
	hdmi_set_reg(tg->v_fsz, 2, m->vtotal);
	hdmi_set_reg(tg->vsync, 2, 0x1);
	hdmi_set_reg(tg->vsync2, 2, 0x233); /* Reset value */
	hdmi_set_reg(tg->field_chg, 2, 0x233); /* Reset value */
	hdmi_set_reg(tg->vsync_top_hdmi, 2, 0x1); /* Reset value */
	hdmi_set_reg(tg->vsync_bot_hdmi, 2, 0x233); /* Reset value */
	hdmi_set_reg(tg->field_top_hdmi, 2, 0x1); /* Reset value */
	hdmi_set_reg(tg->field_bot_hdmi, 2, 0x233); /* Reset value */
	hdmi_set_reg(tg->tg_3d, 1, 0x0);

	/* Workaround 4 implementation for 1440x900 resolution support */
	if (hdata->is_soc_exynos5) {
		if (m->hdisplay == 1440 && m->vdisplay == 900 && m->clock == 106500) {
			hdmi_set_reg(tg->hact_st, 2, m->htotal - m->hdisplay - 0xe0);
			hdmi_set_reg(tg->hact_sz, 2, m->hdisplay + 0xe0);
		}
	}
}

static void hdmi_mode_set(void *ctx, struct drm_display_mode *mode)
{
	struct hdmi_context *hdata = ctx;

	DRM_DEBUG_KMS("[MODE:%d:%s]\n", DRM_BASE_ID(mode), mode->name);

	if (hdata->is_v13)
		hdata->cur_conf = hdmi_v13_conf_index(mode);
	else
		hdmi_v14_mode_set(hdata, mode);
}

static void hdmi_commit(void *ctx)
{
	struct hdmi_context *hdata = ctx;

	DRM_DEBUG_KMS("is_hdmi_powered_on: %u\n", hdata->is_hdmi_powered_on);

	if (!hdata->is_hdmi_powered_on)
		return;

	hdmi_conf_apply(hdata);
	hdata->enabled = true;
}

static int hdmiphy_update_bits(struct i2c_client *client, u8 *reg_cache,
			       u8 reg, u8 mask, u8 val)
{
	int ret;
	u8 buffer[2];

	buffer[0] = reg;
	buffer[1] = (reg_cache[reg] & ~mask) | (val & mask);
	reg_cache[reg] = buffer[1];

	ret = i2c_master_send(client, buffer, 2);
	if (ret != 2)
		return -EIO;

	return 0;
}

static int hdmiphy_s_power(struct i2c_client *client, bool on)
{
	u8 reg_cache[32] = { 0 };
	u8 buffer[2];
	int ret;

	DRM_DEBUG_KMS("%s\n", on ? "on" : "off");

	/* Cache all 32 registers to make the code below faster */
	buffer[0] = 0x0;
	ret = i2c_master_send(client, buffer, 1);
	if (ret != 1) {
		ret = -EIO;
		goto exit;
	}
	ret = i2c_master_recv(client, reg_cache, 32);
	if (ret != 32) {
		ret = -EIO;
		goto exit;
	}

	/* Go to/from configuration from/to operation mode */
	ret = hdmiphy_update_bits(client, reg_cache, 0x1f, 0xff,
				  on ? 0x80 : 0x00);
	if (ret)
		goto exit;

	/*
	 * Turn off undocumented "oscpad" if !on; it turns on again in
	 * hdmiphy_conf_apply()
	 */
	if (!on)
		ret = hdmiphy_update_bits(client, reg_cache, 0x0b, 0xc0, 0x00);
		if (ret)
			goto exit;

	/* Disable powerdown if on; enable if !on */
	ret = hdmiphy_update_bits(client, reg_cache, 0x1d, 0x80,
				  on ? 0 : ~0);
	if (ret)
		goto exit;
	ret = hdmiphy_update_bits(client, reg_cache, 0x1d, 0x77,
				  on ? 0 : ~0);
	if (ret)
		goto exit;

	/*
	 * Turn off bit 3 of reg 4 if !on; it turns on again in
	 * hdmiphy_conf_apply().  It's unclear what this bit does.
	 */
	if (!on)
		ret = hdmiphy_update_bits(client, reg_cache, 0x04, BIT(3), 0);
		if (ret)
			goto exit;

exit:
	/* Don't expect any errors so just do a single warn */
	WARN_ON(ret);

	return ret;
}

static void hdmi_resource_poweron(struct hdmi_context *hdata)
{
	struct hdmi_resources *res = &hdata->res;

	hdata->is_hdmi_powered_on = true;
	hdmi_cfg_hpd(hdata, false);

	/* irq change by TV power status */
	if (hdata->curr_irq == hdata->internal_irq)
		return;

	disable_irq(hdata->curr_irq);

	hdata->curr_irq = hdata->internal_irq;

	enable_irq(hdata->curr_irq);

	/* turn HDMI power on */
	regulator_bulk_enable(res->regul_count, res->regul_bulk);

	/* power-on hdmi clocks */
	clk_enable(res->hdmiphy);

	hdmiphy_s_power(hdata->hdmiphy_port, 1);
	hdmiphy_conf_reset(hdata);
	hdmi_conf_reset(hdata);
	hdmi_conf_init(hdata);
	if (!hdata->is_soc_exynos5)
		hdmi_audio_init(hdata);
	hdmi_commit(hdata);
}

static void hdmi_resource_poweroff(struct hdmi_context *hdata)
{
	struct hdmi_resources *res = &hdata->res;

	hdmi_cfg_hpd(hdata, true);

	if (hdata->curr_irq == hdata->external_irq)
		return;

	disable_irq(hdata->curr_irq);
	hdata->curr_irq = hdata->external_irq;

	enable_irq(hdata->curr_irq);
	hdata->is_hdmi_powered_on = false;

	hdmiphy_s_power(hdata->hdmiphy_port, 0);
	hdmiphy_conf_reset(hdata);

	/* power-off hdmi clocks */
	clk_disable(res->hdmiphy);

	/* turn HDMI power off */
	regulator_bulk_disable(res->regul_count, res->regul_bulk);
}

static int hdmi_dpms(void *ctx, int mode)
{
	struct hdmi_context *hdata = ctx;

	DRM_DEBUG_KMS("[DPMS:%s]\n", drm_get_dpms_name(mode));

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		if (!hdata->is_hdmi_powered_on)
			hdmi_resource_poweron(hdata);
		hdmi_enable_video(hdata);
		break;
	case DRM_MODE_DPMS_STANDBY:
		hdmi_disable_video(hdata);
		break;
	case DRM_MODE_DPMS_OFF:
	case DRM_MODE_DPMS_SUSPEND:
		if (hdata->is_hdmi_powered_on)
			hdmi_resource_poweroff(hdata);
		break;
	default:
		DRM_DEBUG_KMS("unknown dpms mode: %d\n", mode);
		break;
	}

	return 0;
}

static int hdmi_subdrv_probe(void *ctx, struct drm_device *drm_dev)
{
	struct hdmi_context *hdata = ctx;

	DRM_DEBUG_KMS("[DEV:%s]\n", drm_dev->devname);

	hdata->drm_dev = drm_dev;

	return 0;
}

static struct exynos_panel_ops hdmi_ops = {
	/* display */
	.subdrv_probe	= hdmi_subdrv_probe,
	.is_connected	= hdmi_is_connected,
	.get_edid	= hdmi_get_edid,
	.check_timing	= hdmi_check_timing,
	.dpms		= hdmi_dpms,

	/* manager */
	.mode_fixup	= hdmi_mode_fixup,
	.mode_set	= hdmi_mode_set,
	.commit		= hdmi_commit,
};

/*
 * Handle hotplug events outside the interrupt handler proper.
 */
static void hdmi_hotplug_func(struct work_struct *work)
{
	struct hdmi_context *hdata =
		container_of(work, struct hdmi_context, hotplug_work);

	drm_helper_hpd_irq_event(hdata->drm_dev);
}

static irqreturn_t hdmi_irq_handler(int irq, void *arg)
{
	struct hdmi_context *hdata = arg;
	u32 intc_flag;
	if (hdata->is_hdmi_powered_on) {
		intc_flag = hdmi_reg_read(hdata, HDMI_INTC_FLAG);
		/* clearing flags for HPD plug/unplug */
		if (intc_flag & HDMI_INTC_FLAG_HPD_UNPLUG) {
			DRM_DEBUG_KMS("int unplugged, handling:%d\n",
				hdata->hpd_handle);
			hdmi_reg_writemask(hdata, HDMI_INTC_FLAG, ~0,
				HDMI_INTC_FLAG_HPD_UNPLUG);
		}
		if (intc_flag & HDMI_INTC_FLAG_HPD_PLUG) {
			DRM_DEBUG_KMS("int plugged, handling:%d\n",
				hdata->hpd_handle);
			hdmi_reg_writemask(hdata, HDMI_INTC_FLAG, ~0,
				HDMI_INTC_FLAG_HPD_PLUG);
		}
	}

	if (hdata->drm_dev && hdata->hpd_handle)
		queue_work(hdata->wq, &hdata->hotplug_work);

	return IRQ_HANDLED;
}

static int __devinit hdmi_resources_init(struct hdmi_context *hdata)
{
	struct device *dev = hdata->dev;
	struct hdmi_resources *res = &hdata->res;
#ifndef CONFIG_ARCH_EXYNOS5
	static char *supply[] = {
		"hdmi-en",
		"vdd",
		"vdd_osc",
		"vdd_pll",
	};
	int i, ret;
#endif

	DRM_DEBUG_KMS("HDMI resource init\n");

	memset(res, 0, sizeof *res);

	/* get clocks, power */
	res->hdmi = clk_get(dev, "hdmi");
	if (IS_ERR_OR_NULL(res->hdmi)) {
		DRM_ERROR("failed to get clock 'hdmi'\n");
		goto fail;
	}
	res->sclk_hdmi = clk_get(dev, "sclk_hdmi");
	if (IS_ERR_OR_NULL(res->sclk_hdmi)) {
		DRM_ERROR("failed to get clock 'sclk_hdmi'\n");
		goto fail;
	}
	res->sclk_pixel = clk_get(dev, "sclk_pixel");
	if (IS_ERR_OR_NULL(res->sclk_pixel)) {
		DRM_ERROR("failed to get clock 'sclk_pixel'\n");
		goto fail;
	}
	res->sclk_hdmiphy = clk_get(dev, "sclk_hdmiphy");
	if (IS_ERR_OR_NULL(res->sclk_hdmiphy)) {
		DRM_ERROR("failed to get clock 'sclk_hdmiphy'\n");
		goto fail;
	}
	res->hdmiphy = clk_get(dev, "hdmiphy");
	if (IS_ERR_OR_NULL(res->hdmiphy)) {
		DRM_ERROR("failed to get clock 'hdmiphy'\n");
		goto fail;
	}

	clk_set_parent(res->sclk_hdmi, res->sclk_pixel);

#ifndef CONFIG_ARCH_EXYNOS5
	res->regul_bulk = kzalloc(ARRAY_SIZE(supply) *
		sizeof res->regul_bulk[0], GFP_KERNEL);
	if (!res->regul_bulk) {
		DRM_ERROR("failed to get memory for regulators\n");
		goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(supply); ++i) {
		res->regul_bulk[i].supply = supply[i];
		res->regul_bulk[i].consumer = NULL;
	}
	ret = regulator_bulk_get(dev, ARRAY_SIZE(supply), res->regul_bulk);
	if (ret) {
		DRM_ERROR("failed to get regulators\n");
		goto fail;
	}
	res->regul_count = ARRAY_SIZE(supply);
#endif
	/* TODO:
	 * These clocks also should be added in
	 * runtime resume and runtime suspend
	 */
	clk_enable(res->hdmi);
	clk_enable(res->sclk_hdmi);

	return 0;
fail:
	DRM_ERROR("HDMI resource init - failed\n");
	return -ENODEV;
}

static int hdmi_resources_cleanup(struct hdmi_context *hdata)
{
	struct hdmi_resources *res = &hdata->res;

	regulator_bulk_free(res->regul_count, res->regul_bulk);
	/* kfree is NULL-safe */
	kfree(res->regul_bulk);
	if (!IS_ERR_OR_NULL(res->hdmiphy))
		clk_put(res->hdmiphy);
	if (!IS_ERR_OR_NULL(res->sclk_hdmiphy))
		clk_put(res->sclk_hdmiphy);
	if (!IS_ERR_OR_NULL(res->sclk_pixel))
		clk_put(res->sclk_pixel);
	if (!IS_ERR_OR_NULL(res->sclk_hdmi))
		clk_put(res->sclk_hdmi);
	if (!IS_ERR_OR_NULL(res->hdmi))
		clk_put(res->hdmi);
	memset(res, 0, sizeof *res);

	return 0;
}

struct platform_device *hdmi_audio_device;

int hdmi_register_audio_device(struct platform_device *pdev)
{
	struct hdmi_context *hdata = platform_get_drvdata(pdev);
	struct platform_device *audio_dev;
	int ret;

	DRM_DEBUG_KMS("[PDEV:%s]\n", pdev->name);

	audio_dev = platform_device_alloc("exynos-hdmi-audio", -1);
	if (!audio_dev) {
		DRM_ERROR("hdmi audio device allocation failed.\n");
		ret = -ENOMEM;
		goto err;
	}

	ret = platform_device_add_resources(audio_dev, pdev->resource,
			pdev->num_resources);
	if (ret) {
		ret = -ENOMEM;
		goto err_device;
	}

	audio_dev->dev.of_node = of_get_next_child(pdev->dev.of_node, NULL);
	audio_dev->dev.platform_data = (void *)hdata->hpd_gpio;

	ret = platform_device_add(audio_dev);
	if (ret) {
		DRM_ERROR("hdmi audio device add failed.\n");
		goto err_device;
	}

	hdmi_audio_device = audio_dev;
	return 0;

err_device:
	platform_device_put(audio_dev);

err:
	return ret;
}

void hdmi_unregister_audio_device(void)
{
	DRM_DEBUG_KMS("\n");
	platform_device_unregister(hdmi_audio_device);
}

static int __devinit hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hdmi_context *hdata;
	struct exynos_drm_hdmi_pdata *pdata;
	struct resource *res;
	struct device_node *ddc_node, *phy_node;
	int ret;
	enum of_gpio_flags flags;

	DRM_DEBUG_KMS("[PDEV:%s]\n", pdev->name);

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		DRM_ERROR("no platform data specified\n");
		return -EINVAL;
	}

	hdata = kzalloc(sizeof(struct hdmi_context), GFP_KERNEL);
	if (!hdata) {
		DRM_ERROR("out of memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, hdata);

	hdata->is_v13 = pdata->is_v13;
	hdata->default_win = pdata->default_win;
	hdata->default_timing = &pdata->timing;
	hdata->default_bpp = pdata->bpp;
	hdata->dev = dev;
	hdata->is_soc_exynos5 = of_device_is_compatible(dev->of_node,
		"samsung,exynos5-hdmi");

	ret = hdmi_resources_init(hdata);
	if (ret) {
		ret = -EINVAL;
		goto err_data;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		DRM_ERROR("failed to find registers\n");
		ret = -ENOENT;
		goto err_resource;
	}

	hdata->regs_res = request_mem_region(res->start, resource_size(res),
					   dev_name(dev));
	if (!hdata->regs_res) {
		DRM_ERROR("failed to claim register region\n");
		ret = -ENOENT;
		goto err_resource;
	}

	hdata->regs = ioremap(res->start, resource_size(res));
	if (!hdata->regs) {
		DRM_ERROR("failed to map registers\n");
		ret = -ENXIO;
		goto err_req_region;
	}

	/* DDC i2c driver */
	ddc_node = of_find_node_by_name(NULL, "exynos_ddc");
	if (!ddc_node) {
		DRM_ERROR("Failed to find ddc node in device tree\n");
		ret = -ENODEV;
		goto err_iomap;
	}
	hdata->ddc_port = of_find_i2c_device_by_node(ddc_node);
	if (!hdata->ddc_port) {
		DRM_ERROR("Failed to get ddc i2c client by node\n");
		ret = -ENODEV;
		goto err_iomap;
	}

	/* hdmiphy i2c driver */
	phy_node = of_find_node_by_name(NULL, "exynos_hdmiphy");
	if (!phy_node) {
		DRM_ERROR("Failed to find hdmiphy node in device tree\n");
		ret = -ENODEV;
		goto err_ddc;
	}
	hdata->hdmiphy_port = of_find_i2c_device_by_node(phy_node);
	if (!hdata->hdmiphy_port) {
		DRM_ERROR("Failed to get hdmi phy i2c client from node\n");
		ret = -ENODEV;
		goto err_ddc;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		DRM_ERROR("get interrupt resource failed.\n");
		ret = -ENXIO;
		goto err_hdmiphy;
	}

	hdata->internal_irq = res->start;

	hdata->hpd_gpio = of_get_named_gpio_flags(dev->of_node,
				"hpd-gpio", 0, &flags);

	if (!gpio_is_valid(hdata->hpd_gpio)) {
		DRM_ERROR("failed to get hpd gpio.");
		ret = -EINVAL;
		goto err_hdmiphy;
	}

	hdata->external_irq = gpio_to_irq(hdata->hpd_gpio);

	/* create workqueue and hotplug work */
	hdata->wq = alloc_workqueue("exynos-drm-hdmi",
			WQ_UNBOUND | WQ_NON_REENTRANT, 1);
	if (hdata->wq == NULL) {
		DRM_ERROR("Failed to create workqueue.\n");
		ret = -ENOMEM;
		goto err_hdmiphy;
	}
	INIT_WORK(&hdata->hotplug_work, hdmi_hotplug_func);

	ret = request_irq(hdata->internal_irq, hdmi_irq_handler,
			IRQF_SHARED, "int_hdmi", hdata);
	if (ret) {
		DRM_ERROR("request int interrupt failed.\n");
		goto err_workqueue;
	}
	disable_irq(hdata->internal_irq);

	ret = request_irq(hdata->external_irq, hdmi_irq_handler,
		IRQ_TYPE_EDGE_BOTH | IRQF_SHARED, "ext_hdmi",
		hdata);
	if (ret) {
		DRM_ERROR("request ext interrupt failed.\n");
		goto err_int_irq;
	}
	disable_irq(hdata->external_irq);

	if (of_device_is_compatible(dev->of_node,
		"samsung,exynos5-hdmi")) {
		ret = hdmi_register_audio_device(pdev);
		if (ret) {
			DRM_ERROR("hdmi-audio device registering failed.\n");
			goto err_ext_irq;
		}
	}

	hdmi_resource_poweron(hdata);

	if (!hdmi_is_connected(hdata)) {
		hdmi_resource_poweroff(hdata);
		DRM_DEBUG_KMS("gpio state is low. powering off!\n");
	}

	exynos_display_attach_panel(EXYNOS_DRM_DISPLAY_TYPE_MIXER, &hdmi_ops,
			hdata);

	return 0;

err_ext_irq:
	free_irq(hdata->external_irq, hdata);
err_int_irq:
	free_irq(hdata->internal_irq, hdata);
 err_workqueue:
	destroy_workqueue(hdata->wq);
err_hdmiphy:
	put_device(&hdata->hdmiphy_port->dev);
err_ddc:
	put_device(&hdata->ddc_port->dev);
err_iomap:
	iounmap(hdata->regs);
err_req_region:
	release_mem_region(hdata->regs_res->start,
			resource_size(hdata->regs_res));
err_resource:
	hdmi_resources_cleanup(hdata);
err_data:
	kfree(hdata);
	return ret;
}

static int __devexit hdmi_remove(struct platform_device *pdev)
{
	struct hdmi_context *hdata = platform_get_drvdata(pdev);
	struct hdmi_resources *res = &hdata->res;

	DRM_DEBUG_KMS("[PDEV:%s]\n", pdev->name);

	hdmi_resource_poweroff(hdata);

	hdmi_unregister_audio_device();

	disable_irq(hdata->curr_irq);
	free_irq(hdata->internal_irq, hdata);
	free_irq(hdata->external_irq, hdata);

	cancel_work_sync(&hdata->hotplug_work);
	destroy_workqueue(hdata->wq);

	clk_disable(res->hdmi);
	clk_disable(res->sclk_hdmi);
	hdmi_resources_cleanup(hdata);

	iounmap(hdata->regs);

	release_mem_region(hdata->regs_res->start,
			resource_size(hdata->regs_res));

	put_device(&hdata->hdmiphy_port->dev);
	put_device(&hdata->ddc_port->dev);

	kfree(hdata);

	return 0;
}

struct platform_driver hdmi_driver = {
	.probe		= hdmi_probe,
	.remove		= __devexit_p(hdmi_remove),
	.driver		= {
#ifdef CONFIG_ARCH_EXYNOS5
		.name	= "exynos5-hdmi",
#else
		.name   = "exynos4-hdmi",
#endif
		.owner	= THIS_MODULE,
	},
};
