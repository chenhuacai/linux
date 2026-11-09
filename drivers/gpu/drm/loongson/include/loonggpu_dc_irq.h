#ifndef __LOONGGPU_DM_IRQ_H__
#define __LOONGGPU_DM_IRQ_H__

#define DC_INT_I2C0_EN BIT(27)
#define DC_INT_I2C1_EN BIT(28)
#define DC_INT_HDMI0_HOTPLUG_EN BIT(29)
#define DC_INT_HDMI1_HOTPLUG_EN BIT(30)
#define DC_INT_VGA_HOTPLUG_EN BIT(31)
#define DC_VGA_HPD_STATUS_MASK 0x3
#define LS_FB_VSYNC0_INT (1 << 2)
#define LS_FB_HDMI0_INT (1 << 13)
#define LS_FB_HDMI1_INT (1 << 14)
#define LS_FB_VGA_INT (1 << 15)

struct loonggpu_dc_crtc;
struct loonggpu_dc;
typedef void (*interrupt_handler)(void *);
typedef void *irq_handler_idx;
#define DAL_INVALID_IRQ_HANDLER_IDX NULL

#define DC_INT_ID_VSYNC1 1
#define DC_INT_ID_HSYNC1 2
#define DC_INT_ID_VSYNC0 3
#define DC_INT_ID_HSYNC0 4
#define DC_INT_ID_CURSOR_READEND 5
#define DC_INT_ID_READEND_FB1 6
#define DC_INT_ID_READEND_FB0 7
#define DC_INT_ID_UNDERFLOW_DB1 8
#define DC_INT_ID_UNDERFLOW_DB0 9
#define DC_INT_ID_FUNDERFLOW_DB1 10
#define DC_INT_ID_FUNDERFLOW_DB0 11
#define DC_INT_ID_I2C0 12
#define DC_INT_ID_I2C1 13
#define DC_INT_ID_HPD_HDMI0 14
#define DC_INT_ID_HPD_HDMI1 15
#define DC_INT_ID_HPD_VGA 16
#define DC_INT_ID_HPD_EDP 17
#define DC_INT_ID_HPD_DP 18
#define DC_INT_ID_MAX 19

enum dc_irq_source {
	DC_IRQ_SOURCE_INVALID = 0,

	DC_IRQ_SOURCE_VSYNC0,
	DC_IRQ_SOURCE_VSYNC1,

	DC_IRQ_SOURCE_I2C0,
	DC_IRQ_SOURCE_I2C1,

	DC_IRQ_SOURCE_HPD_HDMI0,
	DC_IRQ_SOURCE_HPD_HDMI1,
	DC_IRQ_SOURCE_HPD_VGA,

	DC_IRQ_SOURCE_HPD_HDMI0_NULL,
	DC_IRQ_SOURCE_HPD_HDMI1_NULL,

	DC_IRQ_SOURCE_HPD_EDP,
	DC_IRQ_SOURCE_HPD_DP,
	DC_IRQ_SOURCE_HPD_EDP_NULL,
	DC_IRQ_SOURCE_HPD_DP_NULL,
	DC_IRQ_SOURCES_NUMBER
};

enum dp_hpd_irq_type {
	DP_HPD_IN = 0x1,
	DP_HPD_OUT = 0x2,
	EDP_HPD_IN = 0x10,
	EDP_HPD_OUT = 0x20,
};

enum irq_type {
	DC_IRQ_TYPE_VSYNC = DC_IRQ_SOURCE_VSYNC0,
	DC_IRQ_TYPE_I2C = DC_IRQ_SOURCE_I2C0,
	DC_IRQ_TYPE_HPD = DC_IRQ_SOURCE_HPD_HDMI0,
};

#define DC_VALID_IRQ_SRC_NUM(src) \
	((src) <= DC_IRQ_SOURCES_NUMBER && (src) > DC_IRQ_SOURCE_INVALID)

enum dc_interrupt_context {
	INTERRUPT_LOW_IRQ_CONTEXT = 0,
	INTERRUPT_HIGH_IRQ_CONTEXT,
	INTERRUPT_CONTEXT_NUMBER
};

struct dc_interrupt_params {
	enum dc_irq_source irq_source;
	enum dc_interrupt_context int_context;
};

struct dc_irq_handler_data {
	struct list_head list;
	interrupt_handler handler;
	void *handler_arg;
	enum dc_irq_source irq_source;
};

int dc_irq_sw_init(struct loonggpu_device *adev);
int dc_irq_hw_init(struct loonggpu_device *adev);
int dc_irq_hw_uninit(struct loonggpu_device *adev);
void dc_irq_fini(struct loonggpu_device *adev);
bool dc_hpd_ack(struct loonggpu_dc_crtc *crtc);
bool ls7a1000_dc_hpd_ack(struct loonggpu_dc_crtc *crtc);
bool dc_i2c_ack(struct loonggpu_dc_crtc *crtc);
bool ls7a1000_dc_i2c_ack(struct loonggpu_dc_crtc *crtc);
bool dc_crtc_vblank_ack(struct loonggpu_dc_crtc *crtc);
bool ls7a1000_dc_crtc_vblank_ack(struct loonggpu_dc_crtc *crtc);
bool ls7a2000_dc_hpd_enable(struct loonggpu_device *adev, uint32_t link, bool enable);
bool ls2k3000_dc_hpd_enable(struct loonggpu_device *adev, uint32_t link, bool enable);

void dc_handle_vsync_irq(void *interrupt_params);
int dc_irq_msg_sw_init(struct loonggpu_device *adev);
int dc_irq_msg_hw_init(struct loonggpu_device *adev);
int dc_irq_msg_hw_uninit(struct loonggpu_device *adev);
void dc_irq_msg_fini(struct loonggpu_device *adev);
int get_irq_msg_type(int mod_num, int irq_num);
int video_set_vblank(struct drm_crtc *crtc, bool enable);

#endif /* __LOONGGPU_DM_IRQ_H__ */
