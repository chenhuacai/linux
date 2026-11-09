#ifndef __DC_VIDEO_H__
#define __DC_VIDEO_H__

#define USE_OVERLAY_NUM 0 //MAX 6

struct loonggpu_device;
struct dc_cursor_info;
struct dc_cursor_move;
struct dc_primary_plane;
struct dc_timing_info;

/* dc mod num */
enum dc_mod_num {
	DC_MOD_VIDEO_0,
	DC_MOD_VIDEO_1,
	DC_MOD_VIDEO_2,
	DC_MOD_VIDEO_3,
	DC_MOD_HDMI_0,
	DC_MOD_HDMI_1,
	DC_MOD_DP_0,
	DC_MOD_DP_1,
	DC_MOD_VGA,
	DC_MOD_DVO,
	DC_MOD_WB_0,
	DC_MOD_WB_1,
	DC_MOD_WB_2,
	DC_MOD_WB_3
};

/* layer info */
#define YUV10	4
#define YUV8	5
#define RGB8	8
#define RGB565	9
#define RGB4	10
#define RGB5	11
#define RGB10	12
#define RG8	13

#define LINER	0
#define TILE4	1
#define TILE24	2
#define TILE24n	3

#define ROT0	0
#define ROT90	1
#define ROT180	2
#define ROT270	3

/* unzip mode */
#define VIDEO_NO_COMPRESSED 0
#define VIDEO_COMPRESSED_R8G8B8A8 1
#define VIDEO_COMPRESSED_R10G10B10A2 2
#define VIDEO_COMPRESSED_A2R10G10B10 3
#define VIDEO_COMPRESSED_D24S8 4
#define VIDEO_COMPRESSED_S8D24 5

/* irq msg id */
#define IRQ_MSG_VIDEO_0	0
#define IRQ_MSG_VIDEO_1	1
#define IRQ_MSG_VIDEO_2	2
#define IRQ_MSG_VIDEO_3	3
#define IRQ_MSG_HDMI_0	4
#define IRQ_MSG_HDMI_1	5
#define IRQ_MSG_DP_0	6
#define IRQ_MSG_DP_1	7
#define IRQ_MSG_VGA	8
#define IRQ_MSG_DVO	9
#define IRQ_MSG_WB_0	10
#define IRQ_MSG_WB_1	11
#define IRQ_MSG_WB_2	12
#define IRQ_MSG_WB_3	13

/* video irq num */
enum video_irq_source {
	VIDEO_IRQ_ERROVL,
	VIDEO_IRQ_HSYNC,
	VIDEO_IRQ_VSYNC,
	VIDEO_IRQ_MEM_ACC_DONE,
	VIDEO_IRQ_FRAME_DONE,
	VIDEO_IRQ_CFGMISS,
	VIDEO_IRQ_CFGREQ,
	VIDEO_IRQ_SOFT_RST_DONE,
	VIDEO_IRQ_DSP_ERR,
	VIDEO_IRQ_SOURCES_NUMBER
};

/* hpd irq num */
enum hpd_irq_source {
	HPD_IRQ_HDMI0,
	HPD_IRQ_HDMI1,
	HPD_IRQ_DP0,
	HPD_IRQ_DP1,
	HPD_IRQ_VGA,
	HPD_IRQ_DVO,
	HPD_IRQ_SOURCES_NUMBER
};

struct layer_config {
	int cache_addr_start;
	int cache_addr_end;
	int cache_line_num;
	int uv_cache_addr_start;
	int uv_cache_addr_end;
	int uv_cache_line_num;
};

struct loonggpu_dc_video {
	int video_num; /* 保存video的硬件编号0-3 */
	int hdmi_num; /* hdmi接口的硬件编号0-1 初值为0xf */
	int dp_num; /* dp接口的硬件编号0-1 初值为0xf */
	bool has_dvo; /* 初始化时写成0 */
	bool has_vga; /* 初始化时写成0 */
	int vga_connected; /* VGA连接状态 */
	struct completion reset_complete; /* 软复位完成的中断信号 */
	struct layer_config layer_config[8];
	int cfgok;
	uint32_t edid_length;
	uint8_t raw_edid[256];
	int hdmi_irq_en;
	int dp_hpd_enable;
	spinlock_t hpd_lock;
	bool scdc_scrambling_supported;
};

bool ls9a1000_dc_pll_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
bool video_timing_set(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
void ls9a1000_dc_device_init(struct loonggpu_device *adev);

bool video_enable(struct loonggpu_dc_crtc *dc_crtc, bool enable);
void video_meta_set(struct loonggpu_device *adev);

bool video_mode_config(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);

bool primary_layer_config(struct loonggpu_dc_crtc *crtc, struct dc_primary_plane *primary);

u32 video_get_syncnt(struct loonggpu_device *adev, int link);
int video_get_scanoutpos(struct loonggpu_device *adev, int crtc_num,
				  u32 *vbl, u32 *position);

int video_gamma_set(struct drm_crtc *crtc, u16 *red, u16 *green, u16 *blue);

/* cursor */
bool cursor_layer_set(struct loonggpu_dc_crtc *crtc, struct dc_cursor_info *cursor);
bool cursor_layer_move(struct loonggpu_dc_crtc *crtc, struct dc_cursor_move *move);

int video_gamma_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int layer_display_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int layer_zoom_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int layer_tile_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int layer_rotate_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int dc_get_meta_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int dc_set_meta_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);

#endif /* __DC_VIDEO_H__ */
