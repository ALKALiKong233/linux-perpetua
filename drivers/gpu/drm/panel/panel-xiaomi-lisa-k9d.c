// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi Lisa K9D VTDR6130 AMOLED panel
 *
 * The initialization sequence and timings are derived from Xiaomi's
 * downstream dsi-panel-k9d-36-02-0a-dsc-cmd device tree.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct lisa_k9d_panel {
	struct drm_panel panel;
	struct drm_connector *connector;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data supplies[3];
	struct gpio_desc *reset_gpio;
};

struct lisa_k9d_cmd {
	u8 delay_ms;
	u8 len;
	u8 payload[44];
};

#define K9D_CMD(...) { \
	.len = sizeof((u8[]){ __VA_ARGS__ }), \
	.payload = { __VA_ARGS__ } \
}
static const struct lisa_k9d_cmd lisa_k9d_on_cmds[] = {
	K9D_CMD(0xbe, 0x0e, 0x0b, 0x14, 0x13),
	K9D_CMD(0x6f, 0x05),
	K9D_CMD(0xbe, 0x8a),
	K9D_CMD(0xc0, 0x66),
	K9D_CMD(0x6f, 0x08),
	K9D_CMD(0xb5, 0x32),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x07),
	K9D_CMD(0xc0, 0x01),
	K9D_CMD(0xc1, 0x30, 0x0f, 0x04, 0xc9, 0x0f, 0x81, 0xee, 0xc6, 0x3f, 0xfb, 0xb3, 0x6a, 0x3f, 0xf6, 0xd1, 0x42, 0x80, 0x00, 0xf7, 0x33, 0xb1, 0x00, 0x18, 0x00, 0x00, 0x8b, 0x23, 0x33, 0xc0, 0x0f, 0xb9, 0x0f, 0xdd, 0x8d, 0x00, 0x00, 0x00, 0x0d, 0x08, 0x00, 0x17, 0x23, 0x00),
	K9D_CMD(0xc2, 0x38, 0x0f, 0x0b, 0x64, 0x02, 0x11, 0xf6, 0x4c, 0x3f, 0xfa, 0xe2, 0x14, 0xff, 0xfe, 0x41, 0xa8, 0x00, 0x00, 0x5e, 0x26, 0x90, 0x00, 0x00, 0x24, 0x00, 0x17, 0x90, 0x33, 0xc0, 0x09, 0xb4, 0x0f, 0x94, 0xe9, 0x00, 0x00, 0x90, 0x0d, 0x3c, 0x90, 0x17, 0x57, 0x00),
	K9D_CMD(0xc3, 0x3c, 0x00, 0x04, 0xc9, 0x0f, 0x81, 0x11, 0x3a, 0x3f, 0xf9, 0x58, 0x7c, 0x00, 0x04, 0xf1, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x18, 0x3c, 0x90, 0x8b, 0x5f, 0x33, 0x60, 0x00, 0x00, 0x0c, 0xdd, 0x73, 0x00, 0x00, 0x04, 0x20, 0x08, 0x04, 0x2a, 0x23, 0x00),
	K9D_CMD(0xc4, 0x3c, 0x00, 0x0b, 0x64, 0x02, 0x11, 0x09, 0xb4, 0x3f, 0xf6, 0xca, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0xcf, 0x90, 0x17, 0x3b, 0x33, 0xc0, 0x00, 0x00, 0x0c, 0x94, 0x17, 0x00, 0x00, 0x94, 0x20, 0x3c, 0x94, 0x2a, 0x57, 0x00),
	K9D_CMD(0xc5, 0x26, 0x00, 0x04, 0xc9, 0x0f, 0x81, 0x11, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xef, 0x14, 0x34, 0x80, 0x00, 0x00, 0x00, 0x00, 0x03, 0xac, 0x00, 0x04, 0x1f, 0x23, 0x33, 0xc0, 0x00, 0x00, 0x03, 0x23, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xc6, 0x2e, 0x00, 0x0b, 0x64, 0x02, 0x11, 0x09, 0xb4, 0x00, 0x03, 0x11, 0xf4, 0xff, 0xfd, 0x62, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x20, 0x24, 0x04, 0x37, 0x90, 0x33, 0xc0, 0x00, 0x00, 0x03, 0x6c, 0xe9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xc7, 0x2a, 0x0f, 0x04, 0xc9, 0x0f, 0x81, 0xee, 0xc6, 0x00, 0x02, 0x5a, 0xee, 0x00, 0x0c, 0xae, 0x86, 0x7f, 0xfd, 0xf9, 0xf3, 0x65, 0x93, 0xac, 0x3c, 0x94, 0x1f, 0x5f, 0x33, 0x6f, 0xf0, 0x47, 0x00, 0x23, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xc8, 0x2a, 0x0f, 0x0b, 0x64, 0x02, 0x11, 0xf6, 0x4c, 0x00, 0x07, 0x29, 0xe4, 0xc0, 0x00, 0xdf, 0x2c, 0x7f, 0xff, 0x43, 0xb2, 0xe0, 0x84, 0x20, 0xcf, 0x94, 0x37, 0x3b, 0x33, 0xcf, 0xf6, 0x4c, 0x00, 0x6c, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xc9, 0x27, 0x00, 0x03, 0xc1, 0x04, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xfe, 0xf8, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x24, 0x00, 0x84, 0x43, 0x33, 0x90, 0x00, 0x00, 0x03, 0x1f, 0xdf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xca, 0x21, 0x00, 0x03, 0xc1, 0x04, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x0f, 0xc0, 0x3f, 0xff, 0x08, 0x00, 0x00, 0x00, 0x0f, 0x04, 0x00, 0x00, 0x42, 0x24, 0x00, 0x62, 0x43, 0x33, 0x90, 0x03, 0xe0, 0x0f, 0xe1, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xcb, 0x2d, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x3f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x44, 0x00, 0x62, 0x64, 0x33, 0x60, 0x00, 0x00, 0x0c, 0xe0, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xcc, 0x2b, 0x00, 0x04, 0x00, 0x04, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xee, 0xfc, 0x00, 0x00, 0x63, 0x44, 0x00, 0x84, 0x64, 0x33, 0x6f, 0xfb, 0xe0, 0x00, 0x20, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xb4, 0xc0),
	K9D_CMD(0x6f, 0x01),
	K9D_CMD(0xb4, 0x00, 0x80, 0x80),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	K9D_CMD(0x6f, 0x01),
	K9D_CMD(0xd2, 0x00, 0x00, 0x11),
	K9D_CMD(0x6f, 0x06),
	K9D_CMD(0xd2, 0x05),
	K9D_CMD(0x6f, 0x0f),
	K9D_CMD(0xd2, 0x00),
	K9D_CMD(0x6f, 0x09),
	K9D_CMD(0xd2, 0x00, 0x00),
	K9D_CMD(0xce, 0x00),
	K9D_CMD(0xff, 0xaa, 0x55, 0xa5, 0x80),
	K9D_CMD(0x6f, 0x61),
	K9D_CMD(0xf3, 0x80),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	K9D_CMD(0xc0, 0x46),
	K9D_CMD(0xbe, 0x0e, 0x0b),
	K9D_CMD(0x6f, 0x05),
	K9D_CMD(0xbe, 0x88),
	K9D_CMD(0x6f, 0x08),
	K9D_CMD(0xb5, 0x32),
	K9D_CMD(0x6f, 0x0b),
	K9D_CMD(0xb5, 0x33, 0x23, 0x2b),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	K9D_CMD(0xd1, 0x07, 0x00, 0x04),
	K9D_CMD(0x3b, 0x00, 0x10, 0x00, 0x30),
	K9D_CMD(0xd9, 0xc8),
	K9D_CMD(0x90, 0x01),
	K9D_CMD(0x91, 0xab, 0x28, 0x00, 0x0c, 0xc2, 0x00, 0x03, 0x1c, 0x01, 0x7e, 0x00, 0x0f, 0x08, 0xbb, 0x04, 0x3d, 0x10, 0xf0),
	K9D_CMD(0x03, 0x01),
	K9D_CMD(0x51, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0x53, 0x20),
	K9D_CMD(0x35, 0x00),
	K9D_CMD(0x2a, 0x00, 0x00, 0x04, 0x37),
	K9D_CMD(0x2b, 0x00, 0x00, 0x09, 0x5f),
};

static const struct lisa_k9d_cmd lisa_k9d_post_refresh_cmds[] = {
	K9D_CMD(0xff, 0xaa, 0x55, 0xa5, 0x81),
	K9D_CMD(0x6f, 0x0f),
	K9D_CMD(0xfd, 0x01, 0x5a),
	K9D_CMD(0x6f, 0x04),
	K9D_CMD(0xfd, 0x5f),
	K9D_CMD(0x6f, 0x1a),
	K9D_CMD(0xfd, 0x5f),
	K9D_CMD(0x2c),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	K9D_CMD(0xca, 0x12, 0x00, 0x92, 0x02),
	K9D_CMD(0x6f, 0x02),
	K9D_CMD(0xec, 0x80, 0x10),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	K9D_CMD(0xcd, 0x05, 0x31),
	K9D_CMD(0x6f, 0x10),
	K9D_CMD(0xd8, 0x0c),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x05),
	K9D_CMD(0xb3, 0x86, 0x80),
	K9D_CMD(0xb5, 0x85, 0x81),
	K9D_CMD(0xb7, 0x85, 0x00, 0x00, 0x81),
	K9D_CMD(0xb8, 0x05, 0x00, 0x00, 0x81),
	K9D_CMD(0xec, 0x0d, 0x11),
	K9D_CMD(0x6f, 0x02),
	K9D_CMD(0xec, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00),
	K9D_CMD(0x6f, 0x08),
	K9D_CMD(0xb5, 0x32),
	K9D_CMD(0x6f, 0x0b),
	K9D_CMD(0xb5, 0x33, 0x23, 0x2b),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	K9D_CMD(0xce, 0x00),
	K9D_CMD(0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01),
	K9D_CMD(0xc3, 0x94, 0x01, 0x97, 0xd0, 0x22, 0x02, 0x00),
};

static inline struct lisa_k9d_panel *to_lisa_k9d_panel(struct drm_panel *panel)
{
	return container_of(panel, struct lisa_k9d_panel, panel);
}

static void lisa_k9d_reset(struct lisa_k9d_panel *ctx)
{
	/*
	 * reset-gpios is active-low. These logical values produce the
	 * downstream physical high-low-high reset sequence.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
}

static void lisa_k9d_run_cmds(struct mipi_dsi_multi_context *dsi_ctx,
			      const struct lisa_k9d_cmd *cmds, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const struct lisa_k9d_cmd *cmd = &cmds[i];

		mipi_dsi_dcs_write_buffer_multi(dsi_ctx, cmd->payload, cmd->len);
		if (cmd->delay_ms)
			mipi_dsi_msleep(dsi_ctx, cmd->delay_ms);
	}
}

static void lisa_k9d_run_init(struct mipi_dsi_multi_context *dsi_ctx,
			      unsigned int refresh)
{
	u8 timing = refresh == 60 ? 0x0c : 0x12;

	dsi_ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xba, 0x01, 0xe6, 0x00, 0x10,
				     0x00, 0x30, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x58);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_var_seq_multi(dsi_ctx, 0xb2, timing, timing);
	lisa_k9d_run_cmds(dsi_ctx, lisa_k9d_on_cmds,
			  ARRAY_SIZE(lisa_k9d_on_cmds));
	mipi_dsi_dcs_write_var_seq_multi(dsi_ctx, 0x2f, refresh == 60 ? 0x02 : 0x01);
	lisa_k9d_run_cmds(dsi_ctx, lisa_k9d_post_refresh_cmds,
			  ARRAY_SIZE(lisa_k9d_post_refresh_cmds));

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 50);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);
	mipi_dsi_usleep_range(dsi_ctx, 16000, 17000);
}

static unsigned int lisa_k9d_get_refresh(struct lisa_k9d_panel *ctx)
{
	struct drm_connector *connector = ctx->connector;

	/* The preferred mode is used before a connector state is available. */
	if (!connector || !connector->state || !connector->state->crtc)
		return 90;

	return drm_mode_vrefresh(&connector->state->crtc->state->mode);
}

static void lisa_k9d_power_off(struct lisa_k9d_panel *ctx)
{
	regulator_disable(ctx->supplies[2].consumer);
	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[1].consumer);
	usleep_range(4000, 5000);
	regulator_disable(ctx->supplies[0].consumer);
}

static int lisa_k9d_power_on(struct lisa_k9d_panel *ctx)
{
	int ret;

	/* K9D 36/02: VDDIO, then DVDD, then VCI. */
	ret = regulator_enable(ctx->supplies[0].consumer);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	ret = regulator_enable(ctx->supplies[1].consumer);
	if (ret)
		goto disable_vddio;
	usleep_range(5000, 6000);

	ret = regulator_enable(ctx->supplies[2].consumer);
	if (!ret)
		return 0;

	usleep_range(2000, 3000);
	regulator_disable(ctx->supplies[1].consumer);
disable_vddio:
	usleep_range(4000, 5000);
	regulator_disable(ctx->supplies[0].consumer);
	return ret;
}

static int lisa_k9d_prepare(struct drm_panel *panel)
{
	struct lisa_k9d_panel *ctx = to_lisa_k9d_panel(panel);
	struct drm_dsc_picture_parameter_set pps;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	unsigned int refresh = lisa_k9d_get_refresh(ctx);

	dsi_ctx.accum_err = lisa_k9d_power_on(ctx);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	lisa_k9d_reset(ctx);
	lisa_k9d_run_init(&dsi_ctx, refresh);

	/* Xiaomi's downstream driver sends the generated PPS after panel-on. */
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);
	mipi_dsi_msleep(&dsi_ctx, 28);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		lisa_k9d_power_off(ctx);
	}

	return dsi_ctx.accum_err;
}

static int lisa_k9d_disable(struct drm_panel *panel)
{
	struct lisa_k9d_panel *ctx = to_lisa_k9d_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 80);

	return dsi_ctx.accum_err;
}

static int lisa_k9d_unprepare(struct drm_panel *panel)
{
	struct lisa_k9d_panel *ctx = to_lisa_k9d_panel(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	lisa_k9d_power_off(ctx);

	return 0;
}

/*
 * Command-mode porches describe transfer overhead, not panel blanking.
 * With RGB888 transport and 8-bpc/8-bpp DSC, the compressed horizontal
 * total is 392. Both modes therefore request approximately 682 Mbit/s
 * per lane, matching the fixed downstream link rate.
 */
static const struct drm_display_mode lisa_k9d_modes[] = {
	{
		.clock = (1080 + 16 + 8 + 8) * (2400 + 810 + 4 + 8) * 90 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 16,
		.hsync_end = 1080 + 16 + 8,
		.htotal = 1080 + 16 + 8 + 8,
		.vdisplay = 2400,
		.vsync_start = 2400 + 810,
		.vsync_end = 2400 + 810 + 4,
		.vtotal = 2400 + 810 + 4 + 8,
		.width_mm = 68,
		.height_mm = 152,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	{
		.clock = (1080 + 16 + 8 + 8) * (2400 + 2421 + 4 + 8) * 60 / 1000,
		.hdisplay = 1080,
		.hsync_start = 1080 + 16,
		.hsync_end = 1080 + 16 + 8,
		.htotal = 1080 + 16 + 8 + 8,
		.vdisplay = 2400,
		.vsync_start = 2400 + 2421,
		.vsync_end = 2400 + 2421 + 4,
		.vtotal = 2400 + 2421 + 4 + 8,
		.width_mm = 68,
		.height_mm = 152,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int lisa_k9d_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct lisa_k9d_panel *ctx = to_lisa_k9d_panel(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;

	for (i = 0; i < ARRAY_SIZE(lisa_k9d_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &lisa_k9d_modes[i]);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = lisa_k9d_modes[0].width_mm;
	connector->display_info.height_mm = lisa_k9d_modes[0].height_mm;

	return ARRAY_SIZE(lisa_k9d_modes);
}

static const struct drm_panel_funcs lisa_k9d_panel_funcs = {
	.prepare = lisa_k9d_prepare,
	.unprepare = lisa_k9d_unprepare,
	.disable = lisa_k9d_disable,
	.get_modes = lisa_k9d_get_modes,
};

static int lisa_k9d_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops lisa_k9d_bl_ops = {
	.update_status = lisa_k9d_bl_update_status,
};

static struct backlight_device *
lisa_k9d_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2047,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &lisa_k9d_bl_ops, &props);
}

static int lisa_k9d_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lisa_k9d_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct lisa_k9d_panel, panel,
				   &lisa_k9d_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "dvdd";
	ctx->supplies[2].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ret = regulator_set_voltage(ctx->supplies[0].consumer, 1800000, 1800000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set VDDIO voltage\n");

	ret = regulator_set_voltage(ctx->supplies[2].consumer, 3000000, 3000000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set VCI voltage\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;
	ctx->panel.backlight = lisa_k9d_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 1080;
	ctx->dsc.slice_count = 1;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		dev_err(dev, "failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void lisa_k9d_remove(struct mipi_dsi_device *dsi)
{
	struct lisa_k9d_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id lisa_k9d_of_match[] = {
	{ .compatible = "xiaomi,lisa-k9d-vtdr6130" },
	{ }
};
MODULE_DEVICE_TABLE(of, lisa_k9d_of_match);

static struct mipi_dsi_driver lisa_k9d_driver = {
	.probe = lisa_k9d_probe,
	.remove = lisa_k9d_remove,
	.driver = {
		.name = "panel-xiaomi-lisa-k9d",
		.of_match_table = lisa_k9d_of_match,
	},
};
module_mipi_dsi_driver(lisa_k9d_driver);

MODULE_DESCRIPTION("Xiaomi Lisa K9D VTDR6130 command-mode panel");
MODULE_LICENSE("GPL");
