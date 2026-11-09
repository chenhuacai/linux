#ifndef __DC_INTERFACE_H__
#define __DC_INTERFACE_H__

#include "loonggpu_dc_irq.h"
#include "loonggpu_dc_i2c.h"
#include "loonggpu_ih.h"
#include "loonggpu_dc_interface.h"
#include "loonggpu_dc_helper.h"

#define DC_VER "1.0"
#define DC_DVO_MAXLINK 4
#define VRAM_OVERFLOW  0x1000000

extern const struct loonggpu_ip_block_version dc_ip_block;

struct loonggpu_device;
struct drm_device;
struct dc_cursor_info;
struct loonggpu_bridge_phy;
struct dc_primary_plane;
struct loonggpu_crtc;

struct dc_cursor_position {
	uint32_t x;
	uint32_t y;
	uint32_t x_hotspot;
	uint32_t y_hotspot;
	/* This parameter indicates whether HW cursor should be enabled */
	bool enable;
};

struct irq_list_head {
	struct list_head head;
	/* In case this interrupt needs post-processing, 'work' will be queued*/
	struct work_struct work;
};

/*
 * Software debounce state for the HDMI hotplug irq.
 *
 * The HDMI HPD line bounces during plug/unplug, which triggers an irq
 * storm. Every irq re-arms the delayed work (DC_HPD_DEBOUNCE_MS), so the
 * work only runs after the line has been quiet for the debounce window.
 * It then re-reads the hdmi_hp_stat register to confirm the stable status
 * and only dispatches the real hotplug handler when the status actually
 * changed, filtering out debounced glitches.
 */
struct dc_hpd_debounce {
	struct delayed_work work;
	struct loonggpu_device *adev;
	enum dc_irq_source irq_source;
	/* Last debounced/processed status, connector_status_unknown at first */
	int last_status;
};

struct loonggpu_link_info {
	bool fine;
	struct loonggpu_dc_crtc *crtc;
	struct loonggpu_dc_i2c *i2c;
	struct loonggpu_dc_encoder *encoder;
	struct connector_resource *connector;
	struct loonggpu_dc_bridge *bridge;
};

struct dc_cursor_move {
	u32 hot_y;
	u32 hot_x;
	u32 x, y;
	bool enable;
};

union plane_address {
	struct {
		u32 low_part;
		u32 high_part;
	};
	uint64_t raw;
};

struct dc_timing_info {
	u32 vrefresh;
	s32 clock;		/* from drm_display_mode::clock*/
	s32 hdisplay;
	s32 hsync_start;
	s32 hsync_end;
	s32 htotal;
	s32 vdisplay;
	s32 vsync_start;
	s32 vsync_end;
	s32 vtotal;
	u32 stride;
	u32 depth;
	u32 use_dma;
	u32 fixed_vsync_start;
	u32 fixed_vsync_end;
	u32 array_mode;
};

struct dc_sw_ops {

	/* dc operations */
	void (*dc_topology_init)(struct loonggpu_dc_crtc *crtc);
	int (*dc_irq_sw_init)(struct loonggpu_device *adev);
};

struct dc_hw_ops {

	/* dc operations */
	bool (*dc_pll_set)(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
	int (*dc_irq_hw_init)(struct loonggpu_device *adev);
	int (*dc_irq_hw_uninit)(struct loonggpu_device *adev);
	void (*dc_irq_fini)(struct loonggpu_device *adev);
	int (*dc_i2c_init)(struct loonggpu_device *adev, uint32_t link_index);
	void (*dc_i2c_resume)(struct loonggpu_device *adev, uint32_t link_index);
	bool (*dc_hpd_enable)(struct loonggpu_device *adev, uint32_t link, bool enable);
	int (*dc_get_modes)(struct loonggpu_bridge_phy *phy, int used_method,
					struct drm_connector *connector, struct edid *edid);
	void (*dc_device_init)(struct loonggpu_device *adev);
	void (*dc_device_fini)(struct loonggpu_device *adev);
	void (*manage_dc_int)(struct loonggpu_device *adev, struct loonggpu_crtc *acrtc, bool enable);

	/* crtc operations */
	bool (*crtc_enable)(struct loonggpu_dc_crtc *crtc, bool enable);
	bool (*crtc_timing_set)(struct loonggpu_dc_crtc *crtc, struct dc_timing_info *timing);
	void (*crtc_cfg_adjust)(u32 array_mode, u32 *crtc_cfg);
	int (*crtc_set_vblank)(struct drm_crtc *crtc, bool enable);
	bool (*crtc_plane_set)(struct loonggpu_dc_crtc *crtc, struct dc_primary_plane *primary);
	int (*crtc_gamma_set)(struct drm_crtc *crtc, u16 *red, u16 *green, u16 *blue);

	/* cursor operations*/
	bool (*cursor_move)(struct loonggpu_dc_crtc *crtc, struct dc_cursor_move *move);
	bool (*cursor_set)(struct loonggpu_dc_crtc *crtc, struct dc_cursor_info *cursor);

	/* Interfaces operations */
	/* HDMI */
	bool (*hdmi_enable)(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
	int (*hdmi_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*hdmi_audio_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*hdmi_noaudio_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*hdmi_resume)(struct loonggpu_dc_crtc *crtc, int intf);
	void (*hdmi_suspend)(struct loonggpu_dc_crtc *crtc, int intf);
	void (*hdmi_pll_set)(struct loonggpu_dc_crtc *crtc, int intf, int clock);
	void (*hdmi_i2c_set)(struct loonggpu_dc_crtc *crtc, int intf, bool use_gpio_i2c);

	/* DP */
	bool (*dp_enable)(struct loonggpu_dc_crtc *crtc, int intf, bool enable);
	int (*dp_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*dp_audio_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*dp_noaudio_init)(struct loonggpu_dc_crtc *crtc, int intf);
	int (*dp_resume)(struct loonggpu_dc_crtc *crtc, int intf);
	void (*dp_suspend)(struct loonggpu_dc_crtc *crtc, int intf);
	void (*dp_hpd_handler)(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry);
	void (*dp_pll_set)(struct loonggpu_dc_crtc *crtc, int intf, struct dc_timing_info *timing);
	void (*first_hpd_detect) (struct loonggpu_dc_crtc *crtc, int intf);
	bool (*dc_hpd_ack) (struct loonggpu_dc_crtc *crtc);
	bool (*dc_i2c_ack) (struct loonggpu_dc_crtc *crtc);
	bool (*dc_crtc_vblank_ack) (struct loonggpu_dc_crtc *crtc);
	u32 (*dc_vblank_get_counter) (struct loonggpu_device *adev, int crtc_num);
	bool (*interface_status_changed)(struct drm_connector *connector, struct loonggpu_dc_crtc *crtc);
};

struct display_bo {
	u32 pitch;
	u32 cpp;
	u64 gpu_addr;
	u64 *cpu_ptr;
	struct loonggpu_bo *handle;
	unsigned int size;
};

struct loonggpu_dc {
	struct drm_device *ddev;
	struct loonggpu_device *adev;
	struct loonggpu_vbios *vbios;
	struct list_head crtc_list;
	struct list_head encoder_list;
	struct loonggpu_link_info *link_info;

	struct drm_display_mode native_mode;
	struct display_bo disp_bo[2];
	struct display_bo *scanout_bo;
	u32 scanout_id;

	/* base information */
	int chip;
	int ip_version;
	int hw_version;
	int links;
	int max_cursor_size;

	/* DC meta */
	struct loonggpu_bo *meta_bo;
	uint64_t meta_gpu_addr;
	void *meta_cpu_addr;

	/**
	 * Caches device atomic state for suspend/resume
	 */
	struct lg_drm_atomic_state *cached_state;

	spinlock_t irq_handler_list_table_lock;
	struct irq_list_head irq_handler_list_low_tab[DC_IRQ_SOURCES_NUMBER];
	struct list_head irq_handler_list_high_tab[DC_IRQ_SOURCES_NUMBER];

	/* HDMI hotplug irq software debounce state */
	struct dc_hpd_debounce hpd_debounce[DC_IRQ_SOURCES_NUMBER];

	/* DC related hw operations */
	const struct dc_hw_ops *hw_ops;
	const struct dc_sw_ops *sw_ops;
};

u32 dc_readl(struct loonggpu_device *adev, u32 reg);
void dc_writel(struct loonggpu_device *adev, u32 reg, u32 val);
void dc_writel_check(struct loonggpu_device *adev, u32 link, u32 reg, u32 val);
u32 dc_readl_locked(struct loonggpu_device *adev, u32 reg);
void dc_writel_locked(struct loonggpu_device *adev, u32 reg, u32 val);

struct display_bo *loonggpu_dc_get_disp_bo(struct loonggpu_device *adev);
int loonggpu_dc_scale_init(struct loonggpu_device *adev);
int loonggpu_dc_scale_fini(struct loonggpu_device *adev);

bool dc_submit_timing_update(struct loonggpu_dc *dc, u32 link, struct dc_timing_info *timing);
void handle_cursor_update(struct drm_plane *plane,
			  struct drm_plane_state *old_plane_state);
bool dc_cursor_set(struct loonggpu_dc_crtc *crtc, struct dc_cursor_info *cursor);
bool dc_cursor_move(struct loonggpu_dc_crtc *crtc, struct dc_cursor_move *move);
void dc_set_dma_consistent(struct loonggpu_device *adev);
#endif
