// SPDX-License-Identifier: GPL-2.0+
#ifndef __LOONGGPU_BACKLIGHT_H__
#define __LOONGGPU_BACKLIGHT_H__

#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <linux/backlight.h>
#include <linux/timer.h>

#define BL_MAX_LEVEL 100
#define BL_MIN_LEVEL 0
#define BL_DEF_LEVEL 60
#define GPIO_LCD_EN 62
#define GPIO_LCD_VDD 63

#define LCD_CTRL_ACTION_CLOSE                   0
#define LCD_CTRL_ACTION_OPEN                    1
#define LCD_CTRL_SIGNAL_MASK                    0xf
#define LCD_CTRL_SIGNAL_GET(seq, order)         ((seq >> (order*4)) & LCD_CTRL_SIGNAL_MASK)
#define LCD_CTRL_DELAY_MASK                     0xffff
#define LCD_CTRL_DELAY_GET(delay, action)       ((delay >> (action*16)) & LCD_CTRL_DELAY_MASK)

#define PWM_LIGHT_TABLES_MAX  50

enum vbios_lcd_hw_signal {
	LCD_HW_SIGNAL_NULL,
	LCD_HW_SIGNAL_VDD,
	LCD_HW_SIGNAL_EN,
	LCD_HW_SIGNAL_PWM,
	LCD_HW_SIGNAL_RESET,
	LCD_HW_SIGNAL_MAX,
};

#define lcd_sig_to_str(cmd) \
	(cmd == LCD_HW_SIGNAL_NULL ? "null":\
	(cmd == LCD_HW_SIGNAL_VDD ? "vdd":\
	(cmd == LCD_HW_SIGNAL_EN ? "en":\
	(cmd == LCD_HW_SIGNAL_PWM ? "pwm" :\
	(cmd == LCD_HW_SIGNAL_RESET ? "reset":\
	"Unknown")))))

struct lcd_hw_signal_node {
	enum vbios_lcd_hw_signal  signal;
	int delay_ms;
	struct list_head node;
	struct loonggpu_device *adev;
	int (*signal_func)(struct loonggpu_backlight *ls_bl, unsigned int delay_ms, bool open);
};

struct bl_timer {
	struct timer_list timer;
	struct loonggpu_backlight *ls_bl;
	volatile bool running;
};

struct loonggpu_lcd_ctrl {
	struct bl_timer open_timer;
	u32 pre_open_delay;
	u32 pre_close_delay;
	struct list_head open_signal_list;
	struct list_head close_signal_list;
	u32 gpio_detect_open_pin;
	u32 gpio_detect_open_timer;
	u8 gpio_detect_open_polarity;
	u32 gpio_ctrl_vdd;
	u32 gpio_ctrl_en;
};

struct pwm_light {
	u8 used;
	u8 level_count;
	struct {
		u8 level;
		u8 duty;
	} tables[PWM_LIGHT_TABLES_MAX];
};

struct loonggpu_backlight {
	void *driver_private;
	struct backlight_device *device;
	struct pwm_device *pwm;
	struct loonggpu_lcd_ctrl *lcd_ctrl;
	int display_pipe_index;
	u32 pwm_id;
	u32 pwm_polarity;
	u32 pwm_period;
	struct pwm_light light;
	bool hw_enabled;
	u32 level;
	u32 max;
	u32 min;
	u32 pin_vdd;
	u32 pin_en;

	unsigned int (*get_brightness)(struct loonggpu_backlight *ls_bl);
	void (*set_brightness)(struct loonggpu_backlight *ls_bl,
			       unsigned int level);
	void (*power)(struct loonggpu_backlight *ls_bl, bool enable);
};

void loonggpu_lcd_ctrl_hw_action(struct loonggpu_backlight *ls_bl, bool open);
int loonggpu_backlight_register(struct drm_connector *connector);
#endif /* __LOONGGPU_BACKLIGHT_H__ */
