#ifndef __BRIDGE_PHY_H__
#define __BRIDGE_PHY_H__

#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/hdmi.h>
#include <drm/drm_edid.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <video/videomode.h>
#include "loonggpu.h"
#include "loonggpu_dc_resource.h"

#define NAME_SIZE_MAX 50U
#define LS7A_GPIO_OFFSET 16U

#define to_bridge_phy(drm_bridge)                                            \
	container_of(drm_bridge, struct loonggpu_bridge_phy, bridge)

struct loonggpu_bridge_phy;

struct special_display {
    const char *manufacturer;
    u16 product_code;
    u32 serial;
    void (*sd_func)(struct loonggpu_device *adev, u32 link, struct dc_timing_info *timing);
};

struct loonggpu_dc_bridge {
	struct loonggpu_device *adev;
	struct loonggpu_bridge_phy *internal_bp;
	struct loonggpu_dc *dc;
	struct list_head node;
	int display_pipe_index;
	int encoder_obj;
	char chip_name[NAME_SIZE_MAX];
	char vendor_str[NAME_SIZE_MAX];
	unsigned int i2c_bus_num;
	unsigned short i2c_dev_addr;
	unsigned short hotplug;
	unsigned short edid_method;
	unsigned int irq_gpio;
	unsigned int gpio_placement;
};

enum encoder_type {
	encoder_none,
	encoder_dac,
	encoder_tmds,
	encoder_lvds,
	encoder_tvdac,
	encoder_virtual,
	encoder_dsi,
	encoder_dpmst,
	encoder_dpi,
};

enum hpd_status {
	hpd_status_plug_off = 0,
	hpd_status_plug_on = 1,
};

enum int_type {
	interrupt_all = 0,
	interrupt_hpd = 1,
	interrupt_max = 0xff,
};

struct reg_mask_seq {
	unsigned int reg;
	unsigned int mask;
	unsigned int val;
};

#define encoder_type_to_str(index)\
	(index == encoder_none ? "none" :\
	(index == encoder_dac ? "dac" :\
	(index == encoder_tmds ? "tmds" :\
	(index == encoder_lvds ? "lvds" :\
	(index == encoder_virtual ? "virtual" :\
	(index == encoder_dsi ? "dsi" :\
	(index == encoder_dpmst ? "dpmst" :\
	(index == encoder_dpi ? "dpi" :\
	"other"))))))))

#define hotplug_to_str(index)\
	(index == FORCE_ON ? "connected" :\
	(index == POLLING ? "polling" :\
	(index == IRQ ? "irq" :\
	"Unknown")))

enum encoder_config {
	encoder_transparent = 0,
	encoder_os_config, /* vbios_config */
	encoder_bios_config,
	encoder_timing_filling, /* legacy */
	encoder_kernel_driver,  /* Driver */
	encoder_type_max = 0xffffffff,
} __packed;

#define encoder_config_to_str(index)\
	(index == encoder_transparent ? "transparent" :\
	(index == encoder_os_config ? "os" :\
	(index == encoder_bios_config ? "bios" :\
	(index == encoder_timing_filling ? "timing" :\
	(index == encoder_kernel_driver ? "kernel" :\
	"Unknown")))))

#define edid_method_to_str(index)\
	(index == via_null ? "null" :\
	(index == via_i2c ? "i2c" :\
	(index == via_vbios ? "vbios" :\
	(index == via_encoder ? "encoder" :\
	"Unknown"))))

enum input_signal_sample_type {
	SDR_CLK = 0,
	DDR_CLK,
};

struct ddc_status {
	struct mutex ddc_bus_mutex;
	bool ddc_bus_idle;
	bool ddc_bus_error;
	bool ddc_fifo_empty;
};

struct bridge_phy_mode_config {
	bool edid_read;
	u8 edid_buf[256];
	union hdmi_infoframe hdmi_frame;
	struct {
		enum input_signal_sample_type input_signal_type;
		bool gen_sync;
		struct drm_display_mode *mode;
		struct videomode vmode;
	} input_mode;
};

struct bridge_phy_cfg_funcs {
	int (*reg_init)(struct loonggpu_bridge_phy *phy);
	int (*hw_reset)(struct loonggpu_bridge_phy *phy);
	int (*sw_enable)(struct loonggpu_bridge_phy *phy);
	int (*sw_reset)(struct loonggpu_bridge_phy *phy);
	int (*suspend)(struct loonggpu_bridge_phy *phy);
	int (*resume)(struct loonggpu_bridge_phy *phy);
	void (*prepare)(struct loonggpu_bridge_phy *phy);
	void (*commit)(struct loonggpu_bridge_phy *phy);
	int (*backlight_ctrl)(struct loonggpu_bridge_phy *phy, int mode);
	int (*video_input_cfg)(struct loonggpu_bridge_phy *phy);
	int (*video_input_check)(struct loonggpu_bridge_phy *phy);
	int (*video_output_cfg)(struct loonggpu_bridge_phy *phy);
	int (*video_output_timing)(struct loonggpu_bridge_phy *phy,
				   const struct drm_display_mode *mode);
	int (*hdmi_output_mode)(struct loonggpu_bridge_phy *phy, int mode);
	int (*hdmi_audio)(struct loonggpu_bridge_phy *phy);
	int (*hdmi_csc)(struct loonggpu_bridge_phy *phy);
	int (*hdmi_hdcp_init)(struct loonggpu_bridge_phy *phy);
	int (*afe_high)(struct loonggpu_bridge_phy *phy);
	int (*afe_low)(struct loonggpu_bridge_phy *phy);
	int (*afe_set_tx)(struct loonggpu_bridge_phy *phy, bool enable);
	int (*mode_set_pre)(struct drm_bridge *bridge,
			    const struct drm_display_mode *mode,
			    const struct drm_display_mode *adj_mode);
	int (*mode_set)(struct loonggpu_bridge_phy *phy,
			const struct drm_display_mode *mode,
			const struct drm_display_mode *adj_mode);
	int (*mode_set_post)(struct drm_bridge *bridge,
			     const struct drm_display_mode *mode,
			     const struct drm_display_mode *adj_mode);
	enum drm_mode_status (*mode_valid)(struct drm_connector *connector,
					   struct drm_display_mode *mode);
};

struct bridge_phy_misc_funcs {
	bool (*chip_id_verify)(struct loonggpu_bridge_phy *phy, char *id);
	int (*debugfs_init)(struct loonggpu_bridge_phy *phy);
	void (*dpms_ctrl)(struct loonggpu_bridge_phy *phy, int mode);
};

struct bridge_phy_hpd_funcs {
	void (*hpd_init)(struct loonggpu_bridge_phy *phy, struct loonggpu_connector * lconnector, bool has_ext_encoder);
	enum hpd_status (*get_hpd_status)(struct loonggpu_bridge_phy *phy);
	enum drm_connector_status (*get_connect_status)(struct loonggpu_bridge_phy *phy);
	int (*int_enable)(struct loonggpu_bridge_phy *phy,
			  enum int_type interrut);
	int (*int_disable)(struct loonggpu_bridge_phy *phy,
			   enum int_type interrut);
	int (*int_clear)(struct loonggpu_bridge_phy *phy, enum int_type interrut);
	irqreturn_t (*irq_handler)(int irq, void *dev);
	irqreturn_t (*isr_thread)(int irq, void *dev);
};

struct bridge_phy_ddc_funcs {
	int (*ddc_fifo_fetch)(struct loonggpu_bridge_phy *phy, u8 *buf, u8 block,
			      size_t len, size_t offset);
	int (*ddc_fifo_abort)(struct loonggpu_bridge_phy *phy);
	int (*ddc_fifo_clear)(struct loonggpu_bridge_phy *phy);
	int (*get_edid_block)(void *data, u8 *buf, unsigned int block,
			      size_t len);
	int (*get_modes)(struct loonggpu_bridge_phy *phy,
			 struct drm_connector *connector);
};

struct bridge_phy_hdmi_aux_funcs {
	int (*set_gcp_avmute)(struct loonggpu_bridge_phy *phy, bool enable,
			      bool blue_screen);
	int (*set_avi_infoframe)(struct loonggpu_bridge_phy *phy,
				 const struct drm_display_mode *mode);
	int (*set_hdcp)(struct loonggpu_bridge_phy *phy);
};

struct loonggpu_bridge_phy {
	int display_pipe_index;
	struct drm_bridge bridge;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	enum drm_connector_status status;

	struct loonggpu_dc_bridge *res;
	struct loonggpu_device *adev;
	struct loonggpu_dc_i2c *li2c;
	struct i2c_client *i2c_phy;
	struct regmap *phy_regmap;

	void *priv;
	u8 chip_version;
	bool is_ext_encoder;
	u32 connector_type;

	u8 sys_status;
	int irq_num;
	atomic_t irq_status;
	struct ddc_status ddc_status;
	struct bridge_phy_mode_config mode_config;
	struct bridge_phy_helper *helper;
	const struct regmap_config *regmap_cfg;
	const struct bridge_phy_misc_funcs *misc_funcs;
	const struct bridge_phy_cfg_funcs *cfg_funcs;
	const struct bridge_phy_hpd_funcs *hpd_funcs;
	const struct bridge_phy_ddc_funcs *ddc_funcs;
	const struct bridge_phy_hdmi_aux_funcs *hdmi_aux_funcs;
};

struct loonggpu_dc_bridge
*dc_bridge_construct(struct loonggpu_dc *dc,
		     struct encoder_resource *encoder_res,
		     struct connector_resource *connector_res);
int loonggpu_dc_bridge_init(struct loonggpu_device *adev, int link_index);
struct loonggpu_bridge_phy *bridge_phy_alloc(struct loonggpu_dc_bridge *dc_bridge);

int bridge_phy_lt6711_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_lt6711_remove(struct loonggpu_dc_bridge *phy);
int bridge_phy_lt9721_init(struct loonggpu_dc_bridge *res);
int bridge_phy_lt8619_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ncs8805_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ncs8805_remove(struct loonggpu_dc_bridge *phy);
int bridge_phy_lt8718_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ls7a2000_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ls7a1000_init(struct loonggpu_dc_bridge *dc_bridge);
int internal_bridge_ls7a2000_register(struct loonggpu_dc_bridge *dc_bridge);
int internal_bridge_ls7a1000_register(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ls2k2000_init(struct loonggpu_dc_bridge *dc_bridge);
int internal_bridge_ls2k2000_register(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ls2k3000_init(struct loonggpu_dc_bridge *dc_bridge);
int internal_bridge_ls2k3000_register(struct loonggpu_dc_bridge *dc_bridge);

bool ls2k3000_interface_status_changed(struct drm_connector *connector,
					struct loonggpu_dc_crtc *crtc);
int loonggpu_dc_get_modes(struct loonggpu_bridge_phy *phy, int used_method,
					struct drm_connector *connector, struct edid *edid);
int ls2k3000_dc_get_mdoes(struct loonggpu_bridge_phy *phy, int used_method,
					struct drm_connector *connector, struct edid *edid);
int bridge_phy_ls9a1000_init(struct loonggpu_dc_bridge *dc_bridge);
int internal_bridge_ls9a1000_register(struct loonggpu_dc_bridge *dc_bridge);

void show_filtered_modes(struct drm_connector *connector);
void filter_unique_progressive_modes(struct drm_connector *connector);
int drm_connector_edid_intersection(struct drm_connector *actual_connector,
                                  struct drm_connector *virtual_connector);

struct drm_connector *create_virtual_connector(struct drm_device *dev);

void bridge_phy_mode_set(struct loonggpu_bridge_phy *phy,
				struct drm_display_mode *mode,
				struct drm_display_mode *adj_mode);
void loonggpu_bridge_suspend(struct loonggpu_device *adev);
void loonggpu_bridge_resume(struct loonggpu_device *adev);
bool is_connected(struct drm_connector *connector);
int bridge_phy_init(struct loonggpu_bridge_phy *phy);

void bridge_phy_reg_mask_seq(struct loonggpu_bridge_phy *phy,
			     const struct reg_mask_seq *seq, size_t seq_size);
void bridge_phy_reg_update_bits(struct loonggpu_bridge_phy *phy, unsigned int reg,
				unsigned int mask, unsigned int val);
int bridge_phy_reg_dump(struct loonggpu_bridge_phy *phy, size_t start,
			size_t count);
int bridge_phy_lt8618_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_lt8618_remove(struct loonggpu_bridge_phy *phy);
int bridge_phy_it66121_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_it66121_remove(struct loonggpu_bridge_phy *phy);
int bridge_phy_ms7210_init(struct loonggpu_dc_bridge *dc_bridge);
int bridge_phy_ms7210_remove(struct loonggpu_bridge_phy *phy);

bool is_audio_supported_connector(int connector_type);
void is_special_display(struct loonggpu_device *adev, struct loonggpu_connector *aconnector,
								u32 link, struct dc_timing_info *timing);
bool is_special_hdmi_display(struct loonggpu_connector *aconnector, struct dc_timing_info *timing);
void get_edid_manufacturer(const struct edid *edid, char mfg[4]);

static inline struct loonggpu_bridge_phy *lg_bridge_phy_alloc(struct loonggpu_device *adev,
							const struct drm_bridge_funcs *bridge_funcs)
{
#if defined(devm_drm_bridge_alloc)
	struct device *dev;
	if (adev->loongson_dc)
		dev = &adev->loongson_dc->dev;
	else {
		/* type 9a10 */
		dev = &adev->pdev->dev;
	}
	return devm_drm_bridge_alloc(dev, struct loonggpu_bridge_phy, bridge, bridge_funcs);
#else
	struct loonggpu_bridge_phy *bridge_phy;

	return kzalloc(sizeof(*bridge_phy), GFP_KERNEL);
#endif
}


#endif /* __BRIDGE_PHY_H__ */
