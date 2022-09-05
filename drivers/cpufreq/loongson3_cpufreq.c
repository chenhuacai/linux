/*
 * CPUFreq driver for the loongson-3 processors
 *
 * All revisions of Loongson-3 processor support this feature.
 *
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/platform_device.h>

#include <asm/idle.h>
#include <asm/loongarch.h>
#include <asm/loongson.h>

/* Message */
union smc_message
{
	u32 value;
	struct
	{
		u32 id          : 4;
		u32 info        : 4;
		u32 val         : 16;
		u32 cmd         : 6;
		u32 extra       : 1;
		u32 complete  	: 1;
	};
};

/* Command return values */
#define CMD_OK                      0  /* No error */
#define CMD_ERROR                   1  /* Regular error */
#define CMD_NOCMD                   2  /* Command does not support */
#define CMD_INVAL                   3  /* Invalid Parameter */

/* Version commands */
/*
 * CMD_GET_VERSION - Get interface version
 * Input: none
 * Output: version
 */
#define CMD_GET_VERSION             0x1

/* Feature commands */
/*
 * CMD_GET_FEATURE - Get feature state
 * Input: feature ID
 * Output: feature flag
 */
#define CMD_GET_FEATURE             0x2

/*
 * CMD_SET_FEATURE - Set feature state
 * Input: feature ID, feature flag
 * output: none
 */
#define CMD_SET_FEATURE             0x3

/* Feature IDs */
#define FEATURE_SENSOR              0
#define FEATURE_FAN                 1
#define FEATURE_DVFS                2

/* Sensor feature flags */
#define FEATURE_SENSOR_ENABLE       (1 << 0)
#define FEATURE_SENSOR_SAMPLE       (1 << 1)

/* Fan feature flags */
#define FEATURE_FAN_ENABLE          (1 << 0)
#define FEATURE_FAN_AUTO            (1 << 1)

/* DVFS feature flags */
#define FEATURE_DVFS_ENABLE         (1 << 0)
#define FEATURE_DVFS_BOOST          (1 << 1)

/* Sensor commands */
/*
 * CMD_GET_SENSOR_NUM - Get number of sensors
 * Input: none
 * Output: number
 */
#define CMD_GET_SENSOR_NUM          0x4

/*
 * CMD_GET_SENSOR_STATUS - Get sensor status
 * Input: sensor ID, type
 * Output: sensor status
 */
#define CMD_GET_SENSOR_STATUS       0x5

/* Sensor types */
#define SENSOR_INFO_TYPE            0
#define SENSOR_INFO_TYPE_TEMP       1

/* Fan commands */
/*
 * CMD_GET_FAN_NUM - Get number of fans
 * Input: none
 * Output: number
 */
#define CMD_GET_FAN_NUM             0x6

/*
 * CMD_GET_FAN_INFO - Get fan status
 * Input: fan ID, type
 * Output: fan info
 */
#define CMD_GET_FAN_INFO            0x7

/*
 * CMD_SET_FAN_INFO - Set fan status
 * Input: fan ID, type, value
 * Output: none
 */
#define CMD_SET_FAN_INFO            0x8

/* Fan types */
#define FAN_INFO_TYPE_LEVEL         0

/* DVFS commands */
/*
 * CMD_GET_FREQ_LEVEL_NUM - Get number of freq levels
 * Input: none
 * Output: number
 */
#define CMD_GET_FREQ_LEVEL_NUM      0x9

/*
 * CMD_GET_FREQ_BOOST_LEVEL - Get number of boost levels
 * Input: none
 * Output: number
 */
#define CMD_GET_FREQ_BOOST_LEVEL    0x10

/*
 * CMD_GET_FREQ_LEVEL_INFO - Get freq level info
 * Input: level ID
 * Output: level info
 */
#define CMD_GET_FREQ_LEVEL_INFO     0x11

/*
 * CMD_GET_FREQ_INFO - Get freq info
 * Input: CPU ID, type
 * Output: freq info
 */
#define CMD_GET_FREQ_INFO           0x12

/*
 * CMD_SET_FREQ_INFO - Set freq info
 * Input: CPU ID, type, value
 * Output: none
 */
#define CMD_SET_FREQ_INFO           0x13

/* Freq types */
#define FREQ_INFO_TYPE_FREQ         0
#define FREQ_INFO_TYPE_LEVEL        1

static inline int do_service_request(union smc_message *msg)
{
	int retries;
	union smc_message last;

	last.value = iocsr_read32(LOONGARCH_IOCSR_SMCMBX);
	if (!last.complete)
		return -1;

	iocsr_write32(msg->value, LOONGARCH_IOCSR_SMCMBX);
	iocsr_write32(iocsr_read32(LOONGARCH_IOCSR_MISC_FUNC) | IOCSR_MISC_FUNC_SOFT_INT,
			LOONGARCH_IOCSR_MISC_FUNC);

	for (retries = 0; retries < 10000; retries++) {
		msg->value = iocsr_read32(LOONGARCH_IOCSR_SMCMBX);
		if (msg->complete)
			break;

		usleep_range(4, 8);
	}

	if (!msg->complete || msg->cmd != CMD_OK)
		return -1;

	return 0;
}

static int boost_supported = 0;
static struct mutex cpufreq_mutex[MAX_PACKAGES];

enum freq {
	FREQ_LEV0, /* Reserved */
	FREQ_LEV1, FREQ_LEV2, FREQ_LEV3, FREQ_LEV4,
	FREQ_LEV5, FREQ_LEV6, FREQ_LEV7, FREQ_LEV8,
	FREQ_LEV9, FREQ_LEV10, FREQ_LEV11, FREQ_LEV12,
	FREQ_LEV13, FREQ_LEV14, FREQ_LEV15, FREQ_LEV16,
	FREQ_RESV
};

/* For Loongson-3A5000, support boost */
static struct cpufreq_frequency_table loongson3_cpufreq_table[] = {
	{0, FREQ_LEV0, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV1, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV2, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV3, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV4, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV5, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV6, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV7, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV8, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV9, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV10, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV11, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV12, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV13, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV14, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV15, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_LEV16, CPUFREQ_ENTRY_INVALID},
	{0, FREQ_RESV, CPUFREQ_TABLE_END},
};

static unsigned int loongson3_cpufreq_get(unsigned int cpu)
{
	union smc_message msg;

	msg.id = cpu;
	msg.info = FREQ_INFO_TYPE_FREQ;
	msg.cmd = CMD_GET_FREQ_INFO;
	msg.extra = 0;
	msg.complete = 0;
	do_service_request(&msg);

	return msg.val * 1000;
}

static int loongson3_cpufreq_set(struct cpufreq_policy *policy, int freq_level)
{
	uint32_t core_id = cpu_data[policy->cpu].core;
	union smc_message msg;

	msg.id = core_id;
	msg.info = FREQ_INFO_TYPE_LEVEL;
	msg.val = freq_level;
	msg.cmd = CMD_SET_FREQ_INFO;
	msg.extra = 0;
	msg.complete = 0;
	do_service_request(&msg);

	return 0;
}

/*
 * Here we notify other drivers of the proposed change and the final change.
 */
static int loongson3_cpufreq_target(struct cpufreq_policy *policy,
				     unsigned int index)
{
	unsigned int cpu = policy->cpu;
	unsigned int package = cpu_data[cpu].package;

	if (!cpu_online(cpu))
		return -ENODEV;

	/* setting the cpu frequency */
	mutex_lock(&cpufreq_mutex[package]);
	loongson3_cpufreq_set(policy, index);
	mutex_unlock(&cpufreq_mutex[package]);

	return 0;
}

static int loongson3_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	if (!cpu_online(policy->cpu))
		return -ENODEV;

	policy->cur = loongson3_cpufreq_get(policy->cpu);

	policy->cpuinfo.transition_latency = 5000;
	policy->freq_table = loongson3_cpufreq_table;
	cpumask_copy(policy->cpus, topology_sibling_cpumask(policy->cpu));

	return 0;
}

static int loongson3_cpufreq_exit(struct cpufreq_policy *policy)
{
	return 0;
}

static struct cpufreq_driver loongson3_cpufreq_driver = {
	.name = "loongson3",
	.flags = CPUFREQ_CONST_LOOPS,
	.init = loongson3_cpufreq_cpu_init,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = loongson3_cpufreq_target,
	.get = loongson3_cpufreq_get,
	.exit = loongson3_cpufreq_exit,
	.attr = cpufreq_generic_attr,
};

static struct platform_device_id cpufreq_id_table[] = {
	{ "loongson3_cpufreq", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(platform, cpufreq_id_table);

static struct platform_driver cpufreq_driver = {
	.driver = {
		.name = "loongson3_cpufreq",
		.owner = THIS_MODULE,
	},
	.id_table = cpufreq_id_table,
};

static int configure_cpufreq_info(void)
{
	int i, r, boost_level, max_level;
	union smc_message msg;

	if (!cpu_has_csr)
		return -EPERM;

	msg.cmd = CMD_GET_VERSION;
	msg.extra = 0;
	msg.complete = 0;
	r = do_service_request(&msg);
	if (r < 0 || msg.val < 0x1)
		return -EPERM;

	msg.id = FEATURE_DVFS;
	msg.val = FEATURE_DVFS_ENABLE | FEATURE_DVFS_BOOST;
	msg.cmd = CMD_SET_FEATURE;
	msg.extra = 0;
	msg.complete = 0;
	r = do_service_request(&msg);
	if (r < 0)
		return -EPERM;

	msg.cmd = CMD_GET_FREQ_LEVEL_NUM;
	msg.extra = 0;
	msg.complete = 0;
	r = do_service_request(&msg);
	if (r < 0)
		return -EPERM;
	max_level = msg.val;

	msg.cmd = CMD_GET_FREQ_BOOST_LEVEL;
	msg.extra = 0;
	msg.complete = 0;
	r = do_service_request(&msg);
	if (r < 0)
		return -EPERM;
	boost_level = msg.val;
	if (boost_level < max_level)
		boost_supported = 1;

	for (i = 0; i < max_level; i++) {
		msg.cmd = CMD_GET_FREQ_LEVEL_INFO;
		msg.id = i;
		msg.info = FREQ_INFO_TYPE_FREQ;
		msg.extra = 0;
		msg.complete = 0;
		r = do_service_request(&msg);
		if (r < 0)
			return -EPERM;

		loongson3_cpufreq_table[i].frequency = msg.val * 1000;
		if (i >= boost_level)
			loongson3_cpufreq_table[i].flags = CPUFREQ_BOOST_FREQ;
	}

	return 0;
}

static int __init cpufreq_init(void)
{
	int i, ret;

	ret = platform_driver_register(&cpufreq_driver);
	if (ret)
		goto err;

	ret = configure_cpufreq_info();
	if (ret)
		goto err;

	for (i = 0; i < MAX_PACKAGES; i++)
		mutex_init(&cpufreq_mutex[i]);

	ret = cpufreq_register_driver(&loongson3_cpufreq_driver);

	if (boost_supported)
		cpufreq_enable_boost_support();

	pr_info("cpufreq: Loongson-3 CPU frequency driver.\n");

	return ret;

err:
	platform_driver_unregister(&cpufreq_driver);
	return ret;
}

static void __exit cpufreq_exit(void)
{
	cpufreq_unregister_driver(&loongson3_cpufreq_driver);
	platform_driver_unregister(&cpufreq_driver);
}

module_init(cpufreq_init);
module_exit(cpufreq_exit);

MODULE_AUTHOR("Huacai Chen <chenhuacaic@loongson.cn>");
MODULE_DESCRIPTION("CPUFreq driver for Loongson-3 processors");
MODULE_LICENSE("GPL");
