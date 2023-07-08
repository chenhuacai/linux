/*
 * Copyright (c) 2018 Loongson Technology Co., Ltd.
 * Authors:
 *	Chen Zhu <zhuchen@loongson.cn>
 *	Yaling Fang <fangyaling@loongson.cn>
 *	Dandan Zhang <zhangdandan@loongson.cn>
 *	Huacai Chen <chenhc@lemote.com>
 *	Jiaxun Yang <jiaxun.yang@flygoat.com>
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/delay.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include "loongson_drv.h"

/**
 * This file contains setup code for the CRTC
 */

DEFINE_SPINLOCK(loongson_crtc_lock);

static int loongson_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct loongson_crtc *lcrtc = to_loongson_crtc(crtc);
	struct loongson_drm_device *ldev = lcrtc->ldev;

	if(lcrtc->crtc_id == 0) {
		ldev->int_reg |= (BIT(INT_DVO0_FB_END) << 16);
	} else {
		ldev->int_reg |= (BIT(INT_DVO1_FB_END) << 16);
	}

	spin_lock(&loongson_reglock);
	writel(ldev->int_reg, ldev->mmio + FB_INT_REG);
	spin_unlock(&loongson_reglock);

	return 0;
}

static void loongson_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct loongson_crtc *lcrtc = to_loongson_crtc(crtc);
	struct loongson_drm_device *ldev = lcrtc->ldev;


	if(lcrtc->crtc_id == 0) {
		ldev->int_reg &= (~BIT(INT_DVO0_FB_END) << 16);
	} else {
		ldev->int_reg &= (~BIT(INT_DVO1_FB_END) << 16);
	}

	spin_lock(&loongson_reglock);
	writel(ldev->int_reg, ldev->mmio + FB_INT_REG);
	spin_unlock(&loongson_reglock);
}

#define PLL_REF_CLK_MHZ    100
#define PCLK_PRECISION_INDICATOR 10000

/**
 * cal_freq
 *
 * @pixclock: unsigned int
 * @pll_config: point to the pix_pll structure
 *
 * Calculate frequency
 */
static unsigned int cal_freq(unsigned int pixclock, struct pix_pll *pll_config)
{
	int i, j, loopc_offset;
	unsigned int refc_set[] = {4, 5, 3};
	unsigned int prec_set[] = {1, 5, 10, 50, 100};   /*in 1/PCLK_PRECISION_INDICATOR*/
	unsigned int pstdiv, loopc, refc;
	unsigned int precision_req, precision;
	unsigned int loopc_min, loopc_max, loopc_mid;
	unsigned long long real_dvo, req_dvo;

	/*try precision from high to low*/
	for (j = 0; j < sizeof(prec_set)/sizeof(int); j++){
		precision_req = prec_set[j];

		/*try each refc*/
		for (i = 0; i < sizeof(refc_set)/sizeof(int); i++) {
			refc = refc_set[i];
			loopc_min = (1200 / PLL_REF_CLK_MHZ) * refc;  /*1200 / (PLL_REF_CLK_MHZ / refc)*/
			loopc_max = (3200 / PLL_REF_CLK_MHZ) * refc;  /*3200 / (PLL_REF_CLK_MHZ / refc)*/
			loopc_mid = (2200 / PLL_REF_CLK_MHZ) * refc;  /*(loopc_min + loopc_max) / 2;*/
			loopc_offset = -1;

			/*try each loopc*/
			for (loopc = loopc_mid; (loopc <= loopc_max) && (loopc >= loopc_min); loopc += loopc_offset) {
				if(loopc_offset < 0)
					loopc_offset = -loopc_offset;
				else
					loopc_offset = -(loopc_offset+1);

				pstdiv = loopc * PLL_REF_CLK_MHZ * 1000 / refc / pixclock;
				if((pstdiv > 127) || (pstdiv < 1))
					continue;

				/*real_freq is float type which is not available, but read_freq * pstdiv is available.*/
				req_dvo  = (pixclock * pstdiv);
				real_dvo = (loopc * PLL_REF_CLK_MHZ * 1000 / refc);
				precision = abs(real_dvo * PCLK_PRECISION_INDICATOR / req_dvo - PCLK_PRECISION_INDICATOR);

				if(precision < precision_req){
					pll_config->l2_div = pstdiv;
					pll_config->l1_loopc = loopc;
					pll_config->l1_frefc = refc;
					if(j > 1)
						printk("Warning: PIX clock precision degraded to %d / %d\n", precision_req, PCLK_PRECISION_INDICATOR);
					return 1;
				}
			}
		}
	}
	return 0;
}

/**
 * config_pll
 *
 * @pll_base: represent a long type
 * @pll_cfg: point to the pix_pll srtucture
 *
 * Config pll apply to ls7a
 */
static void config_pll(void *pll_base, struct pix_pll *pll_cfg)
{
	unsigned long val;

	/* clear sel_pll_out0 */
	val = readl(pll_base + 0x4);
	val &= ~(1UL << 8);
	writel(val, pll_base + 0x4);
	/* set pll_pd */
	val = readl(pll_base + 0x4);
	val |= (1UL << 13);
	writel(val, pll_base + 0x4);
	/* clear set_pll_param */
	val = readl(pll_base + 0x4);
	val &= ~(1UL << 11);
	writel(val, pll_base + 0x4);
	/* clear old value & config new value */
	val = readl(pll_base + 0x4);
	val &= ~(0x7fUL << 0);
	val |= (pll_cfg->l1_frefc << 0); /* refc */
	writel(val, pll_base + 0x4);
	val = readl(pll_base + 0x0);
	val &= ~(0x7fUL << 0);
	val |= (pll_cfg->l2_div << 0);   /* div */
	val &= ~(0x1ffUL << 21);
	val |= (pll_cfg->l1_loopc << 21);/* loopc */
	writel(val, pll_base + 0x0);
	/* set set_pll_param */
	val = readl(pll_base + 0x4);
	val |= (1UL << 11);
	writel(val, pll_base + 0x4);
	/* clear pll_pd */
	val = readl(pll_base + 0x4);
	val &= ~(1UL << 13);
	writel(val, pll_base + 0x4);
	/* wait pll lock */
	while(!(readl(pll_base + 0x4) & 0x80))
		cpu_relax();
	/* set sel_pll_out0 */
	val = readl(pll_base + 0x4);
	val |= (1UL << 8);
	writel(val, pll_base + 0x4);
}

static void loongson_config_pll(int id, unsigned int pix_freq)
{
	unsigned int out;
	struct pix_pll pll_cfg;

	out = cal_freq(pix_freq, &pll_cfg);
	if (id == 0)
		config_pll(LS7A_PIX0_PLL, &pll_cfg);
	else
		config_pll(LS7A_PIX1_PLL, &pll_cfg);
}

/**
 * These provide the minimum set of functions required to handle a CRTC
 * Each driver is responsible for filling out this structure at startup time
 *
 * The drm_crtc_funcs structure is the central CRTC management structure
 * in the DRM. Each CRTC controls one or more connectors
 */
static const struct drm_crtc_funcs loongson_swcursor_crtc_funcs = {
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = loongson_crtc_enable_vblank,
	.disable_vblank = loongson_crtc_disable_vblank,
};

static const struct drm_crtc_funcs loongson_hwcursor_crtc_funcs = {
	.cursor_set2 = loongson_crtc_cursor_set2,
	.cursor_move = loongson_crtc_cursor_move,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = loongson_crtc_enable_vblank,
	.disable_vblank = loongson_crtc_disable_vblank,
};

static const uint32_t loongson_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const uint64_t loongson_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static enum drm_mode_status loongson_crtc_mode_valid(struct drm_crtc *crtc,
						    const struct drm_display_mode *mode)
{
	int id = crtc->index;
	struct drm_device *dev = crtc->dev;
	struct loongson_drm_device *ldev = (struct loongson_drm_device*)dev->dev_private;

	if (mode->hdisplay > get_crtc_max_width(ldev, id))
		return MODE_BAD;
	if (mode->vdisplay > get_crtc_max_height(ldev, id))
		return MODE_BAD;
	if (ldev->num_crtc == 1) {
		if (mode->hdisplay % 16)
			return MODE_BAD;
	} else {
		if (mode->hdisplay % 64)
			return MODE_BAD;
	}

	return MODE_OK;
}

u32 crtc_read(struct loongson_crtc *lcrtc, u32 offset)
{
	struct loongson_drm_device *ldev = lcrtc->ldev;
	return readl(ldev->mmio + offset + (lcrtc->crtc_id * CRTC_REG_OFFSET));
}

void crtc_write(struct loongson_crtc *lcrtc, u32 offset, u32 val)
{
	struct loongson_drm_device *ldev = lcrtc->ldev;
	writel(val, ldev->mmio + offset + (lcrtc->crtc_id * CRTC_REG_OFFSET));
}

static void loongson_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	unsigned int hr, hss, hse, hfl;
	unsigned int vr, vss, vse, vfl;
	unsigned int pix_freq;
	unsigned long flags;
	struct loongson_crtc *lcrtc = to_loongson_crtc(crtc);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;

	hr	= mode->hdisplay;
	hss	= mode->hsync_start;
	hse	= mode->hsync_end;
	hfl	= mode->htotal;

	vr	= mode->vdisplay;
	vss	= mode->vsync_start;
	vse	= mode->vsync_end;
	vfl	= mode->vtotal;

	pix_freq = mode->clock;

	DRM_DEBUG("crtc_id = %d, hr = %d, hss = %d, hse = %d, hfl = %d, vr = %d, vss = %d, vse = %d, vfl = %d, pix_freq = %d,\n",
			lcrtc->crtc_id, hr, hss, hse, hfl, vr, vss, vse, vfl, pix_freq);

	spin_lock_irqsave(&loongson_reglock, flags);
	crtc_write(lcrtc, FB_DITCFG_DVO_REG, 0);
	crtc_write(lcrtc, FB_DITTAB_LO_DVO_REG, 0);
	crtc_write(lcrtc, FB_DITTAB_HI_DVO_REG, 0);
	crtc_write(lcrtc, FB_PANCFG_DVO_REG, 0x80001311);
	crtc_write(lcrtc, FB_PANTIM_DVO_REG, 0);

	crtc_write(lcrtc, FB_HDISPLAY_DVO_REG, (mode->crtc_htotal << 16) | mode->crtc_hdisplay);
	crtc_write(lcrtc, FB_HSYNC_DVO_REG, 0x40000000 | (mode->crtc_hsync_end << 16) | mode->crtc_hsync_start);

	crtc_write(lcrtc, FB_VDISPLAY_DVO_REG, (mode->crtc_vtotal << 16) | mode->crtc_vdisplay);
	crtc_write(lcrtc, FB_VSYNC_DVO_REG, 0x40000000 | (mode->crtc_vsync_end << 16) | mode->crtc_vsync_start);

	crtc_write(lcrtc, FB_STRI_DVO_REG, (crtc->primary->state->fb->pitches[0] + 255) & ~255);

	DRM_DEBUG("Stride: %x\n",(crtc->primary->state->fb->pitches[0] + 255) & ~255);

	switch (crtc->primary->state->fb->format->format) {
	case DRM_FORMAT_RGB565:
		lcrtc->cfg_reg |= 0x3;
		crtc_write(lcrtc, FB_CFG_DVO_REG, lcrtc->cfg_reg);
		break;
	case DRM_FORMAT_RGB888:
	default:
		lcrtc->cfg_reg |= 0x4;
		crtc_write(lcrtc, FB_CFG_DVO_REG, lcrtc->cfg_reg);
		break;
	}
	spin_unlock_irqrestore(&loongson_reglock, flags);

	loongson_config_pll(lcrtc->crtc_id, mode->clock);
}

static void loongson_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	unsigned long flags;
	struct loongson_crtc *lcrtc = to_loongson_crtc(crtc);

	if (lcrtc->cfg_reg & CFG_ENABLE)
		goto vblank_on;

	lcrtc->cfg_reg |= CFG_ENABLE;
	spin_lock_irqsave(&loongson_reglock, flags);
	crtc_write(lcrtc, FB_CFG_DVO_REG, lcrtc->cfg_reg);
	spin_unlock_irqrestore(&loongson_reglock, flags);

vblank_on:
	drm_crtc_vblank_on(crtc);
}

static void loongson_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	unsigned long flags;
	struct loongson_crtc *lcrtc = to_loongson_crtc(crtc);

	lcrtc->cfg_reg &= ~CFG_ENABLE;
	spin_lock_irqsave(&loongson_reglock, flags);
	crtc_write(lcrtc, FB_CFG_DVO_REG, lcrtc->cfg_reg);
	spin_unlock_irqrestore(&loongson_reglock, flags);

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);

	drm_crtc_vblank_off(crtc);
}

static void loongson_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&crtc->dev->event_lock);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static void loongson_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	int id, clonemode;
	unsigned int pitch;
	unsigned long flags;
	struct loongson_crtc *lcrtc;
	struct loongson_drm_device *ldev;
	struct drm_plane_state *pstate = plane->state;

	if (!pstate->crtc || !pstate->fb)
		return;

	pitch = pstate->fb->pitches[0];
	lcrtc = to_loongson_crtc(pstate->crtc);
	ldev = lcrtc->ldev;
	id = lcrtc->crtc_id;
	clonemode = clone_mode(ldev);

	/* CRTC1 cloned from CRTC0 in clone mode */
	if (clonemode)
		ldev->lcrtc[1].cfg_reg |= CFG_PANELSWITCH;
	else
		ldev->lcrtc[1].cfg_reg &= ~CFG_PANELSWITCH;

	spin_lock_irqsave(&loongson_reglock, flags);
	crtc_write(lcrtc, FB_STRI_DVO_REG, (pitch + 255) & ~255);
	if (crtc_read(lcrtc, FB_CFG_DVO_REG) & CFG_FBNUM)
		crtc_write(lcrtc, FB_ADDR0_DVO_REG, drm_fb_dma_get_gem_addr(pstate->fb, pstate, 0));
	else
		crtc_write(lcrtc, FB_ADDR1_DVO_REG, drm_fb_dma_get_gem_addr(pstate->fb, pstate, 0));

	lcrtc->cfg_reg |= CFG_ENABLE;
	crtc_write(lcrtc, FB_CFG_DVO_REG, lcrtc->cfg_reg | CFG_FBSWITCH);
	if (clonemode) {
		if (id)
			crtc_write(&ldev->lcrtc[0], FB_CFG_DVO_REG, ldev->lcrtc[0].cfg_reg | CFG_ENABLE);
		else
			crtc_write(&ldev->lcrtc[1], FB_CFG_DVO_REG, ldev->lcrtc[1].cfg_reg | CFG_ENABLE);
	}
	spin_unlock_irqrestore(&loongson_reglock, flags);

	udelay(2500);
}

/**
 * These provide the minimum set of functions required to handle a CRTC
 *
 * The drm_crtc_helper_funcs is a helper operations for CRTC
 */
static const struct drm_crtc_helper_funcs loongson_crtc_helper_funcs = {
	.mode_valid = loongson_crtc_mode_valid,
	.mode_set_nofb	= loongson_crtc_mode_set_nofb,
	.atomic_enable	= loongson_crtc_atomic_enable,
	.atomic_disable	= loongson_crtc_atomic_disable,
	.atomic_flush	= loongson_crtc_atomic_flush,
};

static void loongson_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static bool loongson_format_mod_supported(struct drm_plane *plane,
					   uint32_t format, uint64_t modifier)
{
	return (modifier == DRM_FORMAT_MOD_LINEAR);
}

static const struct drm_plane_funcs loongson_plane_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.destroy = loongson_plane_destroy,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.update_plane = drm_atomic_helper_update_plane,
	.format_mod_supported = loongson_format_mod_supported,
};

static const struct drm_plane_helper_funcs loongson_plane_helper_funcs = {
	.atomic_update = loongson_plane_atomic_update,
};

/**
 * loongosn_crtc_init
 *
 * @ldev: point to the loongson_drm_device structure
 *
 * Init CRTC
 */
int loongson_crtc_init(struct loongson_drm_device *ldev)
{
	int i, ret;
	extern bool hw_cursor;

	for(i=0;i<ldev->num_crtc;i++){
		ldev->lcrtc[i].ldev = ldev;
		ldev->lcrtc[i].crtc_id = i;

		ldev->lcrtc[i].cfg_reg = CFG_RESET;
		ldev->lcrtc[i].primary = devm_kzalloc(ldev->dev->dev, sizeof(*ldev->lcrtc[i].primary), GFP_KERNEL);
		if (!ldev->lcrtc[i].primary)
			return -ENOMEM;

		ret = drm_universal_plane_init(ldev->dev, ldev->lcrtc[i].primary, BIT(i), &loongson_plane_funcs,
				       loongson_formats, ARRAY_SIZE(loongson_formats),
				       loongson_format_modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
		if (ret)
			return ret;

		drm_plane_helper_add(ldev->lcrtc[i].primary, &loongson_plane_helper_funcs);

		if (hw_cursor)
			ret = drm_crtc_init_with_planes(ldev->dev, &ldev->lcrtc[i].base,ldev->lcrtc[i].primary,
					NULL, &loongson_hwcursor_crtc_funcs, NULL);
		else
			ret = drm_crtc_init_with_planes(ldev->dev, &ldev->lcrtc[i].base,ldev->lcrtc[i].primary,
					NULL, &loongson_swcursor_crtc_funcs, NULL);
		if (ret) {
			loongson_plane_destroy(ldev->lcrtc[i].primary);
			return ret;
		}
		drm_crtc_helper_add(&ldev->lcrtc[i].base, &loongson_crtc_helper_funcs);
	}

	return 0;
}
