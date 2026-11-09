#ifndef __DC_V2_REG_H__
#define __DC_V2_REG_H__

struct dc_video_reg {
	u32 cfginfo;
	u32 intinfo;
	u32 scrsize;
	u32 backsize;
	u32 zoomdx_l;
	u32 zoomdx_h;
	u32 zoomdy_l;
	u32 zoomdy_h;
	u32 hsync;
	u32 vsync;
	u32 display_total;
	u32 colorkey_value;
	u32 hadj;
	u32 vadl;
	u32 dither;
	u32 dth_table0;
	u32 dth_table1;
	u32 rgbout;
	u32 syncnt;
	u32 urg_low;
	u32 urg_hig;
	u32 unzip_mode;
	u32 gamma_table;
	u32 layinfo[8];
	u32 laydu[8];
	u32 laydv[8];
	u32 laysize[8];
	u32 laypoint[8];
	u32 samsize[8];
	u32 samoff[8];
	u32 picsize[8];
	u32 chnorder[8];
	u32 cachebase[8];
	u32 cachesize[8];
	u32 membase_low[8];
	u32 membase_hig[8];
	u32 cachebase_uv[8];
	u32 cachesize_uv[8];
	u32 membase_low_uv[8];
	u32 membase_hig_uv[8];
};

struct dc_vga_reg {
	u32 vga_cfg;
	u32 vga_int_status;
	u32 vga_clear;
};

struct dc_dvo_reg {
	u32 dvo_cfg;
	u32 dvo_int_status;
	u32 dvo_clear;
};

struct dc_wb_reg {
	u32 wb_clear;
	u32 wb_en;
	u32 buff_wcnt;
	u32 buff_acnt;
	u32 state_flags;
	u32 last_wh;
	u32 pitch_clrfmt;
	u32 max_wh;
	u32 base0_lo;
	u32 base0_hi;
	u32 base1_lo;
	u32 base1_hi;
	u32 base2_lo;
	u32 base2_hi;
	u32 base3_lo;
	u32 base3_hi;
};

struct dc_unzip_reg {
	u32 meta_base_l;
	u32 meta_base_h;
	u32 meta_mask_l;
	u32 meta_mask_h;
	u32 dc_zip_ctl;
};

struct dc_top_reg {
	u32 video_sync_en;
	u32 wb_broadcast_en;
	u32 dctop_clear;
	u32 dc_mmap;
	u32 dcint_msgtype;
	u32 dcint_lienstates;
	u32 dctest_ram_ctrl;
	u32 dc_cg_off_mask;
};

#endif /* __DC_V2_REG_H__ */
