// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based tk4c AMOLED panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "panel/panel-samsung-drv.h"
#include "gs_drm/gs_display_mode.h"


/* TODO: b/310801601#comment11 - Update with DSC v1.2a from complete OP code. */
/* PPS Setting DSC 1.1 SCR v4 */
static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 540,
	.slice_height = 101,
	.simple_422 = false,
	.pic_width = 1080,
	.pic_height = 2424,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 594,
	.block_pred_enable = true,
	.first_line_bpg_offset = 15,
	.initial_offset = 6144,
	.rc_buf_thresh = {
		14, 28, 42, 56,
		70, 84, 98, 105,
		112, 119, 121, 123,
		125, 126
	},
	.rc_range_params = {
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2},
		{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0},
		{.range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58},
		{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56},
		{.range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 10, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
		{.range_min_qp = 9, .range_max_qp = 12, .range_bpg_offset = 52},
		{.range_min_qp = 12, .range_max_qp = 13, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 7,
	.scale_increment_interval = 2241,
	.nfl_bpg_offset = 308,
	.slice_bpg_offset = 258,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.dsc_version_minor = 1,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define TK4C_WRCTRLD_DIMMING_BIT    0x08
#define TK4C_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x2F };
static const u8 pixel_off[] = { 0x22 };

static const struct exynos_dsi_cmd tk4c_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_DELAY(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(tk4c_off);

static const struct exynos_dsi_cmd tk4c_lp_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_EXYNOS_CMD_SET(tk4c_lp);

static const struct exynos_dsi_cmd tk4c_lp_2nits_cmd[] = {
	EXYNOS_DSI_CMD_SEQ(0x51, 0x00, 0xB8),
};

static const struct exynos_dsi_cmd tk4c_lp_10nits_cmd[] = {
	EXYNOS_DSI_CMD_SEQ(0x51, 0x01, 0x7E),
};
static const struct exynos_dsi_cmd tk4c_lp_50nits_cmd[] = {
	EXYNOS_DSI_CMD_SEQ(0x51, 0x03, 0x1A),
};

static const struct exynos_binned_lp tk4c_binned_lp[] = {
	/* 0 to 8 nits */
	BINNED_LP_MODE_TIMING("night", 360, tk4c_lp_2nits_cmd,
				12, 12 + 50),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 716, tk4c_lp_10nits_cmd,
				12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 4095, tk4c_lp_50nits_cmd,
				12, 12 + 50),
};

static const struct exynos_dsi_cmd tk4c_init_cmds[] = {
	/* TE on */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* TE width setting */
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x51),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x08, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x09, 0x4B, 0x00, 0x00, 0x0B, 0x00, 0x09, 0x90, 0x00, 0x09, 0x90),
	EXYNOS_DSI_CMD0(test_key_disable),

	/* TE2 width setting */
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x01, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x51),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x26, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x3C, 0x00, 0x09, 0x90, 0x00, 0x09, 0x90),
	EXYNOS_DSI_CMD0(test_key_disable),

	/* CASET: 1080 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* TODO: b/310801601#comment11 - FFC settings with complete OP code */
};
static DEFINE_EXYNOS_CMD_SET(tk4c_init);

/**
 * struct tk4c_panel - panel specific runtime info
 *
 * This struct maintains tk4c panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc
 */
struct tk4c_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct tk4c_panel, base)

static void tk4c_change_frequency(struct exynos_panel *ctx,
									const struct exynos_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (unlikely(!ctx))
		return;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(ctx->dev, "%s: invalid refresh rate %uhz\n", __func__, vrefresh);
		return;
	}

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0x83, vrefresh == 60 ? 0x08 : 0x00);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_info(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void tk4c_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = TK4C_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= TK4C_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s)\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int tk4c_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct tk4c_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode && ctx->current_mode->exynos_mode.is_lp_mode) {
		const struct exynos_panel_funcs *funcs;

		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}
		funcs = ctx->desc->exynos_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_TABLE(ctx, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(ctx->dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	if (!ctx->desc->brt_capability) {
		dev_err(ctx->dev, "no available brightness capability\n");
		return -EINVAL;
	}

	max_brightness = ctx->desc->brt_capability->hbm.level.max;

	if (br > max_brightness) {
		br = max_brightness;
		dev_warn(ctx->dev, "%s: capped to dbv(%d)\n", __func__,
			max_brightness);
	}

	brightness = (br & 0xFF) << 8 | br >> 8;

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void tk4c_set_hbm_mode(struct exynos_panel *ctx,
				enum exynos_hbm_mode mode)
{
	ctx->hbm_mode = mode;

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	if (ctx->hbm_mode) {
		/* TODO: b/310801601#comment11 - Update FGZ mode settings with complete OP code */
		// EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x61, 0x68);
		// EXYNOS_DCS_BUF_ADD(ctx, 0x68, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX);
	}

	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);

	if (ctx->hbm_mode) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x2E, 0xBD);
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x01);
	} else {
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x01);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x2E, 0xBD);
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x02);
	}

	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d.\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tk4c_set_dimming_on(struct exynos_panel *exynos_panel,
                                bool dimming_on)
{
	const struct exynos_panel_mode *pmode = exynos_panel->current_mode;

	exynos_panel->dimming_on = dimming_on;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(exynos_panel->dev, "in lp mode, skip to update dimming usage\n");
		return;
	}

	tk4c_update_wrctrld(exynos_panel);
}

static void tk4c_mode_set(struct exynos_panel *ctx,
                          const struct exynos_panel_mode *pmode)
{
	tk4c_change_frequency(ctx, pmode);
}

static bool tk4c_is_mode_seamless(const struct exynos_panel *ctx,
                                  const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void tk4c_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
#ifdef CONFIG_DEBUG_FS
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct dentry *panel_root, *csroot;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &tk4c_init_cmd_set, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void tk4c_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	exynos_panel_get_panel_rev(ctx, rev);
}

static void tk4c_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	if (!is_panel_active(ctx))
		return;

	/* AOD Mode Off Setting */
	tk4c_update_wrctrld(ctx);
	tk4c_change_frequency(ctx, pmode);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int tk4c_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct drm_dsc_picture_parameter_set pps_payload;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s\n", __func__);

	exynos_panel_reset(ctx);

	/* sleep out */
	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	/* initial command */
	exynos_panel_send_cmd_set(ctx, &tk4c_init_cmd_set);

	/* frequency */
	tk4c_change_frequency(ctx, pmode);

	/* DSC related configuration */
	exynos_dcs_compression_mode(ctx, 0x1);
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	/* DSC Enable */
	EXYNOS_DCS_BUF_ADD(ctx, 0x9D, 0x01);

	/* dimming and HBM */
	tk4c_update_wrctrld(ctx);

	/* display on */
	if (pmode->exynos_mode.is_lp_mode)
		exynos_panel_set_lp_mode(ctx, pmode);

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int tk4c_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tk4c_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_pixel_off = false;

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 500,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1080, VDISPLAY = 2424;
static const u16 HFP = 32, HSA = 12, HBP = 16;
static const u16 VFP = 12, VSA = 4, VBP = 15;

#define TK4C_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.slice_count = 2,\
	.slice_height = 101,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode tk4c_modes[] = {
	{
		.mode = {
			.name = "1080x2424@60:60",
			DRM_MODE_TIMING(60, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
			/* aligned to bootloader setting */
			.type = DRM_MODE_TYPE_PREFERRED,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 8450,
			.bpc = 8,
			.dsc = TK4C_DSC,
			.underrun_param = &underrun_param,
		},
	},
	{
		.mode = {
			.name = "1080x2424@120:120",
			DRM_MODE_TIMING(120, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 276,
			.bpc = 8,
			.dsc = TK4C_DSC,
			.underrun_param = &underrun_param,
		},
	},
};

const struct brightness_capability tk4c_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1250,
		},
		.level = {
			.min = 184,
			.max = 3427,
		},
		.percentage = {
			.min = 0,
			.max = 68,
		},
	},
	.hbm = {
		.nits = {
			.min = 1250,
			.max = 1850,
		},
		.level = {
			.min = 3428,
			.max = 4095,
		},
		.percentage = {
			.min = 68,
			.max = 100,
		},
	},
};

static const struct exynos_panel_mode tk4c_lp_mode = {
	.mode = {
		.name = "1080x2424@30:30",
		DRM_MODE_TIMING(30, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
		.width_mm = WIDTH_MM,
		.height_mm = HEIGHT_MM,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.te_usec = 1109,
		.bpc = 8,
		.dsc = TK4C_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static const struct drm_panel_funcs tk4c_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = tk4c_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = tk4c_debugfs_init,
};

static const struct exynos_panel_funcs tk4c_exynos_funcs = {
	.set_brightness = tk4c_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = tk4c_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_dimming_on = tk4c_set_dimming_on,
	.set_hbm_mode = tk4c_set_hbm_mode,
	.is_mode_seamless = tk4c_is_mode_seamless,
	.mode_set = tk4c_mode_set,
	.get_panel_rev = tk4c_get_panel_rev,
	.read_id = exynos_panel_read_ddic_id,
};

const struct exynos_panel_desc google_tk4c = {
	.data_lane_cnt = 4,
	.max_brightness = 4095,
	.min_brightness = 2,
	.dft_brightness = 1268,    /* 140 nits */
	.brt_capability = &tk4c_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = tk4c_modes,
	.num_modes = ARRAY_SIZE(tk4c_modes),
	.off_cmd_set = &tk4c_off_cmd_set,
	.lp_mode = &tk4c_lp_mode,
	.lp_cmd_set = &tk4c_lp_cmd_set,
	.binned_lp = tk4c_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tk4c_binned_lp),
	.panel_func = &tk4c_drm_funcs,
	.exynos_panel_func = &tk4c_exynos_funcs,
	.reset_timing_ms = {1, 1, 5},
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
};

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,tk4c", .data = &google_tk4c },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = tk4c_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-tk4c",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Safayat Ullah <safayat@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4c panel driver");
MODULE_LICENSE("GPL");
