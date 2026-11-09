// SPDX-License-Identifier: GPL-2.0+
#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/pwm.h>
#include "loonggpu.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_backlight.h"
#include "loonggpu_dc_encoder.h"
#include "bridge_phy.h"
#include "loonggpu_helper.h"

static bool loonggpu_backlight_get_hw_status(struct loonggpu_backlight *ls_bl)
{
	return (gpio_get_value(ls_bl->pin_vdd) && gpio_get_value(ls_bl->pin_en)
		&& pwm_is_enabled(ls_bl->pwm));
}

static int loonggpu_lcd_ctrl_open_detect(struct loonggpu_backlight *ls_bl)
{
	gpio_direction_input(ls_bl->lcd_ctrl->gpio_detect_open_pin);
	return gpio_get_value(ls_bl->lcd_ctrl->gpio_detect_open_pin);
}

static int loonggpu_lcd_ctrl_vdd(struct loonggpu_backlight *ls_bl, unsigned int delay_ms, bool open)
{
	gpio_direction_output(ls_bl->pin_vdd, 1);
	gpio_set_value(ls_bl->pin_vdd, open);
	mdelay(delay_ms);

	return 0;
}

static int loonggpu_lcd_ctrl_en(struct loonggpu_backlight *ls_bl, unsigned int delay_ms, bool open)
{
	gpio_direction_output(ls_bl->pin_en, 1);
	gpio_set_value(ls_bl->pin_en, open);
	mdelay(delay_ms);

	return 0;
}

static int loonggpu_lcd_ctrl_pwm(struct loonggpu_backlight *ls_bl, unsigned int delay_ms, bool open)
{
	if (open)
		pwm_enable(ls_bl->pwm);
	else
		pwm_disable(ls_bl->pwm);

	mdelay(delay_ms);

	return 0;
}

static int loonggpu_lcd_ctrl_reset(struct loonggpu_backlight *ls_bl, unsigned int delay_ms, bool open)
{
	int ret;
	struct loonggpu_device *adev = ls_bl->driver_private;
	struct loonggpu_dc_encoder *dc_encoder;

	dc_encoder = adev->dc->link_info[ls_bl->display_pipe_index].encoder;
	if (dc_encoder) {
		ret = loonggpu_dc_encoder_hw_reset(dc_encoder, open, 0);
		mdelay(delay_ms);
		return ret;
	}
	return -EPERM;
}

void loonggpu_lcd_ctrl_hw_action(struct loonggpu_backlight *ls_bl, bool open)
{
	struct loonggpu_device *adev;
	struct loonggpu_bridge_phy *bridge_phy;
	struct lcd_hw_signal_node *signal_node;
	struct list_head  *list;

	if (!ls_bl)
		return;

	adev = ls_bl->driver_private;
	bridge_phy = adev->mode_info.encoders[ls_bl->display_pipe_index]->bridge;

	/* If there is no lcd_ctrl (struct loonggpu_lcd_ctrl *), use the default signal
	 * control timing to turn the screen on and off. */
	if (!ls_bl->lcd_ctrl) {
		if (open) {
			if (bridge_phy->res->encoder_obj == ENCODER_CHIP_ID_EDP_NCS8805 )
				loonggpu_lcd_ctrl_vdd(ls_bl, 160, open);
			loonggpu_lcd_ctrl_pwm(ls_bl, 10, open);
			loonggpu_lcd_ctrl_en(ls_bl, 0, open);
		} else {
			loonggpu_lcd_ctrl_en(ls_bl, 10, open);
			loonggpu_lcd_ctrl_pwm(ls_bl, 160, open);
			if (bridge_phy->res->encoder_obj == ENCODER_CHIP_ID_EDP_NCS8805 )
				loonggpu_lcd_ctrl_vdd(ls_bl, 0, open);
		}
	} else {
		if (open) {
			list = &ls_bl->lcd_ctrl->open_signal_list;
			mdelay(ls_bl->lcd_ctrl->pre_open_delay);
		} else {
			list = &ls_bl->lcd_ctrl->close_signal_list;
			mdelay(ls_bl->lcd_ctrl->pre_close_delay);
		}

		list_for_each_entry(signal_node, list, node) {
			signal_node->signal_func(ls_bl, signal_node->delay_ms, open);
		}
	}

	ls_bl->hw_enabled = open;
}

static void timer_power_ctrl_callback(struct timer_list *timer)
{
	struct bl_timer *open_timer;

	open_timer = container_of(timer, struct bl_timer, timer);

	if (loonggpu_lcd_ctrl_open_detect(open_timer->ls_bl) ==
			open_timer->ls_bl->lcd_ctrl->gpio_detect_open_polarity) {
		loonggpu_lcd_ctrl_hw_action(open_timer->ls_bl, true);
		lg_del_timer(&open_timer->timer);
		open_timer->running = false;
		return;
	}
	mod_timer(&open_timer->timer, jiffies +
			msecs_to_jiffies(open_timer->ls_bl->lcd_ctrl->gpio_detect_open_timer));
}

static void loonggpu_lcd_ctrl_hw_action_timer(struct loonggpu_backlight *ls_bl,  bool open)
{
	if (open) {
		if (ls_bl->lcd_ctrl->open_timer.running)
			return;
		timer_setup(&ls_bl->lcd_ctrl->open_timer.timer, timer_power_ctrl_callback, 0);
		mod_timer(&ls_bl->lcd_ctrl->open_timer.timer, jiffies +
				msecs_to_jiffies(ls_bl->lcd_ctrl->gpio_detect_open_timer));
		ls_bl->lcd_ctrl->open_timer.running = true;
	} else {
		if (ls_bl->lcd_ctrl->open_timer.running)
			lg_del_timer(&ls_bl->lcd_ctrl->open_timer.timer);
		else
			loonggpu_lcd_ctrl_hw_action(ls_bl, false);
		ls_bl->lcd_ctrl->open_timer.running = false;
	}
}

static void loonggpu_backlight_power(struct loonggpu_backlight *ls_bl, bool enable)
{
 	struct loonggpu_device *adev;
	struct loonggpu_bridge_phy *bridge_phy;

	if (IS_ERR(ls_bl))
		return;

 	if (IS_ERR(ls_bl->pwm))
 		return;

	DRM_DEBUG("Request backlight power: %s->%s.\n",
		  ls_bl->hw_enabled ? "open" : "close",
		  enable ? "open" : "close");

	if (enable == ls_bl->hw_enabled)
		return;

	adev = ls_bl->driver_private;
	bridge_phy = adev->mode_info.encoders[ls_bl->display_pipe_index]->bridge;

	if (bridge_phy && bridge_phy->cfg_funcs && bridge_phy->cfg_funcs->backlight_ctrl) {
		bridge_phy->cfg_funcs->backlight_ctrl(bridge_phy,
				enable ? DRM_MODE_DPMS_ON :DRM_MODE_DPMS_OFF);
		ls_bl->hw_enabled = enable;
	} else {
		/* Some display adapter chips have timing requirements for lighting up the screen.
		 * Such chips often have a gpio to indicate whether the state of turning on
		 * the screen is ready. */
		if (ls_bl->lcd_ctrl && ls_bl->lcd_ctrl->gpio_detect_open_pin) {
			loonggpu_lcd_ctrl_hw_action_timer(ls_bl, enable);
		} else {
			loonggpu_lcd_ctrl_hw_action(ls_bl, enable);
		}
	}

	return;
}

static int loonggpu_backlight_update(struct backlight_device *bd)
{
	struct loonggpu_backlight *ls_bl = bl_get_data(bd);

	DRM_DEBUG("Request bl update: %s->%s, level:%d->%d.\n",
		  ls_bl->hw_enabled ? "open" : "close",
		  bd->props.power == FB_BLANK_UNBLANK ? "open" : "close",
		  ls_bl->level, bd->props.brightness);

	if (ls_bl->level != bd->props.brightness) {
		ls_bl->level = bd->props.brightness;
		ls_bl->set_brightness(ls_bl, ls_bl->level);
	}

	return 0;
}

static int loonggpu_backlight_get_brightness(struct backlight_device *bd)
{
	struct loonggpu_backlight *ls_bl = bl_get_data(bd);

	if (ls_bl->get_brightness)
		return ls_bl->get_brightness(ls_bl);

	return -ENOEXEC;
}

static const struct backlight_ops loonggpu_backlight_ops = {
	.update_status  = loonggpu_backlight_update,
	.get_brightness = loonggpu_backlight_get_brightness,
};

static unsigned int loonggpu_backlight_get(struct loonggpu_backlight *ls_bl)
{
	u16 duty_ns, period_ns;
	u32 level;

	if (IS_ERR(ls_bl->pwm))
		return 0;

	period_ns = ls_bl->pwm_period;
	duty_ns = pwm_get_duty_cycle(ls_bl->pwm);

	level = DIV_ROUND_UP((duty_ns * ls_bl->max), period_ns);
	level = clamp(level, ls_bl->min, ls_bl->max);

	return level;
}

static void loonggpu_backlight_set(struct loonggpu_backlight *ls_bl,
		unsigned int level_raw)
{
	unsigned int period_ns;
	unsigned int duty_ns;
	unsigned int level_max, level_min, level_range;
	unsigned int level_index, level_next;
	unsigned int duty_index, max_index;
	unsigned int index;
	struct pwm_light *light;

	if (IS_ERR(ls_bl->pwm))
		return;

	level_raw = clamp(level_raw, ls_bl->min, ls_bl->max);

	period_ns = ls_bl->pwm_period;
	duty_ns = DIV_ROUND_UP((level_raw * period_ns), ls_bl->max);

	if (ls_bl->light.used) {
		light = &ls_bl->light;
		max_index = light->level_count - 1;

		level_range = light->tables[max_index].level - light->tables[0].level;
		level_min = clamp(DIV_ROUND_CLOSEST(light->tables[0].level
					* (ls_bl->max - ls_bl->min), level_range), ls_bl->min, ls_bl->max);
		level_max = clamp(DIV_ROUND_CLOSEST(light->tables[max_index].level
					* (ls_bl->max - ls_bl->min), level_range), ls_bl->min, ls_bl->max);

		for (index = 0; index <= max_index; index++) {
			duty_index = light->tables[index].duty;
			level_index = clamp(DIV_ROUND_CLOSEST(light->tables[index].level
						* (ls_bl->max - ls_bl->min), level_range), ls_bl->min, ls_bl->max);
			if (level_raw <= level_min) {
				duty_ns = DIV_ROUND_UP((light->tables[0].duty * period_ns), 100);
				break;
			} else if (level_raw >= level_max) {
				duty_ns = DIV_ROUND_UP((light->tables[max_index].duty * period_ns), 100);
				break;
			} else if (level_raw == level_index) {
				duty_ns = DIV_ROUND_UP((duty_index * period_ns), 100);
				break;
			} else {
				level_next = clamp(DIV_ROUND_CLOSEST(light->tables[index + 1].level
							* (ls_bl->max - ls_bl->min), level_range), ls_bl->min, ls_bl->max);
				if ((level_raw > level_index) && (level_raw < level_next)) {
					duty_index = DIV_ROUND_CLOSEST(light->tables[index + 1].duty - duty_index,
							level_next - level_index) * (level_raw - level_index) + duty_index;
					duty_ns = DIV_ROUND_UP((duty_index * period_ns), 100);
					break;
				}
			}
		}
	}

	DRM_DEBUG("Set backlight: level_raw=%d, 0x%x/0x%x ns.\n",
		  level_raw, duty_ns, period_ns);

	pwm_config(ls_bl->pwm, duty_ns, period_ns);
}

static int loonggpu_backlight_hw_request_init(struct loonggpu_backlight *ls_bl)
{
	int ret = 0;
	bool pwm_enable_default;
	struct pwm_state state;
	struct loonggpu_device *adev = ls_bl->driver_private;
	char pwm_name[10];

	sprintf(pwm_name, "pwm%d", ls_bl->pwm_id);
	ls_bl->pwm = lg_pwm_request(adev->ddev->dev, pwm_name, ls_bl->pwm_id, "Loongson_bl");

	if (IS_ERR(ls_bl->pwm)) {
		DRM_ERROR("Failed to get the pwm chip\n");
		ls_bl->pwm = NULL;
		goto ERROR_PWM;
	}

	pwm_enable_default = pwm_is_enabled(ls_bl->pwm);
	/* pwm init.*/
	pwm_disable(ls_bl->pwm);

	pwm_get_state(ls_bl->pwm, &state);
	if (state.polarity != ls_bl->pwm_polarity && !state.enabled) {
		state.polarity = ls_bl->pwm_polarity;
		lg_pwm_apply_state(ls_bl->pwm, &state);
	}

	loonggpu_backlight_set(ls_bl, ls_bl->level);
	if (pwm_enable_default)
		pwm_enable(ls_bl->pwm);

	ret = gpio_request(GPIO_LCD_VDD, "GPIO_VDD");
	if (ret) {
		DRM_ERROR("EN request error!\n");
		goto ERROR_VDD;
	}

	ret = gpio_request(GPIO_LCD_EN, "GPIO_EN");
	if (ret) {
		DRM_ERROR("VDD request error!\n");
		goto ERROR_EN;
	}

	/* gpio init */
	gpio_direction_output(GPIO_LCD_VDD, 1);
	gpio_direction_output(GPIO_LCD_EN, 1);

	return ret;

ERROR_EN:
	gpio_free(GPIO_LCD_VDD);
ERROR_VDD:
	lg_pwm_free(ls_bl->pwm);
ERROR_PWM:
	lg_pwm_remove_table();
	return -ENODEV;
}

static void loonggpu_lcd_ctrl_init_sequence_list(struct loonggpu_device *adev,
		struct loonggpu_lcd_ctrl *lcd_ctrl, struct lcd_ctrl_resource *lcd_ctrl_res, unsigned int action)
{
	struct lcd_hw_signal_node *signal_node;
	struct list_head *list;
	u32 sequence, i;

	if (action == LCD_CTRL_ACTION_CLOSE) {
		sequence = lcd_ctrl_res->close_sequence;
		list = &lcd_ctrl->close_signal_list;
		lcd_ctrl->pre_close_delay = lcd_ctrl_res->pre_close_delay;
	} else if (action == LCD_CTRL_ACTION_OPEN) {
		sequence = lcd_ctrl_res->open_sequence;
		list = &lcd_ctrl->open_signal_list;
		lcd_ctrl->pre_open_delay = lcd_ctrl_res->pre_open_delay;
	}

	for (i = 0; i < LCD_HW_SIGNAL_MAX; i++) {
		signal_node = kzalloc(sizeof(struct lcd_hw_signal_node), GFP_KERNEL);
		if (!signal_node)
			continue;

		signal_node->signal = LCD_CTRL_SIGNAL_GET(sequence, i);
		signal_node->delay_ms =
			LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[signal_node->signal], action);

		switch (signal_node->signal) {
		case LCD_HW_SIGNAL_VDD:
			signal_node->signal_func = loonggpu_lcd_ctrl_vdd;
			break;
		case LCD_HW_SIGNAL_EN:
			signal_node->signal_func = loonggpu_lcd_ctrl_en;
			break;
		case LCD_HW_SIGNAL_PWM:
			signal_node->signal_func = loonggpu_lcd_ctrl_pwm;
			break;
		case LCD_HW_SIGNAL_RESET:
			signal_node->signal_func = loonggpu_lcd_ctrl_reset;
			break;
		case LCD_HW_SIGNAL_NULL:
		default:
			kfree(signal_node);
			continue; /* Invalid node is not required list_add_tail() */
		}
		signal_node->adev = adev;
		list_add_tail(&signal_node->node, list);
	}
}

static struct loonggpu_lcd_ctrl * loonggpu_lcd_ctrl_init(struct loonggpu_device *adev,
		struct loonggpu_backlight *ls_bl, int index)
{
	struct loonggpu_lcd_ctrl *lcd_ctrl;
	struct lcd_ctrl_resource *lcd_ctrl_res;

	lcd_ctrl_res = dc_get_vbios_resource(adev->dc->vbios, index,
			LOONGGPU_RESOURCE_LCD_CTRL);

	if (lcd_ctrl_res) {
		lcd_ctrl = kzalloc(sizeof(*lcd_ctrl), GFP_KERNEL);
		if (!lcd_ctrl) {
			DRM_ERROR("loonggpu backlight alloc loonggpu_lcd failed.\n");
			return NULL;
		}
		DRM_INFO("loonggpu backlight-%d will use lcd_ctrl signal order info.\n", index);

		/* struct vbios_lcd  --> struct loonggpu_lcd */
		INIT_LIST_HEAD(&lcd_ctrl->open_signal_list);
		INIT_LIST_HEAD(&lcd_ctrl->close_signal_list);
		loonggpu_lcd_ctrl_init_sequence_list(adev, lcd_ctrl, lcd_ctrl_res, LCD_CTRL_ACTION_CLOSE);
		loonggpu_lcd_ctrl_init_sequence_list(adev, lcd_ctrl, lcd_ctrl_res, LCD_CTRL_ACTION_OPEN);

		lcd_ctrl->gpio_detect_open_pin = lcd_ctrl_res->gpio_detect_open_pin;
		lcd_ctrl->gpio_detect_open_timer = lcd_ctrl_res->gpio_detect_open_timer;
		lcd_ctrl->gpio_detect_open_polarity = lcd_ctrl_res->gpio_detect_open_polarity;
		lcd_ctrl->gpio_ctrl_vdd = lcd_ctrl_res->gpio_ctrl_vdd;
		lcd_ctrl->gpio_ctrl_en = lcd_ctrl_res->gpio_ctrl_en;

		lcd_ctrl->open_timer.ls_bl = ls_bl;
		lcd_ctrl->open_timer.running = false;

		if (!lcd_ctrl->gpio_ctrl_vdd)
			lcd_ctrl->gpio_ctrl_vdd = GPIO_LCD_VDD;
		if (!lcd_ctrl->gpio_ctrl_en)
			lcd_ctrl->gpio_ctrl_vdd = GPIO_LCD_EN;
		if (lcd_ctrl->gpio_detect_open_pin) {
			if (gpio_request(lcd_ctrl->gpio_detect_open_pin, "GPIO_BL_DETECT")) {
				DRM_ERROR("loonggpu backlight BL_DETECT request error!\n");
			}
			if (!lcd_ctrl->gpio_detect_open_timer)
				lcd_ctrl->gpio_detect_open_timer = 200;
		}

		return lcd_ctrl;
	} else {
		DRM_INFO("loonggpu backlight-%d not will use lcd_ctrl signal order info.\n", index);
		return NULL;
	}
}

static struct loonggpu_backlight
*loonggpu_backlight_init(struct loonggpu_device *adev, int index)
{
	struct loonggpu_backlight *ls_bl = NULL;
	struct backlight_properties props;
	struct pwm_resource *pwm_res;
	int ret = 0;

	ls_bl = kzalloc(sizeof(struct loonggpu_backlight), GFP_KERNEL);
	if (IS_ERR(ls_bl)) {
		DRM_ERROR("Failed to alloc backlight.\n");
		return NULL;
	}

	ls_bl->min = BL_MIN_LEVEL;
	ls_bl->max = BL_MAX_LEVEL;
	ls_bl->level = BL_DEF_LEVEL;
	ls_bl->driver_private = adev;
	ls_bl->display_pipe_index = index;
	ls_bl->get_brightness = loonggpu_backlight_get;
	ls_bl->set_brightness = loonggpu_backlight_set;
	ls_bl->power = loonggpu_backlight_power;

	pwm_res = dc_get_vbios_resource(adev->dc->vbios,
					index, LOONGGPU_RESOURCE_PWM);
	if (pwm_res == NULL) {
		goto ERROR_PWM;
	}
	ls_bl->pwm_id = pwm_res->pwm;
	/* 0:low start, 1:high start */
	ls_bl->pwm_polarity = pwm_res->polarity;
	ls_bl->pwm_period = pwm_res->period;
	ls_bl->light = pwm_res->light;

	DRM_INFO("pwm: id=%d, period=%dns, polarity=%d, light_tables=%d.\n",
		 ls_bl->pwm_id, ls_bl->pwm_period, ls_bl->pwm_polarity,
		 ls_bl->light.used ? ls_bl->light.level_count: 0);

	/* init lcd_ctrl */
	ls_bl->lcd_ctrl = loonggpu_lcd_ctrl_init(adev, ls_bl, index);
	ls_bl->pin_vdd =  ls_bl->lcd_ctrl ? ls_bl->lcd_ctrl->gpio_ctrl_vdd: GPIO_LCD_VDD;
	ls_bl->pin_en  =  ls_bl->lcd_ctrl ? ls_bl->lcd_ctrl->gpio_ctrl_en: GPIO_LCD_EN;

	ret = loonggpu_backlight_hw_request_init(ls_bl);
	if (ret)
		goto ERROR_HW;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.power = FB_BLANK_UNBLANK;
	props.max_brightness = ls_bl->max;
	props.brightness = ls_bl->level;

	ls_bl->device = backlight_device_register("loonggpu-bl",
			adev->ddev->dev,
			ls_bl, &loonggpu_backlight_ops, &props);
	if (IS_ERR(ls_bl->device)) {
		DRM_ERROR("Failed to register backlight.\n");
		goto ERROR_REG;
	}

	DRM_INFO("register loonggpu backlight_%d completed.\n", index);

	adev->mode_info.backlights[index] = ls_bl;

	return ls_bl;

ERROR_PWM:
ERROR_HW:
ERROR_REG:
	kfree(ls_bl);

	return NULL;
}

int loonggpu_backlight_register(struct drm_connector *connector)
{
	struct loonggpu_backlight *ls_bl;
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct backlight_resource *bl_res;
	int ret = 0;
	bool ls_bl_status = false;

	bl_res = dc_get_vbios_resource(adev->dc->vbios, connector->index,
			LOONGGPU_RESOURCE_BACKLIGHT);
	if (!bl_res)
		return ret;

	ls_bl = loonggpu_backlight_init(adev, connector->index);
	if (!ls_bl)
		return -ENXIO;

	ls_bl_status = loonggpu_backlight_get_hw_status(ls_bl);
	if (ls_bl_status) {
		ls_bl->hw_enabled = true;
		ls_bl->power(ls_bl, true);
	} else {
		ls_bl->hw_enabled = false;
		ls_bl->power(ls_bl, false);
	}

	DRM_INFO("backlight power status: %s->%s.\n",
		 ls_bl_status ? "on" : "off",
		 ls_bl->hw_enabled ? "on" : "off");

	return ret;
}
