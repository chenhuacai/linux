#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/i2c.h>
#include <drm/display/drm_scdc_helper.h>
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_hdmi.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_helper.h"
#include "bridge_phy.h"

void ls7a1000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c)
{
	/* 7a1000 has no hdmi interface */
	return;
}


void ls7a2000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c)
{
	u32 value;
	struct loonggpu_device *adev = crtc->dc->adev;

	value = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
	if (use_gpio_i2c)
		value &= ~(1 << 8);
	else
		value |= (1 << 8);
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, value);
}

void ls7a2000_hdmi_pll_set(struct loonggpu_dc_crtc *crtc, int intf, int clock)
{
	int val;
	int count = 0;
	struct loonggpu_device *adev = crtc->dc->adev;

	/* 7A2000CB don't need config phy pll */
	if (adev->chip == dev_7a2000 && adev->dc_revision == 0x3)
		return;

	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].phy_pllcfg, 0x0);

	if (clock >= 170000)
		val = (0x0 << 13) | (0x28 << 6) | (0x10 << 1) | (0 << 0);
	else if (clock >= 85000  && clock < 170000)
		val = (0x1 << 13) | (0x28 << 6) | (0x8 << 1) | (0 << 0);
	else if (clock >= 42500 && clock < 85000)
		val = (0x2 << 13) | (0x28 << 6) | (0x4 << 1) | (0 << 0);
	else if (clock >= 21250 && clock < 42500)
		val = (0x3 << 13) | (0x28 << 6) | (0x2 << 1) | (0 << 0);

	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].phy_pllcfg, val);
	mdelay(5);
	val |= (1 << 0);
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].phy_pllcfg, val);

	/* wait pll lock */
	while (!(dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].phy_pllcfg) & 0x10000)) {
		count++;
		if (count >= 1000) {
			DRM_ERROR("LOONGGPU HDMI PHY PLL lock failed\n");
			return;
		}
	}
}

bool ls7a2000_hdmi_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable)
{
	u32 hdmi_ctrl, hdmi_phy;
	struct loonggpu_device *adev = crtc->dc->adev;

	if (IS_ERR_OR_NULL(adev))
		return false;

	if (intf >= MAX_DC_INTERFACES)
		return false;

	hdmi_phy = 0xf02;
	hdmi_phy |= dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].phy_ctrl);
	hdmi_ctrl = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
	if (enable && (hdmi_phy & HDMI_PHY_CTRL_ENABLE) && (hdmi_ctrl & HDMI_CTRL_ENABLE))
		return true;

	if (!enable && (!(hdmi_phy & HDMI_PHY_CTRL_ENABLE))&& (!(hdmi_ctrl & HDMI_CTRL_ENABLE)))
		return true;

	if (enable) {
		hdmi_phy |= HDMI_PHY_CTRL_ENABLE;
		hdmi_ctrl = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
		hdmi_ctrl |= HDMI_CTRL_ENABLE;
		dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, hdmi_ctrl);
	} else {
		hdmi_ctrl = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
		hdmi_ctrl &= ~HDMI_CTRL_ENABLE;
		dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, hdmi_ctrl);
		msleep(50);
		hdmi_phy &= ~HDMI_PHY_CTRL_ENABLE;
	}

	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].phy_ctrl, hdmi_phy);

	return true;
}

void ls7a2000_hdmi_suspend(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;

	crtc->hdmi_ctrl[intf] = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
}

int ls7a2000_hdmi_resume(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;

	if (crtc->intf[intf].connected) {
		dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, crtc->hdmi_ctrl[intf]);
		if (crtc->hdmi_ctrl[intf] & HDMI_CTRL_AUDIO_ENABLE) {
			dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].zoneidle, 0x00400040);
			dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_ncfg, 6272);
			dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_ctscfg, 0x80000000);
			dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_infoframe, 0x15);
			dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_sample, 0x1);
		}
	}

	return 0;
}

int ls7a2000_hdmi_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;

	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, 0x282);
	msleep(50);
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].phy_ctrl, 0xf02);
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].zoneidle, 0x00400040);

	return 0;
}

int ls7a2000_hdmi_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_connector *aconnector;
	u32 crtc_id;
	u32 val;

	crtc_id = crtc->resource->base.link;
	aconnector = adev->mode_info.connectors[crtc_id];
	val = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].ctrl);

	if (!is_special_hdmi_display(aconnector, crtc->timing))
		dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, 0x282 | (val & 1));

	return 0;
}

int ls7a2000_hdmi_audio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	u32 val;
	struct loonggpu_device *adev = crtc->dc->adev;

	val = dc_readl(adev, gdc_reg->hdmi_reg_v1[intf].ctrl);
	if (val & 0x4)
		return 0;

	/* enable hdmi audio, but don't touch EN bit which will be
	 * update in loonggpu_hdmi_enable() */
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].ctrl, 0x286 | (val & 0x1));

	/* Audio N 
	 * 44.1KHz * 4, dynamic update N && CTS value */
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_ncfg, 6272);

	/* Enable Send CTS */
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_ctscfg, 0x80000000);

	/* Audio AIF
	 * enable AIF,set freq,and set CC = 1, CA = 0
	 * Update AIF */
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_infoframe, 0x15);

	/* Audio Sample Packet */
	dc_writel(adev, gdc_reg->hdmi_reg_v1[intf].audio_sample, 0x1);

	return 0;
}

void ls2k3000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c)
{
	u32 value;
	struct loonggpu_device *adev = crtc->dc->adev;

	value = dc_readl(adev, gdc_reg->hdmi_reg_v2[0].ctrl);
	if (use_gpio_i2c)
		value &= ~(1 << 8);
	else
		value |= (1 << 8);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[0].ctrl, value);
}

#define PCLK_PRECISION_INDICATOR 10000
static struct mpll {
	uint32_t multiplier;
	uint32_t tx_clkdiv;
	uint32_t tx_rate;
	uint32_t tmds_clk_ratio;
} mpllcfg;

static uint32_t get_tx_clkdiv(void)
{
	uint32_t tx_clkdiv;

	switch (mpllcfg.tx_clkdiv) {
  	  case 1:
  	    tx_clkdiv = 0;
  	    break;
  	  case 2:
  	    tx_clkdiv = 1;
  	    break;
  	  case 3:
  	    tx_clkdiv = 4;
  	    break;
  	  case 4:
  	    tx_clkdiv = 2;
  	    break;
  	  case 5:
  	    tx_clkdiv = 5;
  	    break;
  	  case 6:
  	    tx_clkdiv = 6;
  	    break;
  	  case 8:
  	    tx_clkdiv = 3;
  	    break;
  	  case 10:
  	    tx_clkdiv = 7;
  	    break;
  	  default:
  	    tx_clkdiv = 0;
  	    break;
  	}

	return tx_clkdiv;
}

static uint32_t get_tx_rate(void)
{
	uint32_t tx_rate;

	switch (mpllcfg.tx_rate) {
  	  case 1:
  	    tx_rate = 0;
  	    break;
  	  case 2:
  	    tx_rate = 1;
  	    break;
  	  case 4:
  	    tx_rate = 2;
  	    break;
  	  case 8:
  	    tx_rate = 3;
  	    break;
  	  default:
  	    tx_rate = 0;
  	    break;
  	}

	return tx_rate;
}

static uint32_t hdmi_phy_cr_access(struct loonggpu_device *adev, uint32_t cr_wr_en, uint32_t cr_wdata, uint32_t cr_addr)
{
	uint32_t tmp;
	void __iomem *io_base = adev->io_base;
	int timeout;

	if (io_base == NULL)
		return false;

	writel(cr_wdata, io_base + 0x790);
	/* cr_wr_en 0: read, 1: write */
	writel(cr_addr | (cr_wr_en << 26) | (0x1 << 27), io_base + 0x794);

	/* clear en */
	tmp = readl(io_base + 0x794);
	writel(tmp & (~(0x1 << 27)), io_base + 0x794);

	/* busy */
	timeout = 1000;
	while ((readl(io_base + 0x79c) & (0x1 << 31)) == (0x1 << 31)) {
		if (--timeout == 0) {
			DRM_ERROR("HDMI PHY CR access timeout\n");
			break;
		}
		msleep(1);
	}

	if (cr_wr_en == 0)
		tmp = readl(io_base + 0x798) & 0xffff;
	else
		tmp = 0;

	return tmp;
}

static void dc_global_reg_config(struct loonggpu_device *adev, uint32_t *dc_global_reg_save, uint8_t flag)
{
	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1) /* 2K3000CA */
		return;

	if (flag == 1) {
		dc_global_reg_save[0] = dc_readl(adev, gdc_reg->global_reg.gpio_cfg_off);
		dc_writel(adev, gdc_reg->global_reg.gpio_cfg_off, 0xffff);
	} else if (flag == 0) {
		/* restore global reg value */
		dc_writel(adev, gdc_reg->global_reg.gpio_cfg_off, dc_global_reg_save[0]);
	}
}

static void configmpll(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	uint32_t tx_rate, tx_clkdiv, phy_clk, high_phy_clk, tmds_clk_ratio;
	u32 val;

	tx_clkdiv = get_tx_clkdiv();
	tx_rate   = get_tx_rate();
	phy_clk   = ((2 * mpllcfg.multiplier * 27) / mpllcfg.tx_clkdiv / 5 / mpllcfg.tx_rate) * 10;
	high_phy_clk = 3400;

	tmds_clk_ratio = ((phy_clk <= high_phy_clk) ? 0 : 1);
	mpllcfg.tmds_clk_ratio = tmds_clk_ratio;

	DRM_INFO( "[2K3000] PhyClk %d Mhz\n", phy_clk);
	DRM_INFO( "[2K3000] Multiplier %d\n", mpllcfg.multiplier);
	DRM_INFO( "[2K3000] TxClkDiv %d\n", mpllcfg.tx_clkdiv);
	DRM_INFO( "[2K3000] TxRate %d\n", mpllcfg.tx_rate);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg1, 0x33330000);

	/* phy/lane reset */
	val = 0xe0000100;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0,
		val & (~(0x3 << 30)));
	mdelay(10);

	/* deassert phy/lane reset */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0,
		val | (0x3 << 30));
	mdelay(10);

	/* dc global reg config save
	 *
	 * !!Notice
	 *
	 * This call is used to workaround hw bug.
	 *
	 * Any global reg read access with non-zero returned value will
	 * destroy hdmi phy registers, so the call save int reg and write
	 * it to zero and read back before hdmi phy configuration. After
	 * config done, restore saved int reg.
	 */

	/* ext ld done */
	val = 0x20000200;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg2, val & (~(0x1 << 29)));
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg2, val | (0x1 << 29));
	mdelay(10);

	DRM_INFO("hdmi phy boot up done...\n");
	hdmi_phy_cr_access(adev, 1, 0x1f, 0x4000);
	hdmi_phy_cr_access(adev, 1, mpllcfg.multiplier << 8, 0x4001);
	hdmi_phy_cr_access(adev, 1, (tx_clkdiv << 12) | 0x800, 0x4002);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x4003);
	hdmi_phy_cr_access(adev, 1, tx_rate << 11, 0x4006);
	hdmi_phy_cr_access(adev, 1, 0x226, 0x4042);
	hdmi_phy_cr_access(adev, 1, tmds_clk_ratio, 0x5011);
	hdmi_phy_cr_access(adev, 1, 0x718, 0x5000);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x5001);
	hdmi_phy_cr_access(adev, 1, tmds_clk_ratio, 0x5111);
	hdmi_phy_cr_access(adev, 1, 0x718, 0x5100);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x5101);
	hdmi_phy_cr_access(adev, 1, tmds_clk_ratio, 0x5211);
	hdmi_phy_cr_access(adev, 1, 0x718, 0x5200);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x5201);
	hdmi_phy_cr_access(adev, 1, tmds_clk_ratio, 0x5311);
	hdmi_phy_cr_access(adev, 1, 0x718, 0x5300);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x5301);
	hdmi_phy_cr_access(adev, 1, 0x5811, 0x4041);
	hdmi_phy_cr_access(adev, 1, 0x0, 0x4040);
	hdmi_phy_cr_access(adev, 1, 0x224, 0x4042);

	/* power up */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg1, 0);

	/* enable tx valid */
	val = 0xe0000100;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0, val & (~(0x1 << 29)));
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0, val | (0x1 << 29));


	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg1, 0x33330000);
	msleep(10);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg1, 0);

	DRM_INFO("hdmi mpll config done......\n");
}

static int ls2k3000_calc_phy_clk(unsigned int pixclock_khz)
{
	unsigned int tx_clkdiv_set[] = {10, 8, 6, 5, 4, 3, 2, 1};
	unsigned int tx_rate_set[] = {1, 2, 4, 8};
	unsigned int tx_rate, multiplier, tx_clkdiv;
	int i, j;
	unsigned int precision_req, precision;
	unsigned int multiplier_min, multiplier_max, multiplier_mid;
	unsigned long long real_dvo, req_dvo;
	unsigned int pixclock_mhz;
	int offset;

	pixclock_mhz = pixclock_khz / 1000;
	precision_req = ~0;
	multiplier_min = (4500 / 27) / 2;
	multiplier_max = (6500 / 27) / 2;
	multiplier_mid = (5500 / 27) / 2;
	for (i = 0; i < sizeof(tx_clkdiv_set) / sizeof(uint32_t); i++) {
		tx_clkdiv = tx_clkdiv_set[i];
		for (j = 0; j < (sizeof(tx_rate_set) / sizeof(uint32_t)); j++) {
			tx_rate = tx_rate_set[j];
			offset = 0;
			for (multiplier = multiplier_mid; (multiplier <= multiplier_max) && (multiplier >= multiplier_min); multiplier += offset) {
				if (offset < 0) {
					offset = -(offset - 1);
				} else {
					offset = -(offset + 1);
				}

				real_dvo = (2 * multiplier * 27) / tx_clkdiv / 5 / tx_rate;
				req_dvo  = pixclock_mhz;
				precision = abs(real_dvo * PCLK_PRECISION_INDICATOR / req_dvo - PCLK_PRECISION_INDICATOR);
				if (precision < precision_req &&
					real_dvo >= req_dvo) {
					precision_req = precision;
					mpllcfg.multiplier = multiplier;
					mpllcfg.tx_clkdiv = tx_clkdiv;
					mpllcfg.tx_rate   = tx_rate;
				}
			}
		}
		if (precision_req == 0) break;
	}
	DRM_INFO("for pixclock = %d mhz, found: multiplier = %d, "
			"tx_clkdiv = %d, tx_rate = %d, precision = %d / %d.\n", pixclock_mhz,
			mpllcfg.multiplier, mpllcfg.tx_clkdiv, mpllcfg.tx_rate, precision_req, PCLK_PRECISION_INDICATOR);
	return true;
}

#define FRL_TMDS_SCRAMBLE_EN	(0x1 << 4)

#define SCDC_TMDS_CONFIG		0x20
#define SCDC_TMDS_CONFIG_SCRAMBLE	0x3

static int ls2k3000_scdc_write(struct loonggpu_device *adev, int link,
			       u8 reg, u8 val)
{
	if (IS_ERR_OR_NULL(adev->i2c[link]))
		return -ENODEV;

	return drm_scdc_writeb(&adev->i2c[link]->adapter, reg, val);
}

/* Write the SCDC TMDS config register, retrying in case the sink is slow. */
static int ls2k3000_scdc_tmds_config(struct loonggpu_device *adev, int link,
				     u8 val)
{
	int ret = -ENODEV;
	int retry;

	for (retry = 0; retry < 5; retry++) {
		ret = ls2k3000_scdc_write(adev, link, SCDC_TMDS_CONFIG, val);
		if (!ret)
			break;
		msleep(100);
	}

	if (ret)
		DRM_INFO("SCDC write retry %d failed\n", retry);

	return ret;
}

/* Arm or disarm the DC side TMDS scrambler. */
static void ls2k3000_hdmi_tmds_scramble_enable(struct loonggpu_device *adev,
					       int intf, bool enable)
{
	u32 val = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].frl_cfg0);

	if (enable)
		val |= FRL_TMDS_SCRAMBLE_EN;
	else
		val &= ~FRL_TMDS_SCRAMBLE_EN;

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].frl_cfg0, val);
}

static void ls2k3000_hdmi_set_tmds_scramble(struct loonggpu_dc_crtc *crtc,
					    int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	int link = crtc->resource->base.link;
	u32 ctrl_reg = gdc_reg->hdmi_reg_v2[intf].ctrl;
	u32 ctrl;

	ctrl = dc_readl(adev, ctrl_reg);

	/* stop the output during the transition */
	dc_writel(adev, ctrl_reg, ctrl & ~HDMI_CTRL_ENABLE);
	msleep(100);

	if (ls2k3000_scdc_tmds_config(adev, link,
				      SCDC_TMDS_CONFIG_SCRAMBLE)) {
		/*
		 * The sink did not take the scrambling configuration;
		 * keep the DC side unscrambled too, otherwise both
		 * ends disagree and the screen stays black.
		 */
		ls2k3000_hdmi_tmds_scramble_enable(adev, intf, false);
		dc_writel(adev, ctrl_reg, ctrl);
		return;
	}

	msleep(100);

	ls2k3000_hdmi_tmds_scramble_enable(adev, intf, true);
	msleep(200);

	/* restart the output */
	dc_writel(adev, ctrl_reg, ctrl);
	msleep(100);
}

static void ls2k3000_hdmi_clear_tmds_scramble(struct loonggpu_dc_crtc *crtc,
					      int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	int link = crtc->resource->base.link;
	u32 ctrl_reg = gdc_reg->hdmi_reg_v2[intf].ctrl;
	u32 ctrl;

	ctrl = dc_readl(adev, ctrl_reg);

	dc_writel(adev, ctrl_reg, ctrl & ~HDMI_CTRL_ENABLE);
	msleep(100);

	ls2k3000_scdc_tmds_config(adev, link, 0x0);

	ls2k3000_hdmi_tmds_scramble_enable(adev, intf, false);

	dc_writel(adev, ctrl_reg, ctrl);
	msleep(100);
}

void ls2k3000_hdmi_pll_set(struct loonggpu_dc_crtc *crtc, int intf, int clock)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	if (ls2k3000_calc_phy_clk((unsigned int)(clock))) {
		configmpll(crtc, intf);
		if (mpllcfg.tmds_clk_ratio) {
			u32 avi_ctrl = gdc_reg->hdmi_reg_v2[intf].avi_ctrl;

			/*
			 * 4K60 (TMDS > 340 MHz): arm scrambling on the DC side
			 * like PMON config_hdmi() does before scanout, then
			 * tell the sink over SCDC.
			 */
			ls2k3000_hdmi_tmds_scramble_enable(adev, intf, true);
			dc_writel(adev, avi_ctrl,
				  dc_readl(adev, avi_ctrl) | 0x5);
			ls2k3000_hdmi_set_tmds_scramble(crtc, intf);
		} else {
			/*
			 * The sink's scrambling state may have been left
			 * enabled by the firmware (4K60 boot UI) or a
			 * previous >340MHz mode - it persists in the monitor
			 * until rewritten or power-cycled.  Always make sure
			 * both ends are unscrambled for low clocks.
			 */
			ls2k3000_hdmi_clear_tmds_scramble(crtc, intf);
		}
	} else {
		DRM_INFO("\n\nError: Fail to find a proper PLL configuration.\n\n");
	}
}

bool ls2k3000_hdmi_enable(struct loonggpu_dc_crtc *crtc, int intf, bool enable)
{
	u32 hdmi_ctrl, phy_cfg0;
	struct loonggpu_device *adev = crtc->dc->adev;
	uint32_t dc_global_reg_save[6];

	if (IS_ERR_OR_NULL(adev))
		return false;

	if (intf >= MAX_DC_INTERFACES)
		return false;

	dc_global_reg_config(adev, dc_global_reg_save, 1);

	hdmi_ctrl = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].ctrl);
	phy_cfg0 = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0);

	if (enable && (phy_cfg0 & (0x1 << 29)) && (hdmi_ctrl & HDMI_CTRL_ENABLE))
		goto out;

	if (!enable && (!(phy_cfg0 & (0x1 << 29))) && (!(hdmi_ctrl & HDMI_CTRL_ENABLE)))
		goto out;

	if (enable) {
		phy_cfg0 |= (0x1 << 29);
		hdmi_ctrl |= HDMI_CTRL_ENABLE;
	} else {
		hdmi_ctrl &= ~HDMI_CTRL_ENABLE;
		phy_cfg0 &= ~(0x1 << 29);
	}

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].ctrl, hdmi_ctrl);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].phy_cfg0, phy_cfg0);

out:
	dc_global_reg_config(adev, dc_global_reg_save, 0);
	return true;
}

void ls2k3000_hdmi_suspend(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;

	crtc->hdmi_ctrl[intf] = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].ctrl);
}

int ls2k3000_hdmi_resume(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	if (crtc->intf[intf].connected) {
		dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].ctrl, crtc->hdmi_ctrl[intf]);
		if (crtc->hdmi_ctrl[intf] & HDMI_CTRL_AUDIO_ENABLE) {
			dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle, 0x00400040);
			dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_ncfg, 6272);
			dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_ctscfg, 0x80000000);
			dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_infoframe, 0x15);
			dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_sample, 0x1);
		}

		/*
		 * S3 sleep powers down the DC/PHY but the sink keeps its SCDC
		 * state, so re-establish scrambling to match the running mode.
		 * Harmless if the later atomic resume re-runs pll_set - both
		 * helpers are idempotent.
		 */
		if (mpllcfg.tmds_clk_ratio) {
			ls2k3000_hdmi_tmds_scramble_enable(adev, intf, true);
			ls2k3000_hdmi_set_tmds_scramble(crtc, intf);
		} else {
			ls2k3000_hdmi_clear_tmds_scramble(crtc, intf);
		}
	}

	return 0;
}

int ls2k3000_hdmi_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].ctrl, 0x382);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle, 0x00400040);
	return 0;
}

int ls2k3000_hdmi_audio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	u32 val;

	val = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].ctrl);
	if (val & 0x4)
		return 0;

	/* enable hdmi audio, but don't touch EN bit which will be
	 * update in loonggpu_hdmi_enable() */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].ctrl, 0x386 | (val & 0x1));

	/* Audio N 
	 * 44.1KHz * 4, dynamic update N && CTS value */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_ncfg, 6272);

	/* Enable Send CTS */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_ctscfg, 0x80000000);

	/* Audio AIF
	 * enable AIF,set freq,and set CC = 1, CA = 0
	 * Update AIF */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_infoframe, 0x15);

	/* Audio Sample Packet */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].audio_sample, 0x1);

	val = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].avi_ctrl);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].avi_ctrl, val | 0x5);
	return 0;
}

int ls2k3000_hdmi_noaudio_init(struct loonggpu_dc_crtc *crtc, int intf)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_connector *aconnector;
	u32 crtc_id;
	u32 val;

	crtc_id = crtc->resource->base.link;
	aconnector = adev->mode_info.connectors[crtc_id];
	val = dc_readl(adev, gdc_reg->hdmi_reg_v2[intf].ctrl);
	if (!is_special_hdmi_display(aconnector, crtc->timing))
		dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].ctrl, 0x382 | (val & 1));

	return 0;
}

/* 9A1000 */
static int ls9a1000_calc_phy_clk(unsigned int pixclock_khz)
{
	unsigned int tx_clkdiv_set[] = {1, 2, 3, 4, 5, 6, 8, 10};
	unsigned int prec_set[] = {1, 5, 10, 50, 100, 1000};	//in 1/PCLK_PRECISION_INDICATOR
	unsigned int tx_rate, multiplier, tx_clkdiv;
	int i, j;
	unsigned int precision_req, precision;
	unsigned int multiplier_min, multiplier_max, multiplier_mid;
	unsigned long long real_dvo, req_dvo;
	unsigned int pixclock_mhz;
	int offset;

	pixclock_mhz = pixclock_khz / 1000;
	//try precision from high to low
	for (j = 0; j < sizeof(prec_set) / sizeof(uint32_t); j++) {
		precision_req = prec_set[j];
		//try each refc
		for (i = 0; i < sizeof(tx_clkdiv_set) / sizeof(uint32_t); i++) {
			tx_clkdiv = tx_clkdiv_set[i];
			multiplier_min = (4500 / 27) / 2;  //1200 / (PLL_REF_CLK_MHZ / refc)
			multiplier_max = (6500 / 27) / 2;  //3200 / (PLL_REF_CLK_MHZ / refc)
			multiplier_mid = (5500 / 27) / 2;  //(loopc_min + loopc_max) / 2;

			offset = 0;
			//try each loopc
			for (multiplier = multiplier_mid; (multiplier <= multiplier_max) && (multiplier >= multiplier_min); multiplier += offset) {
				if (offset < 0) {
					offset = -(offset - 1);
				} else {
					offset = -(offset + 1);
				}
        		tx_rate = (2 * multiplier * 27) / tx_clkdiv / 5 / pixclock_mhz;
        		if ((tx_rate != 1) && (tx_rate != 2) && (tx_rate != 4) && (tx_rate != 8)) continue;
				real_dvo = (2 * multiplier * 27) / tx_clkdiv / 5 / tx_rate;
				req_dvo  = pixclock_mhz;
				precision = abs(real_dvo * PCLK_PRECISION_INDICATOR / req_dvo - PCLK_PRECISION_INDICATOR);

				if ((precision < precision_req)) {
					mpllcfg.multiplier = multiplier;
					mpllcfg.tx_clkdiv = tx_clkdiv;
					mpllcfg.tx_rate   = tx_rate;
					if (j > 1) {
						DRM_INFO("Warning: PIX clock precision degraded to %d / %d pixclock_khz:%d\n",
							 precision_req, PCLK_PRECISION_INDICATOR, pixclock_khz);
					}

					return 1;
				}
			}
		}
	}
	return 0;
}

static void ls9a1000_codec_init(struct loonggpu_device *adev, int intf)
{
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x240c, 1);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2404, 1);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2400, 0);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2200, 0x00050010);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2204, 0x01001000);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2214, 0x70709);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2254, 1);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2300, 0x00050010);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2304, 0x01001000);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2314, 0x70709);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[intf].zoneidle + 0x2354, 1);
}

bool ls9a1000_hdmi_enable(struct loonggpu_dc_crtc *dc_crtc, int intf, bool enable)
{
	return true;
}

int ls9a1000_hdmi_audio_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	uint32_t reg_val;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl);
	if ((reg_val & 0x2) && (reg_val & 0x4))
		return 0;

	/* enable hdmi */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, 0x389 | 0x4);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].zoneidle, 0x400040);

	/* audio N */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ncfg, 6272);
	ls9a1000_codec_init(adev, hdmi_num);

	/* enable ACR */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ctscfg);
	reg_val |= (1 << 31);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ctscfg, reg_val);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, 0x11);
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe);
	reg_val |= (1 << 2);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, reg_val);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, 0x15);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].avi_ctrl, 0x5);

	return 0;
}

int ls9a1000_hdmi_noaudio_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	u32 val;
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;

	val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, 0x388 | (val & 1));

	return 0;
}

void ls9a1000_hdmi_suspend(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;

	dc_crtc->hdmi_ctrl[hdmi_num] = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl);
}

int ls9a1000_hdmi_resume(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, dc_crtc->hdmi_ctrl[hdmi_num]);
	if (dc_crtc->hdmi_ctrl[hdmi_num] & HDMI_CTRL_AUDIO_ENABLE) {
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].zoneidle, 0x00400040);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ncfg, 6272);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ctscfg, 0x80000000);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, 0x15);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_sample, 0x1);
	}

	return 0;
}

static uint32_t hdmi_phy_cr_access_9a(struct loonggpu_device *adev, uint32_t cr_wr_en,
				      uint32_t cr_wdata, uint32_t cr_addr, int hdmi_num)
{
	uint32_t tmp;
	unsigned int retry;

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].cfg_hdmi_phy1, 0);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].cfg_hdmi_phy0, cr_wdata);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].cfg_hdmi_phy1, cr_addr | (cr_wr_en << 26) | (0x1 << 27));
	mdelay(10);

	tmp = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].cfg_hdmi_phy1);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].cfg_hdmi_phy1, tmp & (~(0x1 << 27)));

	for (retry = 0; retry < 1000; retry++) {
		if (!(dc_readl(adev,
			       gdc_reg->hdmi_reg_v2[hdmi_num].hdmi_phy_rdata1)
		      & BIT(31)))
			break;
	}

	if (cr_wr_en == 0) {
		tmp = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].hdmi_phy_rdata0);
		tmp &= 0xffff;
	}

	return tmp;
}

static void ls9a1000_hdmi_phy_cfg(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	uint32_t tx_rate, tx_clkdiv, phy_clk, high_phy_clk, tmds_clk_ratio;
	u32 reg_val;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;
	u32 const delay_us = 1000;
	u32 const timeout_us = 50000;
	int ret;

	tx_clkdiv = get_tx_clkdiv();
	tx_rate = get_tx_rate();
	phy_clk = ((2 * mpllcfg.multiplier * 27) / mpllcfg.tx_clkdiv / 5 / mpllcfg.tx_rate) * 10;
	high_phy_clk = 3400;
	tmds_clk_ratio = ((phy_clk < high_phy_clk) ? 0 : 1);

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl);
	reg_val &= ~0x1;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, reg_val);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg1, 0x33330000);

	/* deassert phy reset */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0);
	reg_val |= 0x1 << 31;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0, reg_val);
	mdelay(30);

	/* deassert lane tx reset */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0);
	reg_val |= 0x1 << 30;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0, reg_val);
	mdelay(30);

	/* use ref clk 0:in 1:out */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2);
	reg_val &= ~(0x1 << 1);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2, reg_val);

	/* wait sram init done */
	ret = readl_poll_timeout_atomic(adev->loongson_dc_rmmio +
				gdc_reg->hdmi_reg_v2[hdmi_num].phy_mon0,
				reg_val, (reg_val & (0x1 << 14)) != 0,
				delay_us, timeout_us);
	if (ret) {
		DRM_INFO("hdmi:%d phy sram init failed\n", hdmi_num);
		return;
	}

	/* set pixelclk & frag_clk */
	hdmi_phy_cr_access_9a(adev, 1, 0x1f, 0x4000, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, mpllcfg.multiplier << 8, 0x4001, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, (tx_clkdiv << 12) | 0x800, 0x4002, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x4003, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, tx_rate << 11, 0x4006, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x226, 0x4042, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, tmds_clk_ratio, 0x5011, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x718, 0x5000, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x5001, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, tmds_clk_ratio, 0x5111, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x718, 0x5100, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x5101, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, tmds_clk_ratio, 0x5211, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x718, 0x5200, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x5201, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, tmds_clk_ratio, 0x5311, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x718, 0x5300, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x5301, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x6811, 0x4041, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x4040, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x224, 0x4042, hdmi_num);

	/* tx_vboost: 0xf [8,6], override enable [9]; reset value is 0x1d0 */
	hdmi_phy_cr_access_9a(adev, 1, 0x3d0, 0xf, hdmi_num);

	hdmi_phy_cr_access_9a(adev, 1, 0xf0, 0x30e8, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xf0, 0x31e8, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xf0, 0x32e8, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xf0, 0x33e8, hdmi_num);

	/* ext ld done */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2);
	reg_val &= ~(0x1 << 29);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2, reg_val);

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2);
	reg_val |= 0x1 << 29;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg2, reg_val);

	/* wait sram boot done */
	ret = readl_poll_timeout_atomic(adev->loongson_dc_rmmio +
				gdc_reg->hdmi_reg_v2[hdmi_num].phy_mon0,
				reg_val, (reg_val & (0xf << 4)) == 0,
				delay_us, timeout_us);
	if (ret) {
		DRM_INFO("hdmi:%d phy sram boot failed\n", hdmi_num);
		return;
	}

	hdmi_phy_cr_access_9a(adev, 1, 0x3d0, 0xf, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xb1f8, 0x1012, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xb1f8, 0x1112, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xb1f8, 0x1212, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0xb1f8, 0x1312, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x4, 0x10c5, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x4, 0x11c5, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x4, 0x12c5, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x4, 0x13c5, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x10, 0x5001, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x10, 0x5101, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x10, 0x5201, hdmi_num);
	hdmi_phy_cr_access_9a(adev, 1, 0x0, 0x5301, hdmi_num);

	/* power up */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg1);
	reg_val &= ~(0xffff << 16);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg1, reg_val);
	mdelay(30);

	/* enable tx valid */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0);
	reg_val &= ~(0x1 << 29);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0, reg_val);

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0);
	reg_val |= (0x1 << 29); /* 使能数据输出 */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].phy_cfg0, reg_val);
	mdelay(10);
}

void ls9a1000_hdmi_pll_set(struct loonggpu_dc_crtc *dc_crtc, int intf, int clock)
{
	ls9a1000_calc_phy_clk((unsigned int)(clock));
	ls9a1000_hdmi_phy_cfg(dc_crtc, intf);
}

void ls9a1000_hdmi_i2c_set(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c)
{
	return;
}

int ls9a1000_hdmi_init(struct loonggpu_dc_crtc *dc_crtc, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int reg_val;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, 0x386);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].zoneidle, 0x00400040);

	/* audio N */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ncfg, 6272);
	/* enable ACR */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ctscfg);
	reg_val |= (1 << 31);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_ctscfg, reg_val);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, 0x11);
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe);
	reg_val |= (1 << 2);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_infoframe, reg_val);

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].audio_sample, 0x1);

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].avi_ctrl);
	reg_val |= 0x5;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].avi_ctrl, reg_val);

	ls9a1000_codec_init(adev, hdmi_num);

	return 0;
}

static void ls9a1000_hdmi_enable_scramble(struct loonggpu_dc_crtc *dc_crtc,
					  uint32_t clock)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int reg_val, retry;
	unsigned char val;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;
	struct loonggpu_dc_i2c *dc_i2c = adev->i2c[hdmi_num];

	if (!dc_crtc->dc_video.scdc_scrambling_supported)
		return;

	if (clock < 340000) {
		reg_val = dc_readl(adev,
				   gdc_reg->hdmi_reg_v2[hdmi_num].frl_cfg0);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].frl_cfg0,
			  reg_val &= ~(1 << 4));

		ls9a_i2c_write(dc_i2c, 0x54, 0x20, 0);
	} else {
		for (retry = 0; retry < 10; retry++) {
			ls9a_i2c_write(dc_i2c, 0x54, 0x20, 0x3);
			msleep(100);

			ls9a_i2c_read(dc_i2c, 0x54, 0x20, &val);
			if (val == 0x3)
				break;
		}

		if (retry >= 10)
			DRM_INFO("HDMI:%d scramble:%#X\n", hdmi_num, val);

		reg_val = dc_readl(adev,
				   gdc_reg->hdmi_reg_v2[hdmi_num].frl_cfg0);
		dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].frl_cfg0,
			  reg_val | (1 << 4));
		msleep(100);
	}
}

void ls9a1000_hdmi_timing_set(struct loonggpu_dc_crtc *dc_crtc, struct dc_timing_info *timing, int intf)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int reg_val, retry;
	int mode_bpc;
	int hdmi_num = dc_crtc->dc_video.hdmi_num;
	struct loonggpu_dc_i2c *dc_i2c = adev->i2c[hdmi_num];
	unsigned char val;

	/* color mode */
	if(timing->depth == 8){
		mode_bpc = 0;
	}else if(timing->depth == 10){
		mode_bpc = 1;
	}
	mode_bpc = 0;

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing0);
	reg_val &= ~(0xf << 8);
	reg_val |= (mode_bpc << 8);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing0, reg_val);

	/* H & V polarity */
	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing0);
	reg_val &= ~0x3;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing0, reg_val);

	/* H sync */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing1, (timing->hsync_end << 16) | timing->hsync_start);
	/* V sync */
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing2, (timing->vsync_end << 16) | timing->vsync_start);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing3, (timing->hdisplay  << 16) | timing->htotal);
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].timing4, (timing->vdisplay << 16) | timing->vtotal);

	ls9a1000_hdmi_enable_scramble(dc_crtc, timing->clock);

	reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl);
	reg_val |= 0x1;
	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].ctrl, reg_val);

	if (dc_crtc->dc_video.scdc_scrambling_supported
	    && timing->clock >= 340000) {
		for (retry = 0; retry < 10; retry++) {
			mdelay(10);
			ls9a_i2c_read(dc_i2c, 0x54, 0x21, &val);
			if (val)
				break;
		}

		if (retry >= 10)
			DRM_INFO("HDMI:%d scramble_st:%#X\n", hdmi_num, val);
	}
}
