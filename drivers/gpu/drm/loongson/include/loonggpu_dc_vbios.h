#ifndef __DC_VBIOS_H__
#define __DC_VBIOS_H__

#include <linux/acpi.h>
#include "loonggpu_dc_resource.h"
#include "loonggpu_backlight.h"

struct loonggpu_vbios;
struct vbios_desc;
struct loonggpu_dc;
struct loonggpu_device;

#define VBIOS_VERSION_V1_1 (11)
#define VBIOS_DATA_INVAL 0xFF

enum desc_ver {
	ver_v1,
};

struct vbios_info {
	char title[16];
	u32 version_major;
	u32 version_minor;
	char board[20];
	u32 link_num;
	u32 crtc_offset;
	u32 connector_num;
	u32 connector_offset;
	u32 encoder_num;
	u32 encoder_offset;
} __packed;

struct vbios_desc {
	u16 type;
	u8 ver;
	u8 link;
	u32 offset;
	u32 size;
	u64 ext[2];
} __packed;

struct vbios_header {
	u32 feature;
	u8 oem_vendor[32];
	u8 oem_product[32];
	u32 legacy_offset;
	u32 legacy_size;
	u32 desc_offset;
	u32 desc_size;
	u32 data_offset;
	u32 data_size;
} __packed;

enum loonggpu_edid_method {
	via_null = 0,
	via_i2c,
	via_vbios,
	via_encoder,
	via_max = 0xffff,
} __packed;

struct vbios_gpio {
	u32 feature;
	u32 type;
	u32 level_reg_offset; /* offset of DC */
	u32 level_reg_mask; /* mask of reg */
	u32 dir_reg_offset; /* offset of DC */
	u32 dir_reg_mask; /* mask of reg */
} __packed;

struct vbios_i2c {
	u32 feature;
	u16 id;
	enum vbios_i2c_type type;
} __packed;

struct vbios_pwm {
	u32 feature;
	u8 pwm;
	u8 polarity;
	u32 period;
	struct pwm_light light;  /* feature >= 2 */
} __packed;

struct vbios_encoder {
	u32 feature;
	u32 i2c_id;
	u32 connector_id;
	enum vbios_encoder_type type;
	enum vbios_encoder_config config_type;
	enum vbios_encoder_chip chip;
	u8 chip_addr;
	u32 reset_gpio; /* feature = 1  */
} __packed;

struct vbios_connector {
	u32 feature;
	u32 i2c_id;
	u8 internal_edid[256];
	enum vbios_connector_type type;
	enum vbios_hotplug hotplug;
	enum vbios_edid_method edid_method;
	u32 irq_gpio;
	enum gpio_placement gpio_placement;
	bool multi_interface;  /* feature = 1  */
} __packed;

struct vbios_crtc {
	u32 feature;
	u32 crtc_id;
	u32 encoder_id;
	u32 max_freq;
	u32 max_width;
	u32 max_height;
	bool is_vb_timing;
} __packed;

struct vbios_gpu {
	enum vram_type type;
	u32 bit_width;
	u32 cap;
	u32 count_freq;
	u32 freq;
	u32 shaders_num;
	u32 shaders_freq;
} __packed;

struct vbios_panel
{
	u32 feature;
	u32 count;
	u32 max_vrefresh;
	u32 max_hdisplay;
	u32 max_vdisplay;
	struct timing timing[16];
	u32 max_pixclk; // feature >= 1
} __packed;

struct vbios_ext_encoder {
	u32 data_checksum;
	u32 data_size;
	u8 data[ENCODER_DATA_MAX-8];
} __packed;

enum vbios_backlight_type {
	bl_unuse,
	bl_ec,
	bl_pwm
};

struct vbios_backlight {
	u32 feature;
	u8 used;
	enum vbios_backlight_type type;
} __packed;

struct vbios_lcd_ctrl {
	u32 feature;
	u32 open_sequence;
	u32 close_sequence;
	u32 pre_open_delay;
	u32 pre_close_delay;
	u32 signal_delay[LCD_HW_SIGNAL_MAX];
	u32 gpio_detect_open_pin;
	u32 gpio_detect_open_timer;
	u8  gpio_detect_open_polarity;
	u32 gpio_ctrl_vdd;
	u32 gpio_ctrl_en;
} __packed;

struct vbios_scale {
	u32 feature;
	bool enable;
} __packed;

struct vbios_dpm_config {
	u32 feature;
	bool enable;
	struct loonggpu_dvfs_single_table sclk_table;
} __packed;

struct loonggpu_vbios;

struct vbios_funcs {
	bool (*resource_pool_create)(struct loonggpu_vbios *vbios);
	bool (*resource_pool_destory)(struct loonggpu_vbios *vbios);

	bool (*create_header_resource)(struct loonggpu_vbios *vbios, void *data, u32 size);
	bool (*create_i2c_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_gpio_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_gpu_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_pwm_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_crtc_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_encoder_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_connecor_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_ext_encoder_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_backlight_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_panel_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_lcd_ctrl_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_scale_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);
	bool (*create_dpm_config_resource)(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size);

	struct header_resource *(*get_header_resource)(struct loonggpu_vbios *vbios);
	struct i2c_resource *(*get_i2c_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct pwm_resource *(*get_pwm_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct gpu_resource *(*get_gpu_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct gpio_resource *(*get_gpio_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct crtc_resource *(*get_crtc_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct connector_resource *(*get_connector_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct encoder_resource *(*get_encoder_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct ext_encoder_resources *(*get_ext_encoder_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct backlight_resource *(*get_backlight_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct panel_resource *(*get_panel_resource)(struct loonggpu_vbios *vbios, u32 link);
	struct scale_resource *(*get_scale_resouce)(struct loonggpu_vbios *vbios, u32 link);
	struct dpm_config_resource *(*get_dpm_config_resouce)(struct loonggpu_vbios *vbios, u32 link);
	struct lcd_ctrl_resource *(*get_lcd_ctrl_resource)(struct loonggpu_vbios *vbios, u32 link);
};

struct loonggpu_vbios {
	struct loonggpu_dc *dc;
	void *vbios_ptr;
	struct list_head resource_list;
	struct vbios_funcs *funcs;
};

enum desc_type {
	desc_header = 0,
	desc_crtc,
	desc_encoder,
	desc_connector,
	desc_i2c,
	desc_pwm,
	desc_gpio,
	desc_backlight,
	desc_fan,
	desc_irq_vblank,
	desc_cfg_encoder,
	desc_res_encoder,
	desc_gpu,
	desc_panel,
	desc_lcd_ctrl,
	desc_scale,
	desc_dpm_config,
	desc_max = 0xffff
};

#ifdef CONFIG_ACPI
struct acpi_viat_table {
    struct acpi_table_header header;
    u64 vbios_addr;
} __packed;
#endif

typedef bool(parse_func)(struct vbios_desc *, struct loonggpu_vbios *);

struct desc_func {
	enum desc_type type;
	u16 ver;
	parse_func *func;
};

extern const char * const loonggpu_vram_type_names[];

int loonggpu_gpu_info_sysfs_init(struct loonggpu_device *adev);
void loonggpu_gpu_info_sysfs_exit(struct loonggpu_device *adev);

void *dc_get_vbios_resource(struct loonggpu_vbios *vbios, u32 link,
			    enum resource_type type);
bool dc_vbios_init(struct loonggpu_dc *dc);
void dc_vbios_exit(struct loonggpu_vbios *vbios);
u8 loonggpu_vbios_checksum(const u8 *data, int size);
u32 loonggpu_vbios_version(struct loonggpu_vbios *vbios);
bool check_vbios_info(void);
void dc_vbios_show(struct loonggpu_device *adev, struct loonggpu_vbios *vbios);

#endif
