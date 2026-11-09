#ifndef __DC_RESOURCE_H__
#define __DC_RESOURCE_H__
#include "loonggpu_backlight.h"

#define ENCODER_DATA_MAX  (1024*8)

enum resource_type {
	LOONGGPU_RESOURCE_DEFAULT,
	LOONGGPU_RESOURCE_HEADER,
	LOONGGPU_RESOURCE_EXT_ENCODER,
	LOONGGPU_RESOURCE_GPU,
	LOONGGPU_RESOURCE_GPIO,
	LOONGGPU_RESOURCE_I2C,
	LOONGGPU_RESOURCE_PWM,
	LOONGGPU_RESOURCE_BACKLIGHT,
	LOONGGPU_RESOURCE_CRTC,
	LOONGGPU_RESOURCE_ENCODER,
	LOONGGPU_RESOURCE_CONNECTOR,
	LOONGGPU_RESOURCE_PANEL,
	LOONGGPU_RESOURCE_LCD_CTRL,
	LOONGGPU_RESOURCE_SCALE,
	LOONGGPU_RESOURCE_DPM_CONFIG,
	LOONGGPU_RESOURCE_MAX,
};

struct resource_object {
	u32 link;
	enum resource_type type;
	struct list_head node;
};

struct header_resource {
	struct resource_object base;
	u32 links;
	u32 max_planes;
	u8 ver_majro;
	u8 ver_minor;
	u8 name[16];
	char board[20];
	u8 oem_vendor[32];
	u8 oem_product[32];
};

struct gpio_resource {
	struct resource_object base;
	u32 type; 		/* bit Reuse */
	u32 level_reg_offset; 	/* offset of DC */
	u32 level_reg_mask; 	/* mask of reg */
	u32 dir_reg_offset; 	/* offset of DC */
	u32 dir_reg_mask; 	/* mask of reg */
};

enum vbios_i2c_type {
	I2C_CPU,
	I2C_GPIO,
};

struct i2c_resource {
	struct resource_object base;
	u32 feature;
	u16 id;
	enum vbios_i2c_type type;
};

struct pwm_resource {
	struct resource_object base;
	u32 feature;
	u8 pwm;
	u8 polarity;
	u32 period;
	struct pwm_light light;  /* feature >= 2 */
};

struct crtc_resource {
	struct resource_object base;
	u32 feature;
	u32 crtc_id;
	u32 encoder_id;
	u32 max_freq;
	u32 max_width;
	u32 max_height;
	bool is_vb_timing;
};

struct encoder_resource {
	struct resource_object base;
	u32 feature;
	u32 i2c_id;
	u32 connector_id;
	u32 type;
	u32 config_type;
	u32 chip;
	u8 chip_addr;
	u32 reset_gpio;
};

struct connector_resource {
	struct resource_object base;
	u32 feature;
	u32 i2c_id;
	u8 internal_edid[256];
	u32 type;
	u32 hotplug;
	u32 edid_method;
	u32 irq_gpio;
	u32 gpio_placement;
	bool multi_interface;  /* feature = 1  */
};

struct gpu_resource {
	struct resource_object base;
	u32 vram_type;
	u32 bit_width;
	u32 cap;
	u32 count_freq;
	u32 freq;
	u32 shaders_num;
	u32 shaders_freq;
};

struct ext_encoder_resources {
	struct resource_object base;
	u32 data_checksum;
	u32 data_size;
	u8 data[ENCODER_DATA_MAX-8];
};

struct backlight_resource {
	struct resource_object base;
	u32 feature;
	u8 used;
	u32 type;
};

struct timing {
	u32 vrefresh;
	u32 hdisplay;
	u32 vdisplay;
};

struct panel_resource {
	struct resource_object base;
	u32 feature;
	u32 count;
	u32 max_vrefresh;
	u32 max_hdisplay;
	u32 max_vdisplay;
	struct timing timing[16];
	u32 max_pixclk; //khz
};

struct lcd_ctrl_resource {
	struct resource_object base;
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
};

struct scale_resource {
	struct resource_object base;
	u32 feature;
	bool enable;
};

struct dpm_config_resource {
	struct resource_object base;
	u32 feature;
	bool enable;
	struct loonggpu_dvfs_single_table sclk_table;
};

enum gpio_placement {
	GPIO_PLACEMENT_LS3A = 0,
	GPIO_PLACEMENT_LS7A,
};

enum vbios_edid_method {
	EDID_VIA_NULL = 0,
	EDID_VIA_I2C,
	EDID_VIA_VBIOS,
	EDID_VIA_ENCODER,
	EDID_VIA_MAX = EDID_VIA_I2C,
};

enum vbios_encoder_type {
	ENCODER_NONE,
	ENCODER_DAC,
	ENCODER_TMDS,
	ENCODER_LVDS,
	ENCODER_TVDAC,
	ENCODER_VIRTUAL,
	ENCODER_DSI,
	ENCODER_DPMST,
	ENCODER_DPI
};

enum vbios_encoder_config {
	ENCODER_TRANSPARENT = 0,
	ENCODER_OS_CONFIG,
	ENCODER_BIOS_CONFIG,
	ENCODER_TYPE_MAX = ENCODER_TRANSPARENT,
};

enum vbios_encoder_chip {
	/* MISC: 0x00~-0x0f */
	ENCODER_CHIP_ID_NONE = 0X00,
	ENCODER_CHIP_ID_INTERNAL_DVO = 0X01,
	ENCODER_CHIP_ID_INTERNAL_HDMI = 0X02,
	ENCODER_CHIP_ID_INTERNAL_EDP = 0X03,
	ENCODER_CHIP_ID_INTERNAL_DP = 0X04,
	ENCODER_CHIP_ID_INTERNAL_COMPOSITE = 0x05,

	/* VGA: 0x10~-0x1f */
	ENCODER_CHIP_ID_VGA_CH7055 = 0X10,
	ENCODER_CHIP_ID_VGA_ADV7125 = 0X11,
	ENCODER_CHIP_ID_VGA_TRANSPARENT = 0x1F,

	/* DVI: 0x20~-0x2f */
	ENCODER_CHIP_ID_DVI_TFP410 = 0X20,
	ENCODER_CHIP_ID_DVI_TRANSPARENT = 0x2F,

	/* HDMI: 0x30~-0x3f */
	ENCODER_CHIP_ID_HDMI_IT66121 = 0X30,
	ENCODER_CHIP_ID_HDMI_SIL9022 = 0X31,
	ENCODER_CHIP_ID_HDMI_LT8618 = 0X32,
	ENCODER_CHIP_ID_HDMI_MS7210 = 0x33,
	ENCODER_CHIP_ID_HDMI_TRANSPARENT = 0x3F,

	/* EDP: 0x40~-0x4f */
	ENCODER_CHIP_ID_EDP_NCS8805 = 0X40,
	ENCODER_CHIP_ID_EDP_NCS8803 = 0x41,
	ENCODER_CHIP_ID_EDP_LT9721 = 0x42,
	ENCODER_CHIP_ID_EDP_LT6711 = 0x43,
	ENCODER_CHIP_ID_EDP_ICNM7601 = 0x44,
	ENCODER_CHIP_ID_EDP_CS5611AQ_S = 0x45,
	ENCODER_CHIP_ID_EDP_TRANSPARENT = 0x4F,

	/* HDMI to LVDS */
	ENCODER_CHIP_ID_LVDS_LT8619 = 0x50,

	/* DVO to DP */
	ENCODER_CHIP_ID_DP_LT8718 = 0x60,
	ENCODER_CHIP_ID_MAX = 0xFF
};

enum vbios_hotplug {
	FORCE_ON = 0,
	POLLING,
	IRQ,
	VBIOS_HOTPLUG_MAX = FORCE_ON
};

enum vram_type {
	DDR3,
	DDR4,
	DDR5,
	LPDDR4,
	VRAM_UNKNOWN
};

enum vbios_connector_type {
	CONNECTOR_UNKNOWN = 0,
	CONNECTOR_VGA,
	CONNECTOR_DVI_I,
	CONNECTOR_DVI_D,
	CONNECTOR_DVI_A,
	CONNECTOR_COMPOSITE,
	CONNECTOR_SVIDEO,
	CONNECTOR_LVDS,
	CONNECTOR_COMPONENT,
	CONNECTOR_9PINDIN,
	CONNECTOR_DISPLAYPORT,
	CONNECTOR_HDMI_A,
	CONNECTOR_HDMI_B,
	CONNECTOR_TV,
	CONNECTOR_EDP,
	CONNECTOR_VIRTUAL,
	CONNECTOR_DSI,
	CONNECTOR_DPI
};

#endif
