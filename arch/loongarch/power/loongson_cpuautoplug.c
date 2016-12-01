/*
 * CPU Autoplug driver for the Loongson processors
 *
 * Copyright (C) 2010 - 2012 Lemote Inc.
 * Copyright (C) 2024 - 2025 Loongson Technology Corporation Limited.
 * Author: Huacai Chen, chenhuacai@loongson.cn
 */
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/kernel_stat.h>
#include <linux/platform_device.h>

int autoplug_enabled = 0;
int autoplug_verbose = 0;
int autoplug_adjusting = 0;

struct cpu_autoplug_info {
	u64 prev_idle;
	u64 prev_wall;
	struct delayed_work work;
	unsigned int sampling_rate;
	int maxcpus;   /* max cpus for autoplug */
	int mincpus;   /* min cpus for autoplug */
	int dec_reqs;  /* continous core-decreasing requests */
};

struct cpu_autoplug_info ap_info;

static ssize_t show_autoplug_enabled(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", autoplug_enabled);
}

static ssize_t store_autoplug_enabled(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int n;
	char val[5];

	memcpy(val, buf, count);
	n = simple_strtol(val, NULL, 0);

	if (n > 1 || n < 0)
		return -EINVAL;

	autoplug_enabled = n;

	return count;
}

static ssize_t show_autoplug_verbose(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", autoplug_verbose);
}

static ssize_t store_autoplug_verbose(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int n;
	char val[5];

	memcpy(val, buf, count);
	n = simple_strtol(val, NULL, 0);

	if (n > 1 || n < 0)
		return -EINVAL;

	autoplug_verbose = n;

	return count;
}

static ssize_t show_autoplug_maxcpus(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_info.maxcpus);
}

static ssize_t store_autoplug_maxcpus(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int n;
	char val[5];

	memcpy(val, buf, count);
	n = simple_strtol(val, NULL, 0);

	if (n > nr_cpu_ids || n < ap_info.mincpus)
		return -EINVAL;

	ap_info.maxcpus = n;

	return count;
}

static ssize_t show_autoplug_mincpus(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_info.mincpus);
}

static ssize_t store_autoplug_mincpus(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int n;
	char val[5];

	memcpy(val, buf, count);
	n = simple_strtol(val, NULL, 0);

	if (n > ap_info.maxcpus || n < 1)
		return -EINVAL;

	ap_info.mincpus = n;

	return count;
}

static ssize_t show_autoplug_sampling_rate(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_info.sampling_rate);
}

#define SAMPLING_RATE_MIN 600
#define SAMPLING_RATE_MAX 1000

static ssize_t store_autoplug_sampling_rate(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int n;
	char val[6];

	memcpy(val, buf, count);
	n = simple_strtol(val, NULL, 0);

	if (n > SAMPLING_RATE_MAX || n < SAMPLING_RATE_MIN)
		return -EINVAL;

	ap_info.sampling_rate = n;

	return count;
}

static ssize_t show_autoplug_available_values(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "enabled: 0-1\nverbose: 0-1\n"
			"maxcpus: 1-%d\nmincpus: 1-%d\nsampling_rate: %d-%d\n",
			num_possible_cpus(), num_possible_cpus(), SAMPLING_RATE_MIN, SAMPLING_RATE_MAX);
}

static DEVICE_ATTR(enabled, 0644, show_autoplug_enabled, store_autoplug_enabled);
static DEVICE_ATTR(verbose, 0644, show_autoplug_verbose, store_autoplug_verbose);
static DEVICE_ATTR(maxcpus, 0644, show_autoplug_maxcpus, store_autoplug_maxcpus);
static DEVICE_ATTR(mincpus, 0644, show_autoplug_mincpus, store_autoplug_mincpus);
static DEVICE_ATTR(sampling_rate, 0644, show_autoplug_sampling_rate, store_autoplug_sampling_rate);
static DEVICE_ATTR(available_values, 0444, show_autoplug_available_values, NULL);

static struct attribute *cpuclass_default_attrs[] = {
	&dev_attr_enabled.attr,
	&dev_attr_verbose.attr,
	&dev_attr_maxcpus.attr,
	&dev_attr_mincpus.attr,
	&dev_attr_sampling_rate.attr,
	&dev_attr_available_values.attr,
	NULL
};

static struct attribute_group cpuclass_attr_group = {
	.attrs = cpuclass_default_attrs,
	.name = "cpuautoplug",
};

#ifndef MODULE
/*
 * Enable / Disable CPU Autoplug
 */
static int __init setup_autoplug(char *str)
{
	if (!strcmp(str, "on"))
		autoplug_enabled = 1;
	else if (!strcmp(str, "off"))
		autoplug_enabled = 0;
	else
		pr_warn("Unknown autoplug setting \"%s\".\n", str);

	return 1;
}

__setup("autoplug=", setup_autoplug);

#endif

static inline u64 global_idle_time_jiffy(u64 *wall)
{
	unsigned int cpu;
	u64 idle_time = 0;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_nsecs(get_jiffies_64());

	for_each_online_cpu(cpu) {
		busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
		busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
		busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
		busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
		busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
		busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

		idle_time += cur_wall_time - busy_time;
	}

	if (wall)
		*wall = div_u64(cur_wall_time, NSEC_PER_USEC);

	return div_u64(idle_time, NSEC_PER_USEC);
}

static inline u64 global_idle_time(u64 *wall)
{
	unsigned int cpu;
	u64 idle_time = 0;

	for_each_online_cpu(cpu) {
		idle_time += get_cpu_idle_time_us(cpu, wall);
		if (idle_time == -1ULL)
			return global_idle_time_jiffy(wall);
	}

	return idle_time;
}

static void increase_cores(int cur_cpus)
{
	int target_cpu;

	if (cur_cpus == ap_info.maxcpus)
		return;

	target_cpu = cpumask_next_zero(0, cpu_online_mask);
	add_cpu(target_cpu); /* add_cpu() is safe that performs extra housekeeping bits */
}


static void decrease_cores(int cur_cpus)
{
	int target_cpu;

	if (cur_cpus == ap_info.mincpus)
		return;

	target_cpu = find_last_bit(cpumask_bits(cpu_online_mask), nr_cpu_ids);
	remove_cpu(target_cpu); /* remove_cpu() is safe that performs extra housekeeping bits */
}

#define INC_THRESHOLD 95
#define DEC_THRESHOLD 10

static void do_autoplug_timer(struct work_struct *work)
{
	int delay, load;
	int nr_cur_cpus = num_online_cpus();
	int nr_all_cpus = num_possible_cpus();
	unsigned int idle_time, wall_time;
	u64 cur_wall_time = 0, cur_idle_time;

	BUG_ON(smp_processor_id() != 0);
	delay = msecs_to_jiffies(ap_info.sampling_rate);
	if (!autoplug_enabled || system_state != SYSTEM_RUNNING)
		goto out;

	autoplug_adjusting = 1;

	/* user limits */
	if (nr_cur_cpus > ap_info.maxcpus) {
		decrease_cores(nr_cur_cpus);
		goto out;
	}
	if (nr_cur_cpus < ap_info.mincpus) {
		increase_cores(nr_cur_cpus);
		goto out;
	}

	/* based on cpu load */
	cur_idle_time = global_idle_time(&cur_wall_time);
	if (cur_wall_time == 0) {
		cur_wall_time = jiffies64_to_nsecs(get_jiffies_64());
		cur_wall_time = div_u64(cur_wall_time, NSEC_PER_USEC);
	}

	wall_time = (unsigned int)(cur_wall_time - ap_info.prev_wall);
	ap_info.prev_wall = cur_wall_time;

	idle_time = (unsigned int)(cur_idle_time - ap_info.prev_idle);
	idle_time += wall_time * (nr_all_cpus - nr_cur_cpus);
	ap_info.prev_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time * nr_all_cpus < idle_time))
		goto out;

	load = 100 * (wall_time * nr_all_cpus - idle_time) / wall_time;

	if (load < (nr_cur_cpus - 1) * 100 - DEC_THRESHOLD) {
		if (ap_info.dec_reqs <= 2)
			ap_info.dec_reqs++;
		else {
			ap_info.dec_reqs = 0;
			decrease_cores(nr_cur_cpus);
		}
	} else {
		ap_info.dec_reqs = 0;
		if (load > (nr_cur_cpus - 1) * 100 + INC_THRESHOLD)
			increase_cores(nr_cur_cpus);
	}

out:
	autoplug_adjusting = 0;
	schedule_delayed_work_on(0, &ap_info.work, delay);
}

static struct platform_device_id platform_device_ids[] = {
	{
		.name = "cpuautoplug",
	},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(platform, platform_device_ids);

static struct platform_driver platform_driver = {
	.driver = {
		.name = "cpuautoplug",
		.owner = THIS_MODULE,
	},
	.id_table = platform_device_ids,
};

static int __init cpuautoplug_init(void)
{
	int ret, delay;
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);

	ret = sysfs_create_group(&dev_root->kobj, &cpuclass_attr_group);
	if (ret)
		return ret;

	/* Register platform stuff */
	ret = platform_driver_register(&platform_driver);
	if (ret)
		return ret;

	pr_info("cpuautoplug: Loongson CPU autoplug driver.\n");

	ap_info.maxcpus = setup_max_cpus > nr_cpu_ids ? nr_cpu_ids : setup_max_cpus;
	ap_info.mincpus = 1;
	ap_info.dec_reqs = 0;
	ap_info.sampling_rate = 800;  /* 800 ms */
	if (setup_max_cpus == 0) {  /* boot with nosmp */
		ap_info.maxcpus = 1;
		autoplug_enabled = 0;
	}
	if (setup_max_cpus > num_possible_cpus())
		ap_info.maxcpus = num_possible_cpus();
#ifdef MODULE
	delay = msecs_to_jiffies(ap_info.sampling_rate * 8);
#else
	delay = msecs_to_jiffies(ap_info.sampling_rate * 32);
#endif
	INIT_DEFERRABLE_WORK(&ap_info.work, do_autoplug_timer);
	schedule_delayed_work_on(0, &ap_info.work, delay);

	return ret;
}

static void __exit cpuautoplug_exit(void)
{
	struct device *dev_root = bus_get_dev_root(&cpu_subsys);

	cancel_delayed_work_sync(&ap_info.work);
	platform_driver_unregister(&platform_driver);
	sysfs_remove_group(&dev_root->kobj, &cpuclass_attr_group);
}

module_init(cpuautoplug_init);
module_exit(cpuautoplug_exit);

MODULE_AUTHOR("Huacai Chen <chenhuacai@loongson.cn>");
MODULE_DESCRIPTION("CPUAutoplug driver for Loongson");
MODULE_LICENSE("GPL");
