#include "loonggpu.h"
#include "loonggpu_mode.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_plane.h"
#include "loonggpu_dc_video.h"
#include "loonggpu_dc_video_reg.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_dc_hdmi.h"

static void video_config_ok(struct loonggpu_device *adev, int video_num, bool cfg_ok)
{
	int reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val &= ~0x3;

	/* 为0时开始配置，为1时配置完毕 */
	if (cfg_ok)
		reg_val |= (1 << 3) | (1 << 4);
	else
		reg_val &= ~(1 << 3);

	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);
}

static void video_reset(struct loonggpu_device *adev, u32 link, int video_num)
{
	u32 reg_val;
	unsigned long complete;
	struct loonggpu_dc_video *dc_video = &adev->dc->link_info[link].crtc->dc_video;
	int timeout = msecs_to_jiffies(2000);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val &= ~(1 << 4); /* disable_video */
	reg_val &= ~(1 << 3); /* in config */
	reg_val &= ~0x3; /* config video */
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val |= (1 << 5); /* soft_reset */
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	complete = wait_for_completion_timeout(&dc_video->reset_complete, timeout);
	if (!complete) {
		DRM_ERROR("Timeout video:%d reset!\n", video_num);
		return;
	}

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val &= ~(1 << 5); /* soft reset ok */
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);
}

static void set_layer_cache(struct loonggpu_device *adev, int link, int layer_num)
{
	u32 cache_line_num;
	u32 uv_cache_line_num;
	u32 cache_base_addr;
	u32 is_bit32 = 0;
	u32 is_bit16 = 0;
	u32 is_bit8 = 0;
	u32 rot = 0;
	u32 is_yuv = 0;
	u32 data_fmt;
	u32 color_fmt;
	u32 rotate;
	u32 use_flag;
	u32 s_height, s_width;
	u32 s_width_rot;
	u32 reg_val;
	struct loonggpu_dc_video *dc_video = &adev->dc->link_info[link].crtc->dc_video;
	int video_num = dc_video->video_num;
	int i;

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].samsize[layer_num]);
	s_width = reg_val >> 16;
	s_height = reg_val & 0xffff;

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	rotate = (reg_val >> 20) & 0x3;
	color_fmt = (reg_val >> 16) & 0xf;
	data_fmt = (reg_val >> 14) & 0x3;
	use_flag = reg_val & 0x1;

	switch (color_fmt) {
	case RGB8:
	case RGB10:
		is_bit32 = 1;
		break;
	case RGB4:
	case RGB5:
	case RGB565:
	case RG8:
		is_bit16 = 1;
		break;
	case YUV10:
		is_bit16 = 1;
		is_yuv = 1;
		break;
	case YUV8:
		is_bit8 = 1;
		is_yuv = 1;
		break;
	}

	switch (rotate) {
	case ROT90:
	case ROT270:
		rot = 1;
		break;
	}
	s_width_rot = rot ? s_height : s_width;

	if (data_fmt == LINER) {
		cache_line_num = is_bit32 ? 512 : 256;
		uv_cache_line_num = 0;
	} else {
		if (is_bit32) {
			cache_line_num = s_width_rot < 2016 ? 512 :
						s_width_rot < 4064 ? 1024 : 2048;
			uv_cache_line_num = 0;
		} else if (is_bit16) {
			cache_line_num = s_width_rot < 1984 ? 256 :
						s_width_rot < 4032 ? 512 : 1024;
			uv_cache_line_num = is_yuv ? cache_line_num : 0;
		} else {//YUV8
			cache_line_num = s_width_rot < 1920 ? (128 << rot) :
						s_width_rot < 3968 ? (256 << rot) : (512 << rot);
			uv_cache_line_num = is_yuv ? cache_line_num >> rot : 0;
		}
	}

	dc_writel(adev, gdc_reg->video_reg[video_num].cachesize[layer_num], cache_line_num);

	if (layer_num < 2) {
		cache_base_addr = 0;
		dc_video->layer_config[layer_num].cache_addr_start = 0;
		dc_video->layer_config[layer_num].cache_addr_end = cache_base_addr + (cache_line_num << 7);
	} else {
		cache_base_addr = dc_video->layer_config[layer_num - 2].cache_addr_end;
		dc_video->layer_config[layer_num].cache_addr_start = cache_base_addr;
		dc_video->layer_config[layer_num].cache_addr_end = cache_base_addr + (cache_line_num << 7);
	}

	dc_writel(adev, gdc_reg->video_reg[video_num].cachebase[layer_num], cache_base_addr);

	if (dc_video->layer_config[layer_num].cache_line_num != cache_line_num &&
	    layer_num < 6 &&
	    dc_video->layer_config[layer_num + 2].cache_line_num) {
		dc_video->layer_config[layer_num].cache_line_num = cache_line_num;

		/* 从更改的图层向后查找图层做更改 */
		for (i = layer_num + 2; i < 8; i+=2) {
			dc_video->layer_config[i].cache_addr_start =
				dc_video->layer_config[layer_num].cache_addr_end;
			dc_video->layer_config[i].cache_addr_end =
				dc_video->layer_config[i].cache_addr_start +
				(dc_video->layer_config[i].cache_line_num << 7);

			dc_writel(adev, gdc_reg->video_reg[video_num].cachebase[i],
				 dc_video->layer_config[i].cache_addr_start);
		}
	} else
		dc_video->layer_config[layer_num].cache_line_num = cache_line_num;

}

/* 图层解压缩模式设置以及关闭解压缩，需传入video编号，图层编号，解压缩模式 */
static int video_unzip_mode(struct loonggpu_device *adev, int video_num, int layer_num, int unzip_mode)
{
	int ret = 0;
	int reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].unzip_mode);
	reg_val &= ~(0xf << layer_num);

	/* set unzip mode */
	switch (unzip_mode) {
	case VIDEO_NO_COMPRESSED:
		reg_val &= ~(0xf << layer_num);
		break;
	case VIDEO_COMPRESSED_R8G8B8A8:
		reg_val |= 0x1 << (layer_num * 4);
		break;
	case VIDEO_COMPRESSED_R10G10B10A2:
		reg_val |= 0x2 << (layer_num * 4);
		break;
	case VIDEO_COMPRESSED_A2R10G10B10:
		reg_val |= 0x3 << (layer_num * 4);
		break;
	case VIDEO_COMPRESSED_D24S8:
		reg_val |= 0x4 << (layer_num * 4);
		break;
	case VIDEO_COMPRESSED_S8D24:
		reg_val |= (0x5 << (layer_num * 4));
		break;
	default:
		DRM_ERROR("Unsupported zip mode:%#X !!!\n", unzip_mode);
		return -1;
	}

	dc_writel(adev, gdc_reg->video_reg[video_num].unzip_mode, reg_val);

	return ret;
}

u32 video_get_syncnt(struct loonggpu_device *adev, int link)
{
	u32 vsync_cnt;
	u32 reg_val;
	int video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].syncnt);
	vsync_cnt = reg_val >> 16;

	return vsync_cnt;
}

int video_get_scanoutpos(struct loonggpu_device *adev, int crtc_num,
				  u32 *vbl, u32 *position)
{
	*position = 0;
	*vbl = 0;

	return 0;
}

int video_gamma_set(struct drm_crtc *crtc, u16 *red, u16 *green, u16 *blue)
{
	struct loonggpu_device *adev = crtc->dev->dev_private;
	int link = crtc->index;
	int video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	int reg_val;
	int i;

	video_config_ok(adev, video_num, 0);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val |= (1 << 2) | (1 << 0); /* gamma_en | cfg_gamma */
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	/* gamma表共1024项,一次循环写4个寄存器 */
	for (i = 0; i < 256; i++) {
		reg_val = ((red[i] & 0xffc0) << 14) | ((green[i] & 0xffc0) << 4) | ((blue[i] & 0xffc0) >> 6);
		dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table + (4 * i) * 4, reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table + (4 * i + 1) * 4, reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table + (4 * i + 2) * 4, reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table + (4 * i + 3) * 4, reg_val);
	}

	video_config_ok(adev, video_num, 1);

	return 0;
}

bool video_enable(struct loonggpu_dc_crtc *dc_crtc, bool enable)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int video_num;
	u32 link;
	int reg_val;

	if (IS_ERR_OR_NULL(dc_crtc))
		return false;

	link = dc_crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	if (dc_crtc->dc_video.cfgok == 0)
		return true;

	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	if (enable && (reg_val & (1 << 4)))
		return true;

	if (!enable && (!(reg_val & (1 << 4))))
		return true;

	video_config_ok(adev, video_num, 0);

	if (enable) {
		reg_val |= (1 << 4);
		dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);
	} else {
		reg_val &= ~(1 << 4);
		dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);
	}

	reg_val |= (1 << 3);
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	return true;
}

bool video_mode_config(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	int width, hstart, hend, htotal;
	int height, vstart, vend, vtotal;
	int link;
	int reg_val;
	int ret = true;
	int video_num;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	width = timing->hdisplay;
	hstart = timing->hsync_start;
	hend = timing->hsync_end;
	htotal = timing->htotal;
	height = timing->vdisplay;
	vstart = timing->vsync_start;
	vend =  timing->vsync_end;
	vtotal = timing->vtotal;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	if (crtc->dc_video.cfgok == 1)
		video_config_ok(adev, video_num, 0);

	dc_writel(adev, gdc_reg->video_reg[video_num].scrsize, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].backsize, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdx_l, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdx_h, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdy_l, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdy_h, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].hsync, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].vsync, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].display_total, 0);

	reg_val = ((width & 0xfff) << 16) | (height & 0xfff);
	/* 屏幕分辨率 */
	dc_writel(adev, gdc_reg->video_reg[video_num].scrsize, reg_val);
	/* 背景分辨率,需小于屏幕分辨率 */
	dc_writel(adev, gdc_reg->video_reg[video_num].backsize, reg_val);
	/* 屏幕分辨率和背景分辨率相同时放大系数为0x7fffffffff */
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdx_l, 0xffffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdx_h, 0x7f);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdy_l, 0xffffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].zoomdy_h, 0x7f);

	reg_val = (0 << 27) | (1 << 26) | (hend << 13) | hstart;
	/* 水平同步 */
	dc_writel(adev, gdc_reg->video_reg[video_num].hsync, reg_val);

	if (timing->fixed_vsync_start || timing->fixed_vsync_end)
		reg_val = (0 << 27) | (1 << 26) | (timing->fixed_vsync_end << 13) | timing->fixed_vsync_start;
	else
		reg_val = (0 << 27) | (1 << 26) | (vend << 13) | vstart;
	dc_writel(adev, gdc_reg->video_reg[video_num].vsync, reg_val);

	reg_val = (vtotal << 16) | htotal;
	/* 总像素数 */
	dc_writel(adev, gdc_reg->video_reg[video_num].display_total, reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].colorkey_value, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].hadj, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].vadl, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].dither, 0);

	/* 初始化的时候软件里的cfgok为0,在这里已经完成全部配置，cfgok必须置1 */
	video_config_ok(adev, video_num, 1);
	if (crtc->dc_video.cfgok == 0)
		crtc->dc_video.cfgok = 1;

	return ret;
}

/* 背景图层配置，必须使能，层叠关系必须置于底层，窗口大小必须和对应显示器分辨率一致，
   图层大小由framebuffer大小决定，采样位置通过传入的坐标确定，采样大小和窗口大小必须
   一致。*/
bool primary_layer_config(struct loonggpu_dc_crtc *crtc, struct dc_primary_plane *primary)
{
	struct loonggpu_device *adev;
	struct loonggpu_dc_crtc *dc_crtc;
	u32 reg_val;
	u32 layinfo_reg;
	u64 vram_base;
	u32 link;
	int fb_width, fb_height;
	u32 format;
	int width, height;
	int x, y;
	int stride;
	int layer_num = 0;
	int ret = 0;
	int video_num;

	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(primary))
		return false;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	adev = crtc->dc->adev;
	dc_crtc = adev->dc->link_info[link].crtc;
	fb_width = primary->fb_width;
	fb_height = primary->fb_height;
	vram_base = primary->fb_address;
	format = primary->format;
	width = primary->crtc_width;
	height = primary->crtc_height;
	x = primary->crtc_x;
	y = primary->crtc_y;
	stride = primary->stride;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	layinfo_reg = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	layinfo_reg |= (0xff << 6) | /* 固定透明度数值,level为0的图层必须为255 */
		       (1 << 5) | /* 使用固定透明度，level为0的图层必须为1 */
		       (1 << 0); /* enable layer */
	layinfo_reg &= ~(0xf << 1); /* 层叠关系(level),必须为0 */

	/* color format */
	layinfo_reg &= ~(0xf << 16);
	switch (format) {
	case DRM_FORMAT_RGB565:
		layinfo_reg |= (RGB565 << 16);
		break;
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		layinfo_reg |= (RGB8 << 16);
		break;
	default:
		DRM_ERROR("Unsupported screen format!\n");
		return false;
	}

	/* data format */
	DRM_DEBUG("dc_crtc->array_mode:%d\n", dc_crtc->array_mode);
	if (dc_crtc->array_mode == 2) { /* tile24 */
		layinfo_reg &= ~(7 << 22);
		layinfo_reg &= ~(3 << 14);
		layinfo_reg |= (2 << 14);
	} else if (dc_crtc->array_mode == 4) { /* tile248 */
		layinfo_reg &= ~(7 << 22);
		layinfo_reg &= ~(3 << 14);
		layinfo_reg |= (3 << 14);
	} else if (dc_crtc->array_mode == 8) { /* tile24816 */
		layinfo_reg &= ~(7 << 22);
		layinfo_reg &= ~(3 << 14);
		layinfo_reg |= (1 << 22);
		layinfo_reg |= (3 << 14);
	} else { /* liner */
		layinfo_reg &= ~(3 << 14);
		layinfo_reg &= ~(7 << 22);
	}

	/* rotate */ /* 旋转状态无法获取，暂时不使用图层旋转 */
	layinfo_reg &= ~(3 << 20); // rotate 0

	if (dc_crtc->dc_video.cfgok == 1)
		video_config_ok(adev, video_num, 0);

	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].unzip_mode, 0);

	/* unzip mode */
	video_unzip_mode(adev, video_num, layer_num, primary->unzip_mode);

	reg_val = (width << 16) | height;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);

	/* 采样区域在图层中的位置坐标 */
	reg_val = (x << 16) | y;
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], reg_val);

	/* 图层的大小，背景FB的大小 */
	reg_val = (stride/4  << 16) | fb_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);

	/* 颜色通道顺序 */
	reg_val = (3 << 12) |	/* a_order */
		  (2 << 8) |	/* r_order */
		  (1 << 4) |	/* g_order */
		  (0 << 0);	/* b_order */
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[layer_num], (vram_base & 0xffffffff));
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[layer_num], vram_base >> 32);

	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0x3ffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0x3ffffff);

	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], layinfo_reg);

	if (crtc->timing->hdisplay != width || crtc->timing->vdisplay != height ||
	    crtc->timing->array_mode != dc_crtc->array_mode)
		set_layer_cache(adev, link, layer_num);

	if (dc_crtc->dc_video.cfgok == 1)
		video_config_ok(adev, video_num, 1);

	return ret;
}

static void primary_layer_init(struct loonggpu_device *adev, int link)
{
	u32 reg_val;
	int video_num;
	u32 layer_num = 0;

	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	reg_val = (3840 << 16) | 2160;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], 0);

	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0x3ffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0x3ffffff);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	reg_val &= ~(1 << 0);
	reg_val |= (RGB8 << 16);
	reg_val &= ~(7 << 22);
	reg_val &= ~(3 << 14);
	reg_val |= (2 << 14);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

	set_layer_cache(adev, link, layer_num);
}

static void overlay_layer_init(struct loonggpu_device *adev, int link)
{
	struct loonggpu_bo *robj = NULL;
	u32 reg_val;
	u32 i;
	u64 gpu_addr;
	void *cpu_addr;
	u32 width = 100;
	u32 height = 100;
	u32 bo_size = width * height * 4;
	int video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	for (i = 1; i <= USE_OVERLAY_NUM; i++) {
		reg_val = 0;
		DRM_INFO("set video:%d layer:%d \n", video_num, i);
		robj = NULL;
		loonggpu_bo_create_kernel(adev, bo_size, PAGE_SIZE, LOONGGPU_GEM_DOMAIN_VRAM,
					&robj, &gpu_addr, &cpu_addr);
		/* 这里创建了bo用于测试，bo没有释放，后期需修正 */
		memset(cpu_addr, 0x0a090909, bo_size);

		reg_val = (RGB8 << 16) |	/* 色彩格式 */
			(LINER << 14) |		/* 数据格式 */
			(0x1 << 6) |		/* 固定透明度数值，255为不透明 */
			(1 << 5) |		/* 使用固定透明度 */
			(i << 1) |		/* 层叠关系 */
			(1 << 0 );		/* 启用图层 */
		dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[i], reg_val);

		dc_writel(adev, gdc_reg->video_reg[video_num].laydu[i], 0x3ffffff);
		dc_writel(adev, gdc_reg->video_reg[video_num].laydv[i], 0x3ffffff);
		reg_val = (width << 16) | height;
		dc_writel(adev, gdc_reg->video_reg[video_num].laysize[i], reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[i], 0);
		dc_writel(adev, gdc_reg->video_reg[video_num].samsize[i], reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].samoff[i], 0);
		dc_writel(adev, gdc_reg->video_reg[video_num].picsize[i], reg_val);

		reg_val = (3 << 12) | (2 << 8) | (1 << 4) | (0 << 0);
		dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[i], reg_val);

		dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[i], (u32)gpu_addr);
		dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[i], gpu_addr >> 32);

		set_layer_cache(adev, link, i);
	}
}

/* 光标图层的初始化函数，完成初始化时将光标关闭，在进入图形界面后再开启，否侧会在无需光标的
   情况中出现一个方块 */
static void cursor_layer_init(struct loonggpu_device *adev, int link)
{
	u32 reg_val;
	u32 layer_num = USE_OVERLAY_NUM + 1;
	int video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	/* set cursor size */
	reg_val = (64 << 16) | 64;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);

	reg_val = (3 << 12) |	/* a_order */
		(2 << 8) |	/* r_order */
		(1 << 4) |	/* g_order */
		(0 << 0);	/* b_order */
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0x3ffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0x3ffffff);

	set_layer_cache(adev, link, layer_num);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	reg_val &= ~(1 << 0);
	reg_val |= (RGB8 << 16);
	reg_val &= ~(7 << 22);
	reg_val &= ~(3 << 14);
	reg_val |= (2 << 14);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);
}

bool cursor_layer_set(struct loonggpu_dc_crtc *crtc, struct dc_cursor_info *cursor)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_crtc *acrtc;
	int width, height;
	int enable;
	u32 reg_val;
	u32 layer_num = USE_OVERLAY_NUM + 1;
	u32 link;
	int video_num;
	u32 addr_l = cursor->address.low_part;
	u32 addr_h = cursor->address.high_part;

	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(cursor))
		return false;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	acrtc = adev->mode_info.crtcs[link];
	width = cursor->x;
	height = cursor->y;
	enable = cursor->enable;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	DRM_DEBUG("cursor update width:%d, height:%d\n", width, height);
	DRM_DEBUG("format:%#X, enable:%d, addr_l:%#X addr_h:%#X\n",
		  cursor->format, enable, addr_l, addr_h);

	mutex_lock(&acrtc->cursor_lock);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	video_config_ok(adev, video_num, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], 0);
	reg_val &= ~(0xf << 1);

	/* set cursor layer info */
	reg_val &= ~(0xf << 16);
	switch (cursor->format) {
	case DRM_FORMAT_ARGB8888:
		reg_val |= (RGB8 << 16);
		break;
	default:
		DRM_ERROR("Unsupported screen format!\n");
		return false;
	}
	reg_val &= ~(1 << 5); /* 使用颜色数据自带透明度 */
	reg_val |= (layer_num << 1); /* 层叠关系，光标图层处于顶层，和图层编号一致 */

	/* enable cursor layer */
	if (enable && !(reg_val & 0x1))
		reg_val |= (1 << 0);
	else if (!enable)
		reg_val &= ~(1 << 0);

	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

	reg_val = (width << 16) | height;
	dc_writel(adev,
		gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);
	dc_writel(adev,
		gdc_reg->video_reg[video_num].laydu[layer_num], 0x3ffffff);
	dc_writel(adev,
		gdc_reg->video_reg[video_num].laydv[layer_num], 0x3ffffff);
	dc_writel(adev,
		gdc_reg->video_reg[video_num].chnorder[layer_num], 0x3210);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].membase_low[layer_num]);
	if (addr_l != reg_val) {
		dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[layer_num], addr_l);
		dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[layer_num], addr_h);
		set_layer_cache(adev, link, layer_num);
	}

	video_config_ok(adev, video_num, 1);

	mutex_unlock(&acrtc->cursor_lock);

	return true;
}

bool cursor_layer_move(struct loonggpu_dc_crtc *crtc, struct dc_cursor_move *move)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	u32 link = crtc->resource->base.link;
	struct loonggpu_crtc *acrtc = adev->mode_info.crtcs[link];
	int x, y;
	int hot_x, hot_y;
	int width, height;
	int enable;
	int layer_num = USE_OVERLAY_NUM + 1;
	u32 reg_val = 0;
	int video_num;

	if (IS_ERR_OR_NULL(move))
		return false;

	if (link >= DC_DVO_MAXLINK)
		return false;

	mutex_lock(&acrtc->cursor_lock);

	x = move->x;
	y = move->y;
	hot_x = move->hot_x;
	hot_y = move->hot_y;
	enable = move->enable;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	DRM_DEBUG("cursor layer move enable:%d x:%d y:%d hot_x:%d hot_y:%d\n",
		 enable, x, y, hot_x, hot_y);

	video_config_ok(adev, video_num, 0);

	if (move->enable == false) {
		reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
		reg_val &= ~(1 << 0); /* close cursor layer */
		dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

		video_config_ok(adev, video_num, 1);

		mutex_unlock(&acrtc->cursor_lock);
		return true;
	}

	/* set position */
	reg_val = (x << 16) | y;
	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], reg_val);

	/* get layer size */
	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].picsize[layer_num]);
	width = reg_val >> 16;
	height = reg_val & 0xffff;
	DRM_DEBUG("cursor size width:%d height:%d\n", width, height);

	/* set hot point */
	if (hot_x || hot_y) {
		reg_val = (hot_x << 16) | hot_y;
		dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], reg_val);
		reg_val = ((width - hot_x) << 16) | (height - hot_y);
		dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	} else {
		dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);
		reg_val = ((width - hot_x) << 16) | (height - hot_y);
		dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
		dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	}

	video_config_ok(adev, video_num, 1);

	mutex_unlock(&acrtc->cursor_lock);

	return true;
}

void video_meta_set(struct loonggpu_device *adev)
{
	uint64_t meta_gpu_addr;

	adev->dc->meta_gpu_addr = loonggpu_bo_gpu_offset(adev->zip_meta.robj);
	meta_gpu_addr = adev->dc->meta_gpu_addr;

	dc_writel(adev, gdc_reg->unzip_reg.meta_base_l, (u32)meta_gpu_addr);
	dc_writel(adev, gdc_reg->unzip_reg.meta_base_h, (meta_gpu_addr >> 32));
	dc_writel(adev, gdc_reg->unzip_reg.meta_mask_l, (u32)adev->zip_meta.mask);
	dc_writel(adev, gdc_reg->unzip_reg.meta_mask_h, adev->zip_meta.mask >> 32);
	DRM_INFO("Config meta addr %#016llX\n", meta_gpu_addr);

	/* DC top not use DC mmu */
	dc_writel(adev, 0xfc00c, 0x10);
	/* DC unzip ctrl */
	dc_writel(adev, gdc_reg->unzip_reg.dc_zip_ctl, 0x80800113);

	DRM_INFO("set dc video meta OK!\n");
}

bool ls9a1000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing)
{
	/* 9A上一定要先配置接口的时钟再配置video, video依赖接口的时钟 */
	u32 link = crtc->resource->base.link;

	if (link >= 4)
		return false;

	dc_interface_pll_set(crtc, timing);

	return true;
}

/* 注册于crtc_timing_set函数指针，用于配置9A的显示流程，硬件的操作顺序由hdmi phy和hdmi开始，
   之后配置video,因为video依赖hdmi phy的时钟 */
bool video_timing_set(struct loonggpu_dc_crtc *dc_crtc, struct dc_timing_info *timing)
{
	struct loonggpu_device *adev = dc_crtc->dc->adev;
	int link = dc_crtc->resource->base.link;
	int video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	int i;
	int reg_val = 0;

	if (dc_crtc->timing->clock != timing->clock ||
	    dc_crtc->timing->htotal != timing->htotal) {
		reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[0]);
		video_reset(adev, link, video_num);

		/* config hdmi&dp phy */
		dc_crtc->dc->hw_ops->dc_pll_set(dc_crtc, timing); //video时钟来自phy,pll配置函数可以合并

		/* video时序配置 */
		if (reg_val) {
			dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[0], 0);
			dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[0], reg_val);
		}

		video_mode_config(dc_crtc, timing);

		/* hdmi时序配置 */
		for (i = 0; i < dc_crtc->interfaces; i++) {
			if (dc_crtc->intf[i].connected && dc_crtc->intf[i].type == INTERFACE_HDMI)
				ls9a1000_hdmi_timing_set(dc_crtc, timing, dc_crtc->intf[i].index);
		}
	}

	memcpy(dc_crtc->timing, timing, sizeof(struct dc_timing_info));
	return 0;
}

void ls9a1000_dc_device_init(struct loonggpu_device *adev)
{
	int i;
	struct loonggpu_dc_video *dc_video;

	for (i = 0; i < adev->dc->links; i++) {
		int reg_val = dc_readl(adev, gdc_reg->video_reg[i].cfginfo);
		if (reg_val) {
			DRM_INFO("reset video %d\n", i);
			dc_writel(adev, gdc_reg->video_reg[i].intinfo, 0x80);
			dc_writel(adev, gdc_reg->video_reg[i].cfginfo, 0x38);
		}
	}

	/* 初始化dc_video结构体成员以及图层 */
	for (i = 0; i < adev->dc->links; i++) {
		dc_video = &adev->dc->link_info[i].crtc->dc_video;
		init_completion(&dc_video->reset_complete);

		dc_video->video_num = adev->dc->link_info[i].crtc->resource->crtc_id;

		dc_video->has_vga = 0;
		dc_video->has_dvo = 0;

		video_config_ok(adev, dc_video->video_num, 0);
		dc_video->cfgok = 0;

		primary_layer_init(adev, i);
		overlay_layer_init(adev, i);
		cursor_layer_init(adev, i);
	}

	video_meta_set(adev);

	/* 初始化GPIO复用 */
	writel(0x55555555, adev->rmmio_sc + 0x10);
	writel(0x55555555, adev->rmmio_sc + 0x14);
	writel(0x55555555, adev->rmmio_sc + 0x18);
	writel(0x55555555, adev->rmmio_sc + 0x1c);
}

/* DC 的测试项 通过ioctl由用户态调用 */
int video_gamma_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_video_gamma *gamma = data;
	struct drm_crtc *crtc;
	u32 ret = 0;
	u32 i;
	u32 link;
	u32 reg_val;
	int gam = 0;
	int video_num;

	crtc = drm_crtc_find(dev, filp, gamma->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", gamma->crtc_id);
		return -ENOENT;
	}

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;

	video_config_ok(adev, video_num, 0);

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val |= (1 << 2) | (1 << 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	while(gam < 980) {
		for(i = 0; i < 1024; i++){
			dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table,
				  ((gam << 20) | (gam << 10) | gam));
		}

		i += 0x1000;
		while(i--);
		gam++;
	}

	if (gamma->gamma_index == 0) {
		for (i = 0; i < 1024; i++)
			dc_writel(adev, gdc_reg->video_reg[video_num].gamma_table, 0);
	}

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].cfginfo);
	reg_val &= ~(1 << 2);
	dc_writel(adev, gdc_reg->video_reg[video_num].cfginfo, reg_val);

	video_config_ok(adev, video_num, 1);

	return ret;
}

int layer_display_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_layer_display *layer_display = data;
	struct drm_framebuffer *fb;
	struct drm_crtc *crtc;
	struct loonggpu_framebuffer *afb;
	struct drm_gem_object *obj;
	struct loonggpu_bo *rbo;
	u32 ret = 0;
	u32 link;
	u32 layer_num;
	u32 reg_val;
	int video_num;

	crtc = drm_crtc_find(dev, filp, layer_display->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", layer_display->crtc_id);
		return -ENOENT;
	}
//	DRM_INFO("[CRTC:%d:%s] index:%d\n", crtc->base.id, crtc->name, crtc->index);

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	layer_num = layer_display->layer_num;
//	DRM_INFO("%s link:%d video_num:%d layer_num:%d\n", __func__, link, video_num, layer_num);

	fb = drm_framebuffer_lookup(dev, filp, layer_display->fb_id);
	if (!fb) {
		DRM_INFO("error fb id:%d\n", layer_display->fb_id);
		return -ENOENT;
	}

	afb = to_loonggpu_framebuffer(fb);
	obj = fb->obj[0];
	rbo = gem_to_loonggpu_bo(obj);
	ret = loonggpu_bo_reserve(rbo, false);
	if (unlikely(ret != 0))
		return ret;

	ret = loonggpu_bo_pin(rbo, LOONGGPU_GEM_DOMAIN_VRAM);
	if (unlikely(ret != 0)) {
		loonggpu_bo_unreserve(rbo);
		return ret;
	}

	afb->address = loonggpu_bo_gpu_offset(rbo);
//	DRM_INFO("%s fb_id:%d gpu address:%#llX\n", __func__, layer_display->fb_id, afb->address);
//	DRM_INFO("%s width:%d height:%d\n", __func__, layer_display->l_width, layer_display->l_height);

	video_config_ok(adev, video_num, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[layer_num], 0);

	reg_val = 0;
	reg_val |= (RGB8 << 16) |
		   (layer_display->format << 14) |
		   (0xff << 6) | (1 << 5) |
		   (1 << 1) | (1 << 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0x3ffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0x3ffffff);
	reg_val = (layer_display->l_width << 16) | layer_display->l_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);

	reg_val = (layer_display->x << 16) | layer_display->y;
	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], reg_val);

	reg_val = (3 << 12) | (2 << 8) | (1 << 4) | (0 << 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].chnorder[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].membase_low[layer_num], afb->address & 0xffffffff);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig[layer_num], afb->address >> 32);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_low_uv[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].membase_hig_uv[layer_num], 0);

	set_layer_cache(adev, link, layer_display->layer_num);

	video_config_ok(adev, video_num, 1);

	loonggpu_bo_unreserve(rbo);
	loonggpu_bo_ref(rbo);
	drm_framebuffer_put(fb);

	return ret;
}

static int video_zoom_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_video_zoom *video_zoom = data;
	struct drm_crtc *crtc;
	u32 ret = 0;
	u32 link;
	u32 reg_val;
	int video_num;
	int layer_num = video_zoom->layer_num;

	crtc = drm_crtc_find(dev, filp, video_zoom->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", video_zoom->crtc_id);
		return -ENOENT;
	}
	DRM_INFO("[CRTC:%d:%s] index:%d\n", crtc->base.id, crtc->name, crtc->index);

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	DRM_INFO("%s back_width:%d back_height:%d scr_width:%d scr_height:%d\n", __func__,
		video_zoom->back_width, video_zoom->sam_height, video_zoom->l_width, video_zoom->l_height);

	video_config_ok(adev, video_num, 0);

	DRM_INFO("du:%#X dv:%#X\n", video_zoom->du, video_zoom->dv);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], video_zoom->du);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], video_zoom->dv);

	reg_val = (video_zoom->l_width << 16) | video_zoom->l_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);

	reg_val = (video_zoom->sam_width << 16) | video_zoom->sam_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], (100 << 16)|100);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);

	video_config_ok(adev, video_num, 1);

	return ret;
}

int layer_zoom_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_layer_zoom *layer_zoom = data;
	struct drm_crtc *crtc;
	u32 ret = 0;
	u32 link;
	u32 layer_num;
	u32 reg_val;
	int video_num;
	u32 du, dv;

	crtc = drm_crtc_find(dev, filp, layer_zoom->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", layer_zoom->crtc_id);
		return -ENOENT;
	}

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	layer_num = layer_zoom->layer_num;

	du = div64_u64((u64)(layer_zoom->sam_width - 1) * 0x3ffffff, (u64)(layer_zoom->l_width - 1));
	dv = div64_u64((u64)(layer_zoom->sam_height - 1) * 0x3ffffff, (u64)(layer_zoom->l_height - 1));

	video_config_ok(adev, video_num, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], 0);

	dc_writel(adev, gdc_reg->video_reg[video_num].laydu[layer_num], du);
	dc_writel(adev, gdc_reg->video_reg[video_num].laydv[layer_num], dv);

	reg_val = (layer_zoom->l_width << 16) | layer_zoom->l_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);

	reg_val = (layer_zoom->sam_width << 16) | layer_zoom->sam_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].samsize[layer_num], reg_val);
	dc_writel(adev, gdc_reg->video_reg[video_num].picsize[layer_num], reg_val);

	dc_writel(adev, gdc_reg->video_reg[video_num].laypoint[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].samoff[layer_num], 0);

	video_config_ok(adev, video_num, 1);

	return ret;
}

int layer_tile_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_layer_tile *layer_tile = data;
	struct drm_crtc *crtc;
	u32 ret = 0;
	u32 link;
	u32 layer_num;
	u32 reg_val;
	int video_num;

	crtc = drm_crtc_find(dev, filp, layer_tile->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", layer_tile->crtc_id);
		return -ENOENT;
	}

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	layer_num = layer_tile->layer_num;

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);
	video_config_ok(adev, video_num, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], 0);

	switch (layer_tile->tile_mode) {
	case 0:
		reg_val &= ~(3 << 14);
		break;
	case 1: //tile4
		reg_val &= ~(3 << 14);
		reg_val |= (2 << 14); /* tile24 */
		break;
	}

	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

	set_layer_cache(adev, link, layer_num);

	video_config_ok(adev, video_num, 1);

	return ret;
}

int layer_rotate_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct drm_loonggpu_layer_rotate *layer_rotate = data;
	struct drm_crtc *crtc;
	u32 ret = 0;
	u32 link;
	u32 video_num;
	u32 layer_num;
	u32 reg_val;
	u32 angle;

	crtc = drm_crtc_find(dev, filp, layer_rotate->crtc_id);
	if (!crtc) {
		DRM_INFO("Unknown CRTC ID %d\n", layer_rotate->crtc_id);
		return -ENOENT;
	}

	link = crtc->index;
	video_num = adev->dc->link_info[link].crtc->dc_video.video_num;
	layer_num = layer_rotate->layer_num;

	switch (layer_rotate->rotate_angle) {
	case 0:
		angle = 0;
		break;
	case 90:
		angle = 1;
		break;
	case 180:
		angle = 2;
		break;
	case 270:
		angle = 3;
		break;
	}

	reg_val = dc_readl(adev, gdc_reg->video_reg[video_num].layinfo[layer_num]);

	video_config_ok(adev, video_num, 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], 0);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], 0);

	reg_val &= ~(0x3 << 20);
	reg_val |= ((angle & 0x3)<< 20);
	dc_writel(adev, gdc_reg->video_reg[video_num].layinfo[layer_num], reg_val);

	reg_val = (layer_rotate->l_width << 16) | layer_rotate->l_height;
	dc_writel(adev, gdc_reg->video_reg[video_num].laysize[layer_num], reg_val);

	video_config_ok(adev, video_num, 1);

	return ret;
}

int dc_get_meta_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;
	void *meta_vaddr;
	struct file *file;
	const char *path = "/root/meta_data";
	char *buf;
	loff_t pos;
	ssize_t written;
	u32 ret = 0;

	loonggpu_bo_kmap(adev->zip_meta.robj, &meta_vaddr);
	buf = (char *)meta_vaddr;

	file = filp_open(path, O_CREAT | O_RDWR, 0644);
	if (IS_ERR(file)) {
		DRM_INFO("ERROR opening file %s\n", path);
		return -1;
	}

	file_start_write(file);
	pos = 0;
	written = kernel_write(file, buf, adev->zip_meta.mask, &pos);

	file_end_write(file);
	filp_close(file, NULL);

	if (written < 0) {
		DRM_INFO("Write error %ld\n", written);
		return -1;
	}

	DRM_INFO("Write %zd butes to %s\n", written, path);

	return ret;
}

int dc_set_meta_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct loonggpu_device *adev = dev->dev_private;

	memset(adev->zip_meta.ptr, 0x00, adev->zip_meta.table_size);

	DRM_INFO("Clear meta buffer\n");

	return 0;
}
