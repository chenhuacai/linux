#include <drm/drm_atomic_helper.h>
#include <linux/delay.h>
#include <loonggpu.h>
#include "loonggpu_helper.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_dc_plane.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_hdmi.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_backlight.h"
#include "loonggpu_helper.h"
#include "loonggpu_dc_dp.h"
#include "loonggpu_bpipe.h"
#include "bridge_phy.h"

static unsigned int
cal_freq(unsigned int pixclock_khz, struct pixel_clock *pll_config)
{
	unsigned int pstdiv, loopc, frefc;
	unsigned long a, b, c;
	unsigned long min = 50;

	for (pstdiv = 63; pstdiv >= 1; pstdiv--) {
		a = (unsigned long)pixclock_khz * pstdiv;
		for (frefc = 5; frefc >= 3; frefc--) {
			for (loopc = 160; loopc >= 24; loopc--) {
				if ((loopc < 12 * frefc) ||
				    (loopc > 32 * frefc))
					continue;

				b = 100000L * loopc / frefc;
				c = (a > b) ? (a * 10000 / b - 10000) :
					(b * 10000 / a - 10000);
				if (c < min) {
					min = c;
					pll_config->l2_div = pstdiv;
					pll_config->l1_loopc = loopc;
					pll_config->l1_frefc = frefc;
				}
			}
		}
	}

	if (min < 50)
		return 1;

	return 0;
}

static u32 dc_io_rreg(void *base, u32 offset)
{
	return readl(base + offset);
}

static void dc_io_wreg(void *base, u32 offset, u32 val)
{
	writel(val, base + offset);
}

static void dc_crtc_reset(struct loonggpu_device *adev, int link)
{
	int retry_count = 0;
	u32 value;

	DRM_INFO("Reset crtc-%d\n", link);

retry:
	retry_count++;

	value = dc_readl(adev, gdc_reg->crtc_reg[link].cfg);
	value &= ~(CRTC_CFG_RESET | CRTC_CFG_ENABLE);
	dc_writel(adev, gdc_reg->crtc_reg[link].cfg, value);
	mdelay(10);

	value |= CRTC_CFG_RESET | CRTC_CFG_ENABLE;
	dc_writel(adev, gdc_reg->crtc_reg[link].cfg, value);
	mdelay(30);

	/* check buffer overflow */
	value = dc_readl(adev, gdc_reg->crtc_reg[link].cfg);

	if (retry_count > 1000) {
		DRM_INFO("Can not reset crtc-%d !!\n", link);
		return;
	}

	if(value & 0x1000000)
		goto retry;
}

static bool ls7a2000_pix_pll_set(struct loonggpu_device *adev, u32 clock,
				 u32 link, unsigned long pll_reg)
{
	u32 val;
	u32 count = 0;
	struct pixel_clock pll_cfg = {0};
	int connector_type;
	void __iomem *io_base = adev->io_base;

	if (io_base == NULL)
		return false;

	connector_type = adev->mode_info.connectors[link]->base.connector_type;

	cal_freq(clock, &pll_cfg);

	/* clear sel_pll_out0 */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val &= ~(1UL << 8);
	dc_io_wreg(io_base, pll_reg + 0x4, val);

	/* set pll_pd */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val |= (1UL << 13);
	dc_io_wreg(io_base, pll_reg + 0x4, val);

	/* clear set_pll_param */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val &= ~(1UL << 11);
	dc_io_wreg(io_base, pll_reg + 0x4, val);

	/* clear old value & config new value */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val &= ~(0x7fUL << 0);
	val |= (pll_cfg.l1_frefc << 0); /* refc */
	dc_io_wreg(io_base, pll_reg + 0x4, val);
	val = dc_io_rreg(io_base, pll_reg + 0x0);
	val &= ~(0x7fUL << 0);
	val |= (pll_cfg.l2_div << 0); /* div */
	val &= ~(0x1ffUL << 21);
	val |= (pll_cfg.l1_loopc << 21); /* loopc */
	dc_io_wreg(io_base, pll_reg + 0x0, val);

	/* set set_pll_param */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val |= (1UL << 11);
	dc_io_wreg(io_base, pll_reg + 0x4, val);
	/* clear pll_pd */
	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val &= ~(1UL << 13);
	dc_io_wreg(io_base, pll_reg + 0x4, val);

	while (!(dc_io_rreg(io_base, pll_reg + 0x4) & 0x80)) {
		cpu_relax();
		count++;
		if (count >= 1000) {
			DRM_ERROR("loongson-7A PLL lock failed\n");
			return false;
		}
		schedule_timeout(usecs_to_jiffies(100));
	}

	if ((connector_type == DRM_MODE_CONNECTOR_VGA) && !link)
		usleep_range(10000, 20000);

	val = dc_io_rreg(io_base, pll_reg + 0x4);
	val |= (1UL << 8);
	dc_io_wreg(io_base, pll_reg + 0x4, val);

	return true;
}

bool ls7a2000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	u32 link;

	if (IS_ERR_OR_NULL(crtc))
		return false;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	dc_interface_enable(crtc, false);
	if (ls7a2000_pix_pll_set(adev, timing->clock, link,
				 0x10 * link + DC_IO_PIX_PLL) == false)
		return false;

	dc_interface_pll_set(crtc, timing);
	dc_interface_enable(crtc, true);
	return true;
}

bool ls2k3000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing)
{
	u32 link;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	dc_interface_pll_set(crtc, timing);
	return true;
}

void ls2k2000_dc_crtc_cfg_adjust(u32 array_mode, u32 *crtc_cfg)
{
	switch (array_mode) {
	case 0:
		*crtc_cfg &= CRTC_CFG_TILE8_DISABLE;
		*crtc_cfg &= CRTC_CFG_ZIP_DISABLE;
		break;
	case 2:
		*crtc_cfg |= CRTC_CFG_TILE4x4;
		*crtc_cfg &= CRTC_CFG_TILE8_DISABLE;
		*crtc_cfg |= CRTC_CFG_ZIP_ENABLE;
		break;
	default:
		break;
	}
}

bool dc_crtc_timing_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_connector * aconnector;
	struct loonggpu_dc *dc = adev->dc;
	u32 depth;
	u32 link;
	u32 value;
	u32 stride_reg, cur_stride_reg;
	u32 crtc_cfg, cur_crtc_cfg;
	u32 hdisplay, cur_hdisplay;
	u32 hsync, cur_hsync;
	u32 vdisplay, cur_vdisplay;
	u32 vsync, cur_vsync;
	u32 panel_cfg, cur_panel_cfg;
	u32 fixed_vsync_start = 0;
	u32 fixed_vsync_end = 0;

	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(timing))
		return false;

	DRM_DEBUG_DRIVER("crtc %d timing set: clock %d, stride %d\n",
		crtc->resource->base.link, timing->clock, timing->stride);
	DRM_DEBUG_DRIVER("hdisplay %d, hsync_start %d, hsync_end %d, htotal %d\n",
		timing->hdisplay, timing->hsync_start, timing->hsync_end, timing->htotal);
	DRM_DEBUG_DRIVER("vdisplay %d, vsync_start %d, vsync_end %d, vtotal %d\n",
		timing->vdisplay, timing->vsync_start, timing->vsync_end, timing->vtotal);
	DRM_DEBUG_DRIVER("depth %d, use_dma %d\n", timing->depth, timing->use_dma);

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	crtc_cfg = CRTC_CFG_RESET;

	switch (crtc->array_mode) {
	case 0:
		crtc_cfg &= CRTC_CFG_LINEAR;
		crtc_cfg &= CRTC_CFG_TILE8_DISABLE;
		stride_reg = timing->stride;
		break;
	case 2:
		crtc_cfg |= CRTC_CFG_TILE4x4;
		crtc_cfg &= CRTC_CFG_TILE8_DISABLE;
		stride_reg = timing->stride * 4;
		break;
	case 4:
		crtc_cfg &= CRTC_CFG_LINEAR;
		crtc_cfg |= CRTC_CFG_TILE8_ENABLE;
		stride_reg = timing->stride * 8;
		break;
	}

	aconnector = adev->mode_info.connectors[link];
	if (dc->hw_ops->crtc_cfg_adjust)
		dc->hw_ops->crtc_cfg_adjust(crtc->array_mode, &crtc_cfg);

	crtc_cfg &= ~CRTC_CFG_DMA_MASK;
	if (timing->use_dma)
		crtc_cfg |= timing->use_dma;
	else if (!(timing->hdisplay % 64))
		crtc_cfg |= CRTC_CFG_DMA_256;
	else if (!(timing->hdisplay % 32))
		crtc_cfg |= CRTC_CFG_DMA_128;
	else if (!(timing->hdisplay % 16))
		crtc_cfg |= CRTC_CFG_DMA_64;
	else if (!(timing->hdisplay % 8))
		crtc_cfg |= CRTC_CFG_DMA_32;

	crtc_cfg &= ((~CRTC_CFG_FORMAT_MASK) << 0);
	depth = timing->depth;
	switch (depth) {
	case 12:
		crtc_cfg |= (DC_FB_FORMAT12 & CRTC_CFG_FORMAT_MASK);
		break;
	case 15:
		crtc_cfg |= (DC_FB_FORMAT15 & CRTC_CFG_FORMAT_MASK);
		break;
	case 16:
		crtc_cfg |= (DC_FB_FORMAT16 & CRTC_CFG_FORMAT_MASK);
		break;
	case 32:
	case 24:
	default:
		crtc_cfg |= (DC_FB_FORMAT32 & CRTC_CFG_FORMAT_MASK);
		break;
	}

	crtc_cfg = crtc_cfg | CRTC_CFG_ENABLE;

	hdisplay = ((timing->hdisplay & CRTC_HPIXEL_MASK) << CRTC_HPIXEL_SHIFT);
	hdisplay |= ((timing->htotal & CRTC_HTOTAL_MASK) << CRTC_HTOTAL_SHIFT);

	hsync = CRTC_HSYNC_POLSE;
	hsync |= ((timing->hsync_start & CRTC_HSYNC_START_MASK) << CRTC_HSYNC_START_SHIFT);
	hsync |= ((timing->hsync_end & CRTC_HSYNC_END_MASK) << CRTC_HSYNC_END_SHIFT);

	vdisplay = ((timing->vdisplay & CRTC_VPIXEL_MASK) << CRTC_VPIXEL_SHIFT);
	vdisplay |= ((timing->vtotal & CRTC_VTOTAL_MASK) << CRTC_VTOTAL_SHIFT);

	vsync = CRTC_VSYNC_POLSE;
	vsync |= ((timing->vsync_start  & CRTC_VSYNC_START_MASK) << CRTC_VSYNC_START_SHIFT);
	vsync |= ((timing->vsync_end & CRTC_VSYNC_END_MASK) << CRTC_VSYNC_END_SHIFT);

	panel_cfg = CRTC_PANCFG_BASE | CRTC_PANCFG_DE | CRTC_PANCFG_CLKEN;
	if (loonggpu_panel_cfg_clk_pol != -1)
		panel_cfg |= (loonggpu_panel_cfg_clk_pol & 1) << 9;
	else
		panel_cfg |= CRTC_PANCFG_CLKPOL;

	if (loonggpu_panel_cfg_de_pol != -1)
		panel_cfg |= (loonggpu_panel_cfg_de_pol & 1) << 1;

	cur_stride_reg = dc_readl(adev, gdc_reg->crtc_reg[link].stride);
	cur_hdisplay = dc_readl(adev, gdc_reg->crtc_reg[link].hdisplay);
	cur_hsync = dc_readl(adev, gdc_reg->crtc_reg[link].hsync);
	cur_vdisplay = dc_readl(adev, gdc_reg->crtc_reg[link].vdisplay);
	cur_vsync = dc_readl(adev, gdc_reg->crtc_reg[link].vsync);
	cur_panel_cfg = dc_readl(adev, gdc_reg->crtc_reg[link].panelcfg);
	cur_crtc_cfg = dc_readl(adev, gdc_reg->crtc_reg[link].cfg);
	cur_crtc_cfg &= CRTC_CFG_MASK;

	if (cur_stride_reg == stride_reg &&
		cur_hdisplay == hdisplay &&
		cur_hsync == hsync &&
		cur_vdisplay == vdisplay &&
		cur_crtc_cfg == crtc_cfg &&
		crtc->timing->clock == timing->clock &&
		crtc->timing->vrefresh == timing->vrefresh)
		return false;

	value = dc_readl(adev, gdc_reg->crtc_reg[link].cfg);
	value &= CRTC_CFG_MASK;
	dc_writel_check(adev, link, gdc_reg->crtc_reg[link].cfg,
			value & (~CRTC_CFG_ENABLE));
	dc_writel(adev, gdc_reg->crtc_reg[link].stride, stride_reg);
	dc_writel(adev, gdc_reg->crtc_reg[link].hdisplay, hdisplay);
	dc_writel(adev, gdc_reg->crtc_reg[link].hsync, hsync);
	dc_writel(adev, gdc_reg->crtc_reg[link].vdisplay, vdisplay);
	dc_writel(adev, gdc_reg->crtc_reg[link].vsync, vsync);
	dc_writel(adev, gdc_reg->crtc_reg[link].panelcfg, panel_cfg);
	dc_writel(adev, gdc_reg->crtc_reg[link].paneltim, 0);

	if (cur_hdisplay != hdisplay ||
		cur_vdisplay != vdisplay ||
		cur_hsync != hsync ||
		crtc->timing->vsync_start != timing->vsync_start ||
		crtc->timing->vsync_end != timing->vsync_end ||
		crtc->timing->clock != timing->clock ||
		crtc->timing->vrefresh != timing->vrefresh) {
		if (dc->hw_ops->dc_pll_set(crtc, timing))
			memcpy(crtc->timing, timing, sizeof(struct dc_timing_info));
	}

	/*
		Some 2K3000 laptops and cloud terminal devices exhibit screen flickering.
		This is resolved by adjusting the vsync_end value. This workaround is
		limited to EDP interfaces.
	*/
	if (crtc->timing->fixed_vsync_end && !link) {
		fixed_vsync_start = crtc->timing->fixed_vsync_start;
		fixed_vsync_end = crtc->timing->fixed_vsync_end;
		vsync = (vsync & ~(0xFFF)) | ((fixed_vsync_start & 0xFFF));
		vsync = (vsync & ~(0xFFF << 16)) | ((fixed_vsync_end & 0xFFF) << 16);
		dc_writel(adev, gdc_reg->crtc_reg[link].vsync, vsync);
	}

	dc_interface_audio_set(aconnector, crtc);
	dc_writel_check(adev, link, gdc_reg->crtc_reg[link].cfg, crtc_cfg);
	/*
		To address display corruption (offset) issues on certain HP and PanSheng monitors
		when using HDMI, implement a software workaround for this hardware-specific problem.
	*/

	is_special_display(adev, aconnector, link, crtc->timing);

	value = dc_readl(adev, gdc_reg->crtc_reg[link].cfg);
	if (value & VRAM_OVERFLOW && adev->chip == dev_7a1000)
		dc_crtc_reset(adev, link);

	return true;
}

bool dc_crtc_enable(struct loonggpu_dc_crtc *dc_crtc, bool enable)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	struct loonggpu_backlight *ls_bl;
	u32 crtc_cfg, crtc_pan;
	u32 hsync_val, vsync_val;
	u32 crtc_id;
	struct loonggpu_connector *lconnector;
	struct drm_connector *connector;
	int connector_type;

	if (IS_ERR_OR_NULL(dc_crtc))
		return false;

	crtc_id = dc_crtc->resource->base.link;
	if (crtc_id >= DC_DVO_MAXLINK)
		return false;

	crtc_cfg = dc_readl(adev, gdc_reg->crtc_reg[crtc_id].cfg);
	crtc_cfg &= CRTC_CFG_MASK;
	if (enable && (crtc_cfg & CRTC_CFG_ENABLE))
		return true;

	if (!enable && (!(crtc_cfg & CRTC_CFG_ENABLE)))
		return true;

	crtc_pan = dc_readl(adev, gdc_reg->crtc_reg[crtc_id].panelcfg);
	hsync_val = dc_readl(adev, gdc_reg->crtc_reg[crtc_id].hsync);
	vsync_val = dc_readl(adev, gdc_reg->crtc_reg[crtc_id].vsync);


	if (enable) {
		crtc_cfg |= CRTC_CFG_ENABLE;
		crtc_pan |= CRTC_PANCFG_DE;
		crtc_pan |= CRTC_PANCFG_CLKEN;
		hsync_val |= CRTC_HSYNC_POLSE;
		vsync_val |= CRTC_VSYNC_POLSE;

		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].panelcfg, crtc_pan);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].hsync, hsync_val);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].vsync, vsync_val);
		dc_writel_check(adev, crtc_id,
				gdc_reg->crtc_reg[crtc_id].cfg, crtc_cfg);
	} else {
		crtc_cfg &= ~CRTC_CFG_ENABLE;
		crtc_pan &= ~CRTC_PANCFG_DE;
		crtc_pan &= ~CRTC_PANCFG_CLKEN;
		hsync_val &= ~CRTC_HSYNC_POLSE;
		vsync_val &= ~CRTC_VSYNC_POLSE;

		dc_writel_check(adev, crtc_id,
				gdc_reg->crtc_reg[crtc_id].cfg, crtc_cfg);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].panelcfg, crtc_pan);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].hsync, hsync_val);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_id].vsync, vsync_val);
	}

	ls_bl = adev->mode_info.backlights[crtc_id];
	connector_type = adev->mode_info.connectors[crtc_id]->base.connector_type;
	if (ls_bl && ls_bl->power)
		ls_bl->power(ls_bl, enable);
	else if (backlight_device_get_by_type(BACKLIGHT_PLATFORM) &&
		(connector_type == DRM_MODE_CONNECTOR_eDP ||
		 connector_type == DRM_MODE_CONNECTOR_LVDS)) {
		if (enable)
			lg_turn_on_lvds();
		else
			lg_turn_off_lvds();
	}

	return true;
}

bool set_screen_open(struct loonggpu_dc_crtc *dc_crtc)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	struct loonggpu_connector *lconnector;
	struct loonggpu_backlight *ls_bl;
	struct drm_connector *connector;
	int connector_type;
	u32 crtc_cfg;
	u32 crtc_id;
	int i;

	if (adev->family_type == CHIP_LG210)
		return true;

	if (IS_ERR_OR_NULL(dc_crtc))
		return false;

	crtc_id = dc_crtc->resource->base.link;
	if (crtc_id >= DC_DVO_MAXLINK)
		return false;

	/* This function is called frequently, so it is advisable to
	 * avoid performing the same task again. */
	crtc_cfg = dc_readl(adev, gdc_reg->crtc_reg[crtc_id].cfg);
	crtc_cfg &= CRTC_CFG_MASK;
	if (crtc_cfg & CRTC_CFG_ENABLE)
		return true;

	connector_type = adev->mode_info.connectors[crtc_id]->base.connector_type;
	ls_bl = adev->mode_info.backlights[crtc_id];
	if (ls_bl && ls_bl->power)
		ls_bl->power(ls_bl, true);
	else if (backlight_device_get_by_type(BACKLIGHT_PLATFORM) &&
		(connector_type == DRM_MODE_CONNECTOR_eDP ||
		 connector_type == DRM_MODE_CONNECTOR_LVDS)) {
		lg_turn_on_lvds();
	}

	return true;
}

u32 dc_vblank_get_counter(struct loonggpu_device *adev, int crtc_num)
{
	if (crtc_num >= adev->mode_info.num_crtc)
		return 0;

	return adev->dc->hw_ops->dc_vblank_get_counter(adev, crtc_num);
}

int dc_crtc_get_scanoutpos(struct loonggpu_device *adev, int crtc_num,
				  u32 *vbl, u32 *position)
{
	struct loonggpu_dc *dc = adev->dc;
	uint32_t v_blank_start, v_blank_end, h_position, v_position;
	int reg_val = 0;

	if (IS_ERR_OR_NULL(dc) || (crtc_num >= dc->links))
		return false;

	if (IS_ERR_OR_NULL(dc->link_info[crtc_num].crtc))
		return false;

	if ((crtc_num < 0) || (crtc_num >= adev->mode_info.num_crtc))
		return -EINVAL;
	else {
		reg_val = dc_readl(adev, gdc_reg->crtc_reg[crtc_num].display_pos);

		v_blank_start = 0;
		v_blank_end = 0;
		h_position = (reg_val & 0xffff);
		v_position = (reg_val >> 16);

		*position = 0;
		*vbl = 0;
	}

	return 0;
}

static bool dc_crtc_fb_address_update(struct loonggpu_dc_crtc *crtc,
				   union plane_address address)
{
	u32 link;
	struct loonggpu_device *adev;

	if (IS_ERR_OR_NULL(crtc))
		return false;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	adev = crtc->dc->adev;

	dc_writel(adev, gdc_reg->crtc_reg[link].fbaddr0_lo, address.low_part);
	dc_writel(adev, gdc_reg->crtc_reg[link].fbaddr1_lo, address.low_part);

	dc_writel(adev, gdc_reg->crtc_reg[link].fbaddr0_hi, address.high_part);
	dc_writel(adev, gdc_reg->crtc_reg[link].fbaddr1_hi, address.high_part);

	return true;
}

bool crtc_primary_plane_set(struct loonggpu_dc_crtc *crtc,
					 struct dc_primary_plane *primary)
{
	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(primary))
		return false;

	return dc_crtc_fb_address_update(crtc, primary->address);
}

bool dc_crtc_plane_update(struct loonggpu_dc_crtc *crtc, struct dc_plane_update *update)
{
	bool ret;
	struct loonggpu_dc *dc = crtc->dc;

	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(update))
		return false;

	switch (update->type) {
	case DC_PLANE_CURSOR:
		ret = dc->hw_ops->cursor_set(crtc, &update->cursor);
		break;
	case DC_PLANE_PRIMARY:
		ret = dc->hw_ops->crtc_plane_set(crtc, &update->primary);
		break;
	case DC_PLANE_OVERLAY:
	default:
		pr_err("%s 7A1000 not support overlay \n", __func__);
		ret = false;
		break;
	}

	return ret;
}

struct loonggpu_dc_crtc *dc_crtc_construct(struct loonggpu_dc *dc, struct crtc_resource *resource)
{
	struct loonggpu_dc_crtc *crtc;
	u32 link;

	if (IS_ERR_OR_NULL(dc) || IS_ERR_OR_NULL(resource))
		return NULL;

	crtc = kzalloc(sizeof(*crtc), GFP_KERNEL);
	crtc->timing = kzalloc(sizeof(struct dc_timing_info), GFP_KERNEL);

	if (IS_ERR_OR_NULL(crtc))
		return NULL;

	crtc->dc = dc;
	crtc->resource = resource;
	crtc->dc_video.dp_num = 0xf;
	crtc->dc_video.hdmi_num = 0xf;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	list_add_tail(&crtc->node, &dc->crtc_list);

	return crtc;
}

void dc_crtc_destroy(struct loonggpu_dc_crtc *crtc)
{
	if (IS_ERR_OR_NULL(crtc))
		return;

	list_del(&crtc->node);
	kfree(crtc->timing);
	kfree(crtc);
	crtc = NULL;
}

static int crtc_helper_atomic_check(struct drm_crtc *crtc,
				    struct lg_drm_atomic_state *state)
{
	return 0;
}

static bool loonggpu_crtc_helper_scanout_position(struct drm_crtc *crtc,
					bool in_vblank_irq, int *vpos,
					int *hpos, ktime_t *stime, ktime_t *etime,
					const struct drm_display_mode *mode)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;

	return loonggpu_display_get_crtc_scanoutpos(dev, pipe, 0, vpos, hpos, stime, etime, mode);
}

static const struct drm_crtc_helper_funcs loonggpu_dc_crtc_helper_funcs = {
	.atomic_check = crtc_helper_atomic_check,
	lg_get_scanout_position_setting(loonggpu_crtc_helper_scanout_position)
};

int dc_set_vblank(struct drm_crtc *crtc, bool enable)
{
	struct loonggpu_crtc *acrtc = to_loonggpu_crtc(crtc);
	struct loonggpu_device *adev = crtc->dev->dev_private;
	struct loonggpu_irq_src *irq_src = &adev->vsync_irq;

	if (irq_src->funcs && irq_src->funcs->set)
		return irq_src->funcs->set(adev, irq_src, acrtc->crtc_id, enable);
	return 0;
}

static int dc_enable_vblank(struct drm_crtc *crtc)
{
	struct loonggpu_device *adev = crtc->dev->dev_private;

	return adev->dc->hw_ops->crtc_set_vblank(crtc, true);
}

static void dc_disable_vblank(struct drm_crtc *crtc)
{
	struct loonggpu_device *adev = crtc->dev->dev_private;

	adev->dc->hw_ops->crtc_set_vblank(crtc, false);
}

static u32 loonggpu_get_vblank_counter_crtc(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	unsigned int pipe = crtc->index;

	return loonggpu_get_vblank_counter_kms(dev, pipe);
}

static int loonggpu_crtc_gamma_set(struct drm_crtc *crtc, u16 *red, u16 *green,
				    u16 *blue, uint32_t size,
				    struct drm_modeset_acquire_ctx *ctx)
{
	struct loonggpu_device *adev = crtc->dev->dev_private;

	return adev->dc->hw_ops->crtc_gamma_set(crtc, red, green, blue);
}

static void disp_work(struct work_struct *work)
{
	struct loonggpu_crtc *lcrtc = container_of(work,
						struct loonggpu_crtc, disp_work);
	struct drm_crtc *crtc = &lcrtc->base;
	struct loonggpu_device *adev = crtc->dev->dev_private;
	struct loonggpu_dc *dc = adev->dc;
	struct drm_display_mode *native_mode = &dc->native_mode;
	struct display_bo *disp_bo = dc->scanout_bo;
	struct dc_plane_update plane = {0};
	struct loonggpu_bo *bo = NULL;
	struct loonggpu_framebuffer *lfb = NULL;
	struct drm_framebuffer *fb = NULL, *drm_fb = NULL;
	struct loonggpu_ring *ring = &adev->bpipe.ring;
	struct bpipe_box sbox = {0};
	struct bpipe_box dbox = {0};
	struct bpipe_buffer sbo = {0};
	struct bpipe_buffer dbo = {0};
	unsigned long flags;
	uint64_t tiling_flags = 0;
	int array_mode;
	int align = 64;
	int x, y;
	int r;

	if (drm_drv_uses_atomic_modeset(crtc->dev)) {
		//  atomic
		if (crtc->primary->state) {
			fb = crtc->primary->state->fb;
			x = crtc->primary->state->crtc_x;
			y = crtc->primary->state->crtc_y;
		} else {
			fb = NULL;
			x = y = 0;
		}
	} else {
		//  legacy
		fb = crtc->primary->fb;
		x = crtc->x;
		y = crtc->y;
	}

	if (!fb)
		return;

	lfb = to_loonggpu_framebuffer(fb);
	bo = gem_to_loonggpu_bo(lfb->base.obj[0]);

	r = loonggpu_bo_reserve(bo, false);
	if (unlikely(r)) {
		if (r != -ERESTARTSYS)
			DRM_ERROR("Unable to reserve buffer: %d\n", r);
		return;
	}
	loonggpu_bo_get_tiling_flags(bo, &tiling_flags);
	loonggpu_bo_unreserve(bo);

	array_mode = LOONGGPU_TILING_GET(tiling_flags, ARRAY_MODE);

	/* The width of FB is used to calculate the DMA length when
		* the screen is rotated */
	mutex_lock(&crtc->dev->mode_config.fb_lock);
	drm_for_each_fb(drm_fb, crtc->dev) {
		struct loonggpu_framebuffer *afb = to_loonggpu_framebuffer(drm_fb);
		if (drm_fb->width < 480 || !strcmp(drm_fb->comm, "fbcon"))
			continue;
		if (x != 0 && (lfb != afb) && array_mode == 0) {
			if (!(drm_fb->width % 64)) {
				align = 64;
			} else if (!(drm_fb->width % 32)) {
				align = 32;
			} else
				DRM_INFO("Setting with unaligned fb width x: %d\n", drm_fb->width);
		}
	}
	mutex_unlock(&crtc->dev->mode_config.fb_lock);

	/* x is used to calculate the DMA length when the dual screen
		* is arranged horizontally */
	if (x != 0 && array_mode == 0) {
		if (!(x % 64) && align >= 64) {
			align = 64;
		} else if (!(x % 32) && align >= 32) {
			align = 32;
		} else
			DRM_INFO("Setting with unaligned screen x: %d\n", x);
	}

	sbox.x1 = 0;
	sbox.y1 = 0;
	sbox.x2 = crtc->mode.hdisplay;
	sbox.y2 = crtc->mode.vdisplay;

	sbo.width = crtc->mode.hdisplay;
	sbo.height = crtc->mode.vdisplay;
	sbo.pitch = lfb->base.pitches[0] / lfb->base.format->cpp[0];
	sbo.bpp = (lfb->base.format->cpp[0] << 3);

	r = bpipe_map_vram_buffer(bo, 0, lfb->base.obj[0]->size, 0, ring, &sbo.addr);
	if (r) {
		DRM_ERROR("%s : bpipe_map_vram_buffer src error\r\n", __FUNCTION__);
	}

	dbox.x1 = 0;
	dbox.y1 = 0;
	dbox.x2 = native_mode->hdisplay;
	dbox.y2 = native_mode->vdisplay;

	dbo.width = native_mode->hdisplay;
	dbo.height = native_mode->vdisplay;
	dbo.pitch = disp_bo->pitch / disp_bo->cpp;
	dbo.bpp = (disp_bo->cpp << 3);

	r = bpipe_map_vram_buffer(disp_bo->handle, 0, disp_bo->size, 1, ring, &dbo.addr);
	if (r) {
		DRM_ERROR("%s : bpipe_map_vram_buffer dst error\r\n", __FUNCTION__);
	}

	switch (array_mode) {
	case 2:
		y = (y + 3) & ~3;
		x = ALIGN(x, 8);
		sbo.addr = sbo.addr + y * lfb->base.pitches[0] + x * lfb->base.format->cpp[0] * 4;
		sbo.tiling = BPIPE_TILING_ARRAY_MODE_TILED4;
		dbo.tiling = BPIPE_TILING_ARRAY_MODE_TILED4;
		break;
	case 0:
	default:
		sbo.addr = sbo.addr + y * lfb->base.pitches[0] + ALIGN(x, align) * lfb->base.format->cpp[0];
		sbo.tiling = BPIPE_TILING_ARRAY_MODE_LINEAR;
		dbo.tiling = BPIPE_TILING_ARRAY_MODE_LINEAR;
		break;
	}

	r = bpipe_draw_cs_copy(ring, &sbox, &dbox, &sbo, &dbo);
	if (r) {
		DRM_ERROR("%s : bpipe_draw_cs_copy error\r\n", __FUNCTION__);
	}

	plane.type = DC_PLANE_PRIMARY;
	plane.primary.address.low_part = lower_32_bits(disp_bo->gpu_addr);
	plane.primary.address.high_part = upper_32_bits(disp_bo->gpu_addr);

	dc_submit_plane_update(adev->dc, lcrtc->crtc_id, &plane);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	lcrtc->disp_work_status = 0;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	return;
}

static const struct drm_crtc_funcs loonggpu_dc_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = dc_enable_vblank,
	.disable_vblank = dc_disable_vblank,
	.get_vblank_counter = loonggpu_get_vblank_counter_crtc,
	.gamma_set = loonggpu_crtc_gamma_set
};

int loonggpu_dc_crtc_init(struct loonggpu_device *adev,
		       struct drm_plane *plane, uint32_t crtc_index)
{
	struct loonggpu_crtc *acrtc = NULL;
	struct loonggpu_plane *cursor_plane = NULL;
	struct drm_plane *cursor = NULL;
	u32 crtc_pan;
	int res = -ENOMEM;

	/*
	 * The hardware cursor is broken on 7A2000 and on 2K3000 DC
	 * revision 0x10, do not create the cursor plane on them by
	 * default so that userspace uses a software one.  It can be
	 * forced back on with hw_cursor=1.
	 */
	if ((loonggpu_hw_cursor == -1 &&
	    !(adev->chip == dev_7a2000 ||
	     (adev->chip == dev_2k3000 && adev->dc_revision == 0x10))) ||
	     loonggpu_hw_cursor == 1) {
		cursor_plane = kzalloc(sizeof(*cursor_plane), GFP_KERNEL);
		if (!cursor_plane)
			goto fail;

		cursor_plane->base.type = DRM_PLANE_TYPE_CURSOR;
		res = loonggpu_dc_plane_init(adev, cursor_plane, 0);
		if (res)
			goto fail;

		cursor = &cursor_plane->base;
	}

	acrtc = kzalloc(sizeof(struct loonggpu_crtc), GFP_KERNEL);
	if (!acrtc) {
		res = -ENOMEM;
		goto fail;
	}

	res = drm_crtc_init_with_planes(adev->ddev, &acrtc->base, plane,
					cursor, &loonggpu_dc_crtc_funcs, NULL);
	if (res) {
		acrtc->crtc_id = -1;
		goto fail;
	}

	acrtc->crtc_id = crtc_index;

	drm_crtc_helper_add(&acrtc->base, &loonggpu_dc_crtc_helper_funcs);

	mutex_init(&acrtc->cursor_lock);
	acrtc->max_cursor_width = 64;
	acrtc->max_cursor_height = 64;

	acrtc->irq_source_vsync = DC_IRQ_TYPE_VSYNC + crtc_index;
	acrtc->base.enabled = false;

	INIT_WORK(&acrtc->disp_work, disp_work);

	adev->mode_info.crtcs[crtc_index] = acrtc;

	if (adev->chip != dev_9a1000) {
		crtc_pan = dc_readl(adev, gdc_reg->crtc_reg[crtc_index].panelcfg);
		dc_writel(adev, gdc_reg->crtc_reg[crtc_index].panelcfg, crtc_pan & (~CRTC_PANCFG_DE));
	} else
		drm_mode_crtc_set_gamma_size(&acrtc->base, 256);

	return 0;

fail:
	if (cursor)
		drm_plane_cleanup(cursor);
	kfree(cursor_plane);
	kfree(acrtc);
	return res;
}
