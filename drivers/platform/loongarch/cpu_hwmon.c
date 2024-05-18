// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Loongson Technology Corporation Limited
 */
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

#include <asm/loongson.h>

static int nr_packages;
static struct device *cpu_hwmon_dev;

static int loongson3_cpu_temp(int cpu)
{
	u32 reg;

	reg = iocsr_read32(LOONGARCH_IOCSR_CPUTEMP) & 0xff;

	return (int)((s8)reg) * 1000;
}

static ssize_t cpu_temp_label(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int id = (to_sensor_dev_attr(attr))->index - 1;
	return sprintf(buf, "CPU %d Temperature\n", id);
}

static ssize_t get_cpu_temp(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int id = (to_sensor_dev_attr(attr))->index - 1;
	int value = loongson3_cpu_temp(id);
	return sprintf(buf, "%d\n", value);
}

static SENSOR_DEVICE_ATTR(temp1_input, 0444, get_cpu_temp, NULL, 1);
static SENSOR_DEVICE_ATTR(temp1_label, 0444, cpu_temp_label, NULL, 1);
static SENSOR_DEVICE_ATTR(temp2_input, 0444, get_cpu_temp, NULL, 2);
static SENSOR_DEVICE_ATTR(temp2_label, 0444, cpu_temp_label, NULL, 2);
static SENSOR_DEVICE_ATTR(temp3_input, 0444, get_cpu_temp, NULL, 3);
static SENSOR_DEVICE_ATTR(temp3_label, 0444, cpu_temp_label, NULL, 3);
static SENSOR_DEVICE_ATTR(temp4_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp4_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp5_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp5_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp6_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp6_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp7_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp7_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp8_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp8_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp9_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp9_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp10_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp10_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp11_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp11_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp12_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp12_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp13_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp13_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp14_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp14_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp15_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp15_label, 0444, cpu_temp_label, NULL, 4);
static SENSOR_DEVICE_ATTR(temp16_input, 0444, get_cpu_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp16_label, 0444, cpu_temp_label, NULL, 4);

static struct attribute *cpu_hwmon_attributes[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_label.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp2_label.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	&sensor_dev_attr_temp3_label.dev_attr.attr,
	&sensor_dev_attr_temp4_input.dev_attr.attr,
	&sensor_dev_attr_temp4_label.dev_attr.attr,
	&sensor_dev_attr_temp5_input.dev_attr.attr,
	&sensor_dev_attr_temp5_label.dev_attr.attr,
	&sensor_dev_attr_temp6_input.dev_attr.attr,
	&sensor_dev_attr_temp6_label.dev_attr.attr,
	&sensor_dev_attr_temp7_input.dev_attr.attr,
	&sensor_dev_attr_temp7_label.dev_attr.attr,
	&sensor_dev_attr_temp8_input.dev_attr.attr,
	&sensor_dev_attr_temp8_label.dev_attr.attr,
	&sensor_dev_attr_temp9_input.dev_attr.attr,
	&sensor_dev_attr_temp9_label.dev_attr.attr,
	&sensor_dev_attr_temp10_input.dev_attr.attr,
	&sensor_dev_attr_temp10_label.dev_attr.attr,
	&sensor_dev_attr_temp11_input.dev_attr.attr,
	&sensor_dev_attr_temp11_label.dev_attr.attr,
	&sensor_dev_attr_temp12_input.dev_attr.attr,
	&sensor_dev_attr_temp12_label.dev_attr.attr,
	&sensor_dev_attr_temp13_input.dev_attr.attr,
	&sensor_dev_attr_temp13_label.dev_attr.attr,
	&sensor_dev_attr_temp14_input.dev_attr.attr,
	&sensor_dev_attr_temp14_label.dev_attr.attr,
	&sensor_dev_attr_temp15_input.dev_attr.attr,
	&sensor_dev_attr_temp15_label.dev_attr.attr,
	&sensor_dev_attr_temp16_input.dev_attr.attr,
	&sensor_dev_attr_temp16_label.dev_attr.attr,
	NULL
};
static umode_t cpu_hwmon_is_visible(struct kobject *kobj,
				    struct attribute *attr, int i)
{
	int id = i / 2;

	if (id < nr_packages)
		return attr->mode;
	return 0;
}

static struct attribute_group cpu_hwmon_group = {
	.attrs = cpu_hwmon_attributes,
	.is_visible = cpu_hwmon_is_visible,
};

static const struct attribute_group *cpu_hwmon_groups[] = {
	&cpu_hwmon_group,
	NULL
};

static int cpu_initial_threshold = 72000;
static int cpu_thermal_threshold = 96000;
module_param(cpu_thermal_threshold, int, 0644);
MODULE_PARM_DESC(cpu_thermal_threshold, "cpu thermal threshold (96000 (default))");

static struct delayed_work thermal_work;

static void do_thermal_timer(struct work_struct *work)
{
	int i, value, temp_max = 0;

	for (i=0; i<nr_packages; i++) {
		value = loongson3_cpu_temp(i);
		if (value > temp_max)
			temp_max = value;
	}

	if (temp_max <= cpu_thermal_threshold)
		schedule_delayed_work(&thermal_work, msecs_to_jiffies(5000));
	else
		orderly_poweroff(true);
}

static int __init loongson_hwmon_init(void)
{
	int i, value, temp_max = 0;

	pr_info("Loongson Hwmon Enter...\n");

	nr_packages = loongson_sysconf.nr_cpus /
		loongson_sysconf.cores_per_package;

	cpu_hwmon_dev = hwmon_device_register_with_groups(NULL, "cpu_hwmon",
							  NULL, cpu_hwmon_groups);
	if (IS_ERR(cpu_hwmon_dev)) {
		pr_err("Hwmon register fail with %ld!\n", PTR_ERR(cpu_hwmon_dev));
		return PTR_ERR(cpu_hwmon_dev);
	}

	for (i = 0; i < nr_packages; i++) {
		value = loongson3_cpu_temp(i);
		if (value > temp_max)
			temp_max = value;
	}

	pr_info("Initial CPU temperature is %d (highest).\n", temp_max);
	if (temp_max > cpu_initial_threshold)
		cpu_thermal_threshold += temp_max - cpu_initial_threshold;

	INIT_DEFERRABLE_WORK(&thermal_work, do_thermal_timer);
	schedule_delayed_work(&thermal_work, msecs_to_jiffies(20000));

	return 0;
}

static void __exit loongson_hwmon_exit(void)
{
	cancel_delayed_work_sync(&thermal_work);
	hwmon_device_unregister(cpu_hwmon_dev);
}

module_init(loongson_hwmon_init);
module_exit(loongson_hwmon_exit);

MODULE_AUTHOR("Huacai Chen <chenhuacai@loongson.cn>");
MODULE_DESCRIPTION("Loongson CPU Hwmon driver");
MODULE_LICENSE("GPL");
