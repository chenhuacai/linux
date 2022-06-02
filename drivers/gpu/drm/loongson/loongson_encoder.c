// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Loongson Technology Co., Ltd.
 * Copyright (C) 2019 Lemote Inc.
 * Authors:
 *	Chen Zhu <zhuchen@loongson.cn>
 *	Yaling Fang <fangyaling@loongson.cn>
 *	Dandan Zhang <zhangdandan@loongson.cn>
 *	Huacai Chen <chenhc@lemote.com>
 *	Jiaxun Yang <jiaxun.yang@flygoat.com>
 */

#include <drm/drm_crtc_helper.h>
#include "loongson_drv.h"

/**
 * loongson_encoder_destroy
 *
 * @encoder: encoder object
 *
 * Clean up encoder resources
 */
static void loongson_encoder_destroy(struct drm_encoder *encoder)
{
	struct loongson_encoder *loongson_encoder = to_loongson_encoder(encoder);
	drm_encoder_cleanup(encoder);
	kfree(loongson_encoder);
}

static int loongson_encoder_atomic_check(struct drm_encoder *encoder,
				    struct drm_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state)
{
	return 0;
}

static void loongson_encoder_atomic_mode_set(struct drm_encoder *encoder,
				struct drm_crtc_state *crtc_state,
				struct drm_connector_state *conn_state)
{
	unsigned long flags;
	struct loongson_encoder *lenc = to_loongson_encoder(encoder);
	struct loongson_crtc *lcrtc_origin = lenc->lcrtc;
	struct loongson_crtc *lcrtc_current = to_loongson_crtc(crtc_state->crtc);

	if (lcrtc_origin->crtc_id != lcrtc_current->crtc_id)
		lcrtc_origin->cfg_reg |= CFG_PANELSWITCH;
	else
		lcrtc_origin->cfg_reg &= ~CFG_PANELSWITCH;

	spin_lock_irqsave(&loongson_reglock, flags);
	crtc_write(lcrtc_origin, FB_CFG_DVO_REG, lcrtc_origin->cfg_reg);
	spin_unlock_irqrestore(&loongson_reglock, flags);
}

/**
 * These provide the minimum set of functions required to handle a encoder
 *
 * Helper operations for encoders
 */
static const struct drm_encoder_helper_funcs loongson_encoder_helper_funcs = {
	.atomic_check = loongson_encoder_atomic_check,
	.atomic_mode_set = loongson_encoder_atomic_mode_set,
};

/**
 * These provide the minimum set of functions required to handle a encoder
 *
 * Encoder controls,encoder sit between CRTCs and connectors
 */
static const struct drm_encoder_funcs loongson_encoder_encoder_funcs = {
	.destroy = loongson_encoder_destroy,
};

static void loongson_hdmi_init(struct loongson_drm_device *ldev, int index)
{
	u32 val;
	int offset = index * 0x10;
	volatile void __iomem *base = ldev->mmio;

	spin_lock(&loongson_reglock);
	writel(0x287, base + HDMI_CTRL_REG + offset);

	writel(0x00400040, base + HDMI_ZONEIDLE_REG + offset);

	writel(6272, base + HDMI_AUDIO_NCFG_REG + offset);
	writel(0x80000000, base + HDMI_AUDIO_CTSCFG_REG + offset);

	writel(0x11, base + HDMI_AUDIO_INFOFRAME_REG + offset);
	val = readl(base + HDMI_AUDIO_INFOFRAME_REG + offset) | 0x4;
	writel(val, base + HDMI_AUDIO_INFOFRAME_REG + offset);

	writel(0x1, base + HDMI_AUDIO_SAMPLE_REG + offset);
	spin_unlock(&loongson_reglock);

	DRM_DEBUG("Loongson HDMI init finish.\n");
}

/**
 * loongson_encoder_init
 *
 * @dev: point to the drm_device structure
 *
 * Init encoder
 */
struct drm_encoder *loongson_encoder_init(struct drm_device *dev, unsigned int index)
{
	struct drm_encoder *encoder;
	struct loongson_encoder *loongson_encoder;
	struct loongson_drm_device *ldev = dev->dev_private;

	loongson_encoder = kzalloc(sizeof(struct loongson_encoder), GFP_KERNEL);
	if (!loongson_encoder)
		return NULL;

	loongson_encoder->encoder_id = index;
	loongson_encoder->i2c = &ldev->i2c_bus[index];
	loongson_encoder->lcrtc = &ldev->lcrtc[index];
	loongson_encoder->type = get_encoder_type(ldev, index);
	encoder = &loongson_encoder->base;

	if (loongson_encoder->type == DRM_MODE_ENCODER_TMDS)
		loongson_hdmi_init(ldev, index);

	encoder->possible_crtcs = BIT(index);
	encoder->possible_clones = BIT(1) | BIT(0);
	/* encoder->possible_crtcs = BIT(1) | BIT(0); */

	drm_encoder_helper_add(encoder, &loongson_encoder_helper_funcs);
	drm_encoder_init(dev, encoder, &loongson_encoder_encoder_funcs,
			 loongson_encoder->type, NULL);

	return encoder;
}
