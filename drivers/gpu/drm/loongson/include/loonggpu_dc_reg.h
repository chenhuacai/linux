#ifndef __LOONGGPU_DC_REG_H__
#define __LOONGGPU_DC_REG_H__

#include "loonggpu_dc.h"
#include "loonggpu_dc_video_reg.h"

struct dc_global_reg {
	u32 intr;
	u32 hdmi_hp_stat;
	u32 vga_hp_cfg;
	u32 i2c_addr;
	u32 gpio_cfg_off;
	u32 gpio_in_off;
	u32 gpio_out_off;
	u32 intr_1;
	u32 intr_en;
};

struct dc_crtc_reg {
	u32 cfg;
	u32 fbaddr0_lo;
	u32 fbaddr1_lo;
	u32 fbaddr0_hi;
	u32 fbaddr1_hi;
	u32 stride;
	u32 fborigin;
	u32 ditcfg;
	u32 dittab_lo;
	u32 dittab_hi;
	u32 panelcfg;
	u32 paneltim;
	u32 hdisplay;
	u32 hsync;
	u32 vdisplay;
	u32 vsync;
	u32 gamindex;
	u32 gamdata;
	u32 syncdev;
	u32 display_pos;
	u32 meta0_l;
	u32 meta0_h;
	u32 meta1_l;
	u32 meta1_h;
	u32 vsync_counter;
};

struct dc_cursor_reg {
	u32 cfg;
	u32 laddr;
	u32 haddr;
	u32 position;
	u32 back;
	u32 fore;
};

struct dc_hdmi_reg_v1 {
	u32 zoneidle;
	u32 ctrl;
	u32 phy_ctrl;
	u32 phy_pllcfg;
	u32 phy_pec0;
	u32 phy_pec1;
	u32 phy_pec2;
	u32 phy_rsvr;
	u32 phy_calctrl;
	u32 phy_calout;
	u32 avi_cont0;
	u32 avi_cont1;
	u32 avi_cont2;
	u32 avi_cont3;
	u32 avi_ctrl;
	u32 vsi_cfg;
	u32 audio_buf;
	u32 audio_ncfg;
	u32 audio_ctscfg;
	u32 audio_ctscalcfg;
	u32 audio_infoframe;
	u32 audio_sample;
};

struct dc_hdmi_reg_v2 {
	u32 zoneidle;
	u32 ctrl;
	u32 audio_buf;
	u32 audio_ncfg;
	u32 audio_ctscfg;
	u32 audio_ctscalcfg;
	u32 audio_infoframe;
	u32 audio_sample;
	u32 timing0;
	u32 timing1;
	u32 timing2;
	u32 timing3;
	u32 timing4;
	u32 irq;
	u32 irq_en;
	u32 avi_infoframe0;
	u32 avi_infoframe1;
	u32 avi_infoframe2;
	u32 avi_infoframe3;
	u32 avi_ctrl;
	u32 vendor_specific_infoframe;
	u32 i2c0;
	u32 i2c1;
	u32 phy_cfg0;
	u32 phy_cfg1;
	u32 phy_cfg2;
	u32 phy_mon0;
	/* 9A1000 add */
	u32 cfg_hdmi_phy0;
	u32 cfg_hdmi_phy1;
	u32 hdmi_phy_rdata0;
	u32 hdmi_phy_rdata1;
	u32 irq_hpd;
	u32 frl_cfg0;
};

struct dc_dp_reg {
       u32 link_cfg0;
       u32 link_cfg1;
       u32 link_cfg2;
       u32 link_cfg3;
       u32 link_cfg4;
       u32 link_cfg5;
       u32 link_cfg6;
       u32 link_monitor0;
       u32 link_monitor1;
       u32 link_monitor2;
       u32 link_monitor3;
       u32 link_monitor4;
       u32 link_monitor5;
       u32 link_monitor6;
       u32 link_monitor7;
       u32 link_monitor8;
       u32 link_monitor9;
       u32 link_monitor10;
       u32 link_monitor11;
       u32 link_monitor12;
       u32 link_monitor13;
       u32 link_monitor14;
       u32 link_monitor15;
       u32 aux_channel0;
       u32 aux_channel1;
       u32 aux_channel2;
       u32 aux_channel3;
       u32 aux_channel4;
       u32 aux_channel5;
       u32 aux_channel6;
       u32 aux_channel7;
       u32 aux_channel8;
       u32 aux_channel9;
	u32 aux_channel10;
       u32 aux_monitor0;
       u32 aux_monitor1;
       u32 aux_monitor2;
       u32 aux_monitor3;
       u32 aux_monitor4;
       u32 aux_monitor5;
       u32 aux_monitor6;
       u32 aux_monitor7;
       u32 sdp_cfg0;
       u32 sdp_cfg1;
       u32 sdp_cfg2;
       u32 sdp_cfg3;
       u32 sdp_cfg4;
       u32 sdp_cfg5;
       u32 sdp_cfg6;
       u32 sdp_cfg7;
       u32 sdp_cfg8;
       u32 sdp_cfg9;
       u32 sdp_cfg10;
       u32 sdp_cfg11;
       u32 sdp_cfg12;
       u32 sdp_cfg13;
       u32 sdp_cfg61;
       u32 sdp_cfg62;
       u32 sdp_cfg63;
       u32 sdp_cfg64;
       u32 sdp_cfg65;

       u32 phy_cfg0;
       u32 phy_cfg1;
       u32 phy_cfg2;
       u32 phy_cfg3;
       u32 phy_cfg4;
       u32 phy_cfg5;
       u32 phy_cfg6;
       u32 phy_cfg7;
       u32 phy_cfg8;
       u32 phy_cfg12;

       u32 phy_monitor0;
       u32 phy_monitor1;
       u32 phy_monitor2;
	/* 9A1000 add */
	u32 hpd_status;
	u32 hpd_enable;
	u32 dp_hpd;
};

struct dc_reg {
	struct dc_global_reg global_reg;
	struct dc_crtc_reg crtc_reg[DC_DVO_MAXLINK];
	struct dc_cursor_reg cursor_reg[DC_DVO_MAXLINK];
	struct dc_hdmi_reg_v1 hdmi_reg_v1[MAX_DC_INTERFACES];
	struct dc_hdmi_reg_v2 hdmi_reg_v2[MAX_DC_INTERFACES];
	struct dc_dp_reg dp_reg[DC_DVO_MAXLINK];
	struct dc_video_reg video_reg[DC_DVO_MAXLINK];
	struct dc_vga_reg vga_reg;
	struct dc_dvo_reg dvo_reg;
	struct dc_wb_reg wb_reg[DC_DVO_MAXLINK];
	struct dc_unzip_reg unzip_reg;
	struct dc_top_reg top_reg;
};

extern struct dc_reg ls7a2000_dc_reg;
extern struct dc_reg ls2k3000_dc_reg;
extern struct dc_reg ls7a1000_dc_reg;
extern struct dc_reg ls9a1000_dc_reg;

#if defined(TO_UNCAC)
#define LG_TO_UNCAC TO_UNCAC
#else
#define LG_TO_UNCAC TO_UNCACHE
#endif

#define HT1LO_PCICFG_BASE 0xefdfe000000UL
#define LS7A_PCH_CFG_SPACE_REG (LG_TO_UNCAC(HT1LO_PCICFG_BASE)|0x0000a810)
#define LS7A_PCH_CFG_REG_BASE ((*(volatile unsigned int *)(LS7A_PCH_CFG_SPACE_REG))&0xfffffff0)
#define LS_PIX0_PLL (LS7A_PCH_CFG_REG_BASE + 0x04b0)
#define LS_PIX1_PLL (LS7A_PCH_CFG_REG_BASE + 0x04c0)
#define DC_IO_PIX_PLL 0x04B0

/* CRTC */
#define CRTC_CFG_MASK		0x131B97
#define CRTC_CFG_FORMAT_MASK	0x7
#define CRTC_CFG_FORMAT32	(1 << 2)
#define CRTC_CFG_FORMAT16	(11 << 0)
#define CRTC_CFG_FORMAT15	(1 << 1)
#define CRTC_CFG_FORMAT12	(1 << 0)
#define CRTC_CFG_TILE4x4	BIT(4)
#define CRTC_CFG_LINEAR		~BIT(4)
#define CRTC_CFG_TILE8_ENABLE	BIT(5)
#define CRTC_CFG_TILE8_DISABLE	~BIT(5)
#define CRTC_CFG_ZIP_ENABLE BIT(6)
#define CRTC_CFG_ZIP_DISABLE    ~BIT(6)
#define CRTC_CFG_FB_SWITCH	BIT(7)
#define CRTC_CFG_ENABLE		BIT(8)
#define CRTC_CFG_SWITCH_PANEL	BIT(9)
#define CRTC_CFG_DMA_MASK	(0x3 << 16)
#define CRTC_CFG_DMA_32		(0x3 << 16)
#define CRTC_CFG_DMA_64		(0x2 << 16)
#define CRTC_CFG_DMA_128	(0x1 << 16)
#define CRTC_CFG_DMA_256	(0x0 << 16)
#define CRTC_CFG_GAMMA		BIT(12)
#define CRTC_CFG_RESET		BIT(20)

#define CRTC_PANCFG_BASE	0x80001010
#define CRTC_PANCFG_DE		BIT(0)
#define CRTC_PANCFG_DEPOL	BIT(1)
#define CRTC_PANCFG_CLKEN	BIT(8)
#define CRTC_PANCFG_CLKPOL	BIT(9)

#define CRTC_HPIXEL_SHIFT	0
#define CRTC_HPIXEL_MASK	0xFFFF
#define CRTC_HTOTAL_SHIFT	16
#define CRTC_HTOTAL_MASK	0xFFFF

#define CRTC_HSYNC_START_SHIFT	0
#define CRTC_HSYNC_START_MASK	0xFFFF
#define CRTC_HSYNC_END_MASK	0x3FFF
#define CRTC_HSYNC_END_SHIFT	16
#define CRTC_HSYNC_POLSE	BIT(30)
#define CRTC_HSYNC_POL		BIT(31)

#define CRTC_VPIXEL_SHIFT	0
#define CRTC_VPIXEL_MASK	0xFFFF
#define CRTC_VTOTAL_SHIFT	16
#define CRTC_VTOTAL_MASK	0xFFFF

#define CRTC_VSYNC_START_SHIFT	0
#define CRTC_VSYNC_START_MASK	0xFFFF
#define CRTC_VSYNC_END_SHIFT	16
#define CRTC_VSYNC_END_MASK	0x3FFF
#define CRTC_VSYNC_POLSE	BIT(30)
#define CRTC_VSYNC_POL		BIT(31)

#define CRTC_GAMMA_BLUE_SHIFT	0
#define CRTC_GAMMA_BLUE_MASK	0xFF
#define CRTC_GAMMA_GREEN_SHIFT	8
#define CRTC_GAMMA_GREEN_MASK	0xFF
#define CRTC_GAMMA_RED_SHIFT	16
#define CRTC_GAMMA_RED_MASK	0xFF

#define INT_VSYNC1_ENABLE	BIT(16)
#define INT_VSYNC0_ENABLE	BIT(18)

/* CURSOR */
#define DC_CURSOR_FORMAT_SHIFT	0
#define DC_CURSOR_FORMAT_MASK	0x3
#define DC_CURSOR_MODE_CLEAN	~(1 << 2)
#define DC_CURSOR_MODE_32x32	(0 << 2)
#define DC_CURSOR_MODE_64x64	(1 << 2)
#define DC_CURSOR_DISPLAY_SHIFT	4
#define DC_CURSOR_DISPLAY_MASK	0x1
#define DC_CURSOR_POS_HOT_Y_SHIFT 8
#define DC_CURSOR_POS_HOT_Y_MASK  0x3F
#define DC_CURSOR_POS_HOT_X_SHIFT 16
#define DC_CURSOR_POS_HOT_X_MASK  0x3F

#define DC_CURSOR_POS_Y_SHIFT	16
#define DC_CURSOR_POS_Y_MASK	0xFFF
#define DC_CURSOR_POS_X_SHIFT	0
#define DC_CURSOR_POS_X_MASK	0xFFF

/* HDMI */
#define HDMI_CTRL_ENABLE	(1 << 0)
#define HDMI_CTRL_AUDIO_ENABLE	(1 << 2)
#define HDMI_PHY_CTRL_ENABLE	(1 << 0)
#define HDMI_AVI_ENABLE_PACKET	(1 << 0)
#define HDMI_AVI_FREQ_EACH_FRAME (0 << 1)
#define HDMI_AVI_FREQ_TWO_FRAME	(1 << 1)
#define HDMI_AVI_UPDATE		(1 << 2)

/* AUDIO */
#define HDMI_EDID_AUDIO_STATUS      (1UL << (6))

#endif /* __LOONGGPU_DC_REG_H__ */
