#include <linux/seq_file.h>
#include <drm/drm_debugfs.h>
#include "loonggpu.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_pm.h"

#define FREQ_LEVEL0 0x1
#define FREQ_LEVEL1 0x3
#define FREQ_LEVEL2 0x5
#define FREQ_LEVEL3 0x7
#define FREQ_LEVEL4 0x9
#define FREQ_LEVEL5 0xb
#define FREQ_LEVEL6 0xd
#define FREQ_LEVEL7 0xf

#define SET_LEVEL0 0
#define SET_LEVEL1 1
#define SET_LEVEL2 2
#define SET_LEVEL3 3
#define SET_LEVEL4 4
#define SET_LEVEL5 5
#define SET_LEVEL6 6
#define SET_LEVEL7 7

#define STATIC_FREQ 0x00

static ssize_t loonggpu_get_gpu_clk(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	struct gpu_resource *gpu_res = NULL;

	int i, now, size = 0;
	int number = 8;
	uint32_t regular_freq_count;
	uint32_t max_freq_value;
	uint32_t value;
	uint32_t freq;
	uint32_t default_freq_count = 3;
	uint32_t default_freq_value = 480;

	gpu_res = dc_get_vbios_resource(adev->dc->vbios, 0,
					LOONGGPU_RESOURCE_GPU);

	if (NULL == gpu_res) {
		regular_freq_count = default_freq_count;
		max_freq_value = default_freq_value;
	} else {
		regular_freq_count = gpu_res->count_freq;
		max_freq_value = gpu_res->shaders_freq;
	}
	number -= regular_freq_count - 1;
	value = max_freq_value / 8 * number;
	freq = RREG32(LOONGGPU_FREQ_SCALE);

	switch (freq) {
	case FREQ_LEVEL0:
		now = SET_LEVEL0;
		break;
	case FREQ_LEVEL1:
		now = SET_LEVEL1;
		break;
	case FREQ_LEVEL2:
		now = SET_LEVEL2;
		break;
	case FREQ_LEVEL3:
		now = SET_LEVEL3;
		break;
	case FREQ_LEVEL4:
		now = SET_LEVEL4;
		break;
	case FREQ_LEVEL5:
		now = SET_LEVEL5;
		break;
	case FREQ_LEVEL6:
		now = SET_LEVEL6;
		break;
	case FREQ_LEVEL7:
		now = SET_LEVEL7;
		break;
	default:
		now = SET_LEVEL7;
		break;
	}

	for (i = 0; i < regular_freq_count; i++) {
		size += sprintf(buf + size, "%d: %uMhz %s\n", i, value,
				(8 - regular_freq_count + i == now) ? "*" : "");
		number = number + 1;
		value = max_freq_value / 8 * number;
	}

	return size;
}

static ssize_t loonggpu_read_level(const char *buf, size_t count,
				   uint32_t max_level)
{
	int ret;
	char *tmp;
	char **str;
	char *sub_str = NULL;
	char buf_cpy[100];
	const char delimiter[3] = { ' ', '\n', '\0' };

	memcpy(buf_cpy, buf, count);
	tmp = buf_cpy;

	if (NULL == tmp)
		return -EINVAL;

	sub_str = strsep(&tmp, delimiter);
	if (0 == strlen(sub_str))
		return -EINVAL;

	ret = simple_strtoul(sub_str, str, 10);
	if (ret >= 0 && ret <= (max_level - 1))
		return ret;
	else
		return -EINVAL;
}

static ssize_t loonggpu_set_gpu_clk(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	struct gpu_resource *gpu_res = NULL;
	uint32_t level;
	uint32_t regular_freq_count;
	uint32_t default_freq_count = 3;

	gpu_res = dc_get_vbios_resource(adev->dc->vbios, 0,
					LOONGGPU_RESOURCE_GPU);

	if (NULL == gpu_res)
		regular_freq_count = default_freq_count;
	else
		regular_freq_count = gpu_res->count_freq;

	level = loonggpu_read_level(buf, count, regular_freq_count);

	level = 8 - default_freq_count + level;
	switch (level) {
	case SET_LEVEL0:
		level = FREQ_LEVEL0;
		break;
	case SET_LEVEL1:
		level = FREQ_LEVEL1;
		break;
	case SET_LEVEL2:
		level = FREQ_LEVEL2;
		break;
	case SET_LEVEL3:
		level = FREQ_LEVEL3;
		break;
	case SET_LEVEL4:
		level = FREQ_LEVEL4;
		break;
	case SET_LEVEL5:
		level = FREQ_LEVEL5;
		break;
	case SET_LEVEL6:
		level = FREQ_LEVEL6;
		break;
	case SET_LEVEL7:
		level = FREQ_LEVEL7;
		break;
	default:
		level = FREQ_LEVEL7;
		break;
	}

	if (adev->family_type == CHIP_LG100)
		loonggpu_cmd_exec(adev, GSCMD(GSCMD_FREQ, STATIC_FREQ), level,
				  0);
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211) {
		loonggpu_cmd_exec(adev,
				  LG2XX_ICMD32(LG2XX_ICMD32_MOP_FREQ,
					       LG2XX_ICMD32_SOP_FREQ_UFRQ),
				  level, 0);
	} else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__,
			  adev->family_type);

	return count;
}

/*
 * Worst case: 32 bits individually specified, in octal at 12 characters
 * per line (+1 for \n).
 */
#define LOONGGPU_MASK_BUF_MAX (32 * 13)

static ssize_t loonggpu_read_mask(const char *buf, size_t count, uint32_t *mask)
{
	int ret;
	unsigned long level;
	char *sub_str = NULL;
	char *tmp;
	char buf_cpy[LOONGGPU_MASK_BUF_MAX + 1];
	const char delimiter[3] = { ' ', '\n', '\0' };
	size_t bytes;

	*mask = 0;

	bytes = min(count, sizeof(buf_cpy) - 1);
	memcpy(buf_cpy, buf, bytes);
	buf_cpy[bytes] = '\0';
	tmp = buf_cpy;
	while (tmp[0]) {
		sub_str = strsep(&tmp, delimiter);
		if (strlen(sub_str)) {
			ret = kstrtoul(sub_str, 0, &level);
			if (ret || level > 31)
				return -EINVAL;
			*mask = level;
		} else
			break;
	}

	return 0;
}

static ssize_t loonggpu_get_dpm_state(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	enum loonggpu_pm_state_type pm;

	mutex_lock(&adev->pm.mutex);
	pm = adev->pm.dpm.state;
	mutex_unlock(&adev->pm.mutex);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			(pm == POWER_STATE_TYPE_ONDEMAND) ? "ondemand" :
							    "unknown");
}

static ssize_t loonggpu_set_dpm_state(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	enum loonggpu_pm_state_type state;
	int ret = 0;

	if (strncmp("ondemand", buf, strlen("ondemand")) == 0)
		state = POWER_STATE_TYPE_ONDEMAND;
	else {
		count = -EINVAL;
		goto fail;
	}

	mutex_lock(&adev->pm.mutex);
	adev->pm.dpm.state = state;

	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->update_state)
		ret = loonggpu_dpm_update_state(adev);

	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;
fail:
	return count;
}

/**
 * DOC: power_dpm_force_performance_level
 *
 * The loonggpu driver provides a sysfs API for adjusting certain power
 * related parameters.  The file power_dpm_force_performance_level is
 * used for this.  It accepts the following arguments:
 *
 * - auto
 *
 * - low
 *
 * - high
 *
 * - manual
 *
 * auto
 *
 * When auto is selected, the driver will attempt to dynamically select
 * the optimal power profile for current conditions in the driver.
 *
 * low
 *
 * When low is selected, the clocks are forced to the lowest power state.
 *
 * high
 *
 * When high is selected, the clocks are forced to the highest power state.
 *
 * manual
 *
 * When manual is selected, the user can manually adjust which power states
 * are enabled for each clock domain via the sysfs dpm_mclk, dpm_sclk.
 *
 */

static ssize_t loonggpu_get_dpm_forced_performance_level(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	enum loonggpu_dpm_forced_level level = 0xff;

	mutex_lock(&adev->pm.mutex);
	level = adev->pm.dpm.forced_level;
	mutex_unlock(&adev->pm.mutex);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			(level == LOONGGPU_DPM_FORCED_LEVEL_AUTO) ? "auto" :
			(level == LOONGGPU_DPM_FORCED_LEVEL_LOW)  ? "low" :
			(level == LOONGGPU_DPM_FORCED_LEVEL_HIGH) ? "high" :
			(level == LOONGGPU_DPM_FORCED_LEVEL_MANUAL) ?
								    "manual" :
								    "unknown");
}

static ssize_t
loonggpu_set_dpm_forced_performance_level(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	enum loonggpu_dpm_forced_level level;
	int ret = 0;

	if (strncmp("low", buf, strlen("low")) == 0) {
		level = LOONGGPU_DPM_FORCED_LEVEL_LOW;
	} else if (strncmp("high", buf, strlen("high")) == 0) {
		level = LOONGGPU_DPM_FORCED_LEVEL_HIGH;
	} else if (strncmp("auto", buf, strlen("auto")) == 0) {
		level = LOONGGPU_DPM_FORCED_LEVEL_AUTO;
	} else if (strncmp("manual", buf, strlen("manual")) == 0) {
		level = LOONGGPU_DPM_FORCED_LEVEL_MANUAL;
	} else {
		count = -EINVAL;
		goto fail;
	}

	mutex_lock(&adev->pm.mutex);
	adev->pm.dpm.forced_level = level;

	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->update_state)
		ret = loonggpu_dpm_update_state(adev);

	mutex_unlock(&adev->pm.mutex);

	if (ret)
		return ret;
fail:
	return count;
}

static ssize_t loonggpu_get_pm_dpm_sclk(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret = 0;

	if (adev->family_type == CHIP_LG100 || !adev->pm.dpm_enabled)
		return loonggpu_get_gpu_clk(dev, attr, buf);

	mutex_lock(&adev->pm.mutex);
	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->print_clock_levels)
		ret = loonggpu_dpm_print_clock_levels(adev, DPM_SCLK, buf);

	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "\n");
}

static ssize_t loonggpu_set_pm_dpm_sclk(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret;
	uint32_t mask = 0;

	if (adev->family_type == CHIP_LG100 || !adev->pm.dpm_enabled)
		return loonggpu_set_gpu_clk(dev, attr, buf, count);

	ret = loonggpu_read_mask(buf, count, &mask);
	if (ret)
		return ret;

	mutex_lock(&adev->pm.mutex);
	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->force_clock_level)
		ret = loonggpu_dpm_force_clock_level(adev, DPM_SCLK, mask);

	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;

	return count;
}

static ssize_t loonggpu_get_pm_dpm_mclk(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret = 0;

	mutex_lock(&adev->pm.mutex);
	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->print_clock_levels)
		ret = loonggpu_dpm_print_clock_levels(adev, DPM_MCLK, buf);

	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "\n");
}

static ssize_t loonggpu_set_pm_dpm_mclk(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret;
	uint32_t mask = 0;

	ret = loonggpu_read_mask(buf, count, &mask);
	if (ret)
		return ret;

	mutex_lock(&adev->pm.mutex);
	if (adev->pm.dpm_funcs && adev->pm.dpm_funcs->force_clock_level)
		ret = loonggpu_dpm_force_clock_level(adev, DPM_MCLK, mask);

	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;

	return count;
}

static ssize_t loonggpu_get_gpu_capture_flags(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret = 0;

	/* sanity check get capture flags is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->get_capture_flags))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	ret = loonggpu_dpm_get_capture_flags(adev, buf);
	mutex_unlock(&adev->pm.mutex);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "\n");
}

static ssize_t loonggpu_set_gpu_capture_flags(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int ret;
	long int value;

	ret = kstrtol(buf, 0, &value);
	if (ret) {
		count = -EINVAL;
		goto fail;
	}

	/* sanity set capture flags flags is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->set_capture_flags))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	ret = loonggpu_dpm_set_capture_flags(adev, (uint32_t)value);
	mutex_unlock(&adev->pm.mutex);
	if (ret)
		count = -EINVAL;
fail:
	return count;
}

/**
 * DOC: busy_percent
 *
 * The loonggpu driver provides a sysfs API for reading how busy the GPU
 * is as a percentage.  The file gpu_busy_percent is used for this.
 * The dvfs computes a percentage of load based on the
 * aggregate activity level in the IP cores.
 */
static ssize_t loonggpu_get_busy_percent(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	int r, value, size = sizeof(value);

	/* sanity check read sensor is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->read_sensor))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	/* read the IP busy sensor */
	r = loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GPU_LOAD,
				     (void *)&value, &size);
	mutex_unlock(&adev->pm.mutex);
	if (r)
		return r;

	return snprintf(buf, PAGE_SIZE, "%d\n", value / 100);
}

static ssize_t loonggpu_get_gpu_pcnt(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	char str[LOONGGPU_PM_STR_MAX] = { 0 };
	int r, size = sizeof(str);

	/* sanity check read sensor is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->read_sensor))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	/* read the IP pcnt sensor */
	r = loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GFX_PCNT,
				     (void *)str, &size);
	mutex_unlock(&adev->pm.mutex);
	if (r)
		return r;

	return snprintf(buf, PAGE_SIZE, "%s", str);
}

static ssize_t loonggpu_get_gpu_evnt(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	char str[LOONGGPU_PM_STR_MAX] = { 0 };
	int r, size = sizeof(str);

	/* sanity check read sensor is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->read_sensor))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	/* read the IP evnt sensor */
	r = loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GFX_EVNT,
				     (void *)str, &size);
	mutex_unlock(&adev->pm.mutex);
	if (r)
		return r;

	return snprintf(buf, PAGE_SIZE, "%s", str);
}

static ssize_t loonggpu_get_gpu_counter(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct loonggpu_device *adev = ddev->dev_private;
	uint64_t value;
	int r, size = sizeof(value);

	/* sanity check read sensor is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->read_sensor))
		return -EINVAL;

	mutex_lock(&adev->pm.mutex);
	/* read the IP busy sensor */
	r = loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GFX_COUNTER,
				     (void *)&value, &size);
	mutex_unlock(&adev->pm.mutex);
	if (r)
		return r;

	return snprintf(buf, PAGE_SIZE, "%016llx\n", value);
}

static DEVICE_ATTR(power_dpm_state, S_IRUGO | S_IWUSR, loonggpu_get_dpm_state,
		   loonggpu_set_dpm_state);
static DEVICE_ATTR(power_dpm_force_performance_level, S_IRUGO | S_IWUSR,
		   loonggpu_get_dpm_forced_performance_level,
		   loonggpu_set_dpm_forced_performance_level);
static DEVICE_ATTR(pm_dpm_sclk, S_IRUGO | S_IWUSR, loonggpu_get_pm_dpm_sclk,
		   loonggpu_set_pm_dpm_sclk);
static DEVICE_ATTR(pm_dpm_mclk, S_IRUGO | S_IWUSR, loonggpu_get_pm_dpm_mclk,
		   loonggpu_set_pm_dpm_mclk);
static DEVICE_ATTR(gpu_busy_percent, S_IRUGO, loonggpu_get_busy_percent, NULL);
static DEVICE_ATTR(gpu_pcnt, S_IRUGO, loonggpu_get_gpu_pcnt, NULL);
static DEVICE_ATTR(gpu_evnt, S_IRUGO, loonggpu_get_gpu_evnt, NULL);
static DEVICE_ATTR(gpu_counter, S_IRUGO, loonggpu_get_gpu_counter, NULL);
static DEVICE_ATTR(gpu_capture_flags, S_IRUGO | S_IWUSR,
		   loonggpu_get_gpu_capture_flags,
		   loonggpu_set_gpu_capture_flags);
/*
 * Debugfs info
 */
#if defined(CONFIG_DEBUG_FS)

static int loonggpu_debugfs_pm_info_dvfs(struct seq_file *m,
					 struct loonggpu_device *adev)
{
	uint32_t value;
	int size;

	/* sanity check read sensor is enabled */
	if (!(adev->pm.dpm_funcs && adev->pm.dpm_funcs->read_sensor)) {
		seq_printf(m,
			   "Debugfs support not implemented for this asic\n");
		return 0;
	}

	/* GPU Clocks */
	size = sizeof(value);
	seq_printf(m, "GFX Clocks and Power:\n");

	mutex_lock(&adev->pm.mutex);
	if (!loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GFX_MCLK,
				      (void *)&value, &size))
		seq_printf(m, "\t%u MHz (MCLK)\n", (value & 0xffff));
	if (!loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GFX_SCLK,
				      (void *)&value, &size))
		seq_printf(m, "\t%u MHz (SCLK)\n", (value & 0xffff));

	seq_printf(m, "\n");

	/* GPU Load */
	if (!loonggpu_dpm_read_sensor(adev, LOONGGPU_DPM_SENSOR_GPU_LOAD,
				      (void *)&value, &size))
		seq_printf(m, "GPU Load: %u.%02d %%\n", value / 100,
			   value % 100);

	mutex_unlock(&adev->pm.mutex);
	seq_printf(m, "\n");

	return 0;
}

static int loonggpu_debugfs_pm_info(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct loonggpu_device *adev = dev->dev_private;
	u32 flags = 0;

	seq_printf(m, "Clock Gating Flags Mask: 0x%x\n", flags);
	seq_printf(m, "\n");

	if (!adev->pm.dpm_enabled) {
		seq_printf(m, "dpm not enabled\n");
		return 0;
	}

	return loonggpu_debugfs_pm_info_dvfs(m, adev);
}

static const struct drm_info_list loonggpu_pm_info_list[] = {
	{ "loonggpu_pm_info", loonggpu_debugfs_pm_info, 0, NULL },
};
#endif

int loonggpu_pm_sysfs_init(struct loonggpu_device *adev)
{
	int ret;

	struct device *dev = adev->ddev->dev;

	if (adev->family_type == CHIP_NO_GPU)
		return 0;

	mutex_init(&adev->pm.mutex);

	ret = device_create_file(dev, &dev_attr_pm_dpm_sclk);
	if (ret) {
		DRM_ERROR("failed to create device file pp_dpm_sclk\n");
		return ret;
	}

	if (adev->family_type < CHIP_LG200 || !adev->pm.dpm_enabled)
		return 0;

	ret = device_create_file(dev,
				 &dev_attr_power_dpm_force_performance_level);
	if (ret) {
		DRM_ERROR(
		"failed to create device file for dpm force performance level\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_power_dpm_state);
	if (ret) {
		DRM_ERROR("failed to create device file for dpm state\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_pm_dpm_mclk);
	if (ret) {
		DRM_ERROR("failed to create device file pp_dpm_mclk\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_gpu_busy_percent);
	if (ret) {
		DRM_ERROR("failed to create device file	"
			  "gpu_busy_level\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_gpu_capture_flags);
	if (ret) {
		DRM_ERROR("failed to create device file	"
			  "gpu capture flags\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_gpu_pcnt);
	if (ret) {
		DRM_ERROR("failed to create device file	"
			  "gpu_pcnt\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_gpu_evnt);
	if (ret) {
		DRM_ERROR("failed to create device file	"
			  "gpu_evnt\n");
		return ret;
	}

	ret = device_create_file(dev, &dev_attr_gpu_counter);
	if (ret) {
		DRM_ERROR("failed to create device file	"
			  "gpu_counter\n");
		return ret;
	}
#if defined(CONFIG_DEBUG_FS)
	return loonggpu_debugfs_add_files(adev, loonggpu_pm_info_list,
					  ARRAY_SIZE(loonggpu_pm_info_list));
#endif
	return 0;

}

int loonggpu_pm_sysfs_exit(struct loonggpu_device *adev)
{
	struct device *dev = adev->ddev->dev;

	if (adev->family_type == CHIP_NO_GPU)
		return 0;

	device_remove_file(dev, &dev_attr_pm_dpm_sclk);

	if (adev->family_type < CHIP_LG200 || !adev->pm.dpm_enabled)
		return 0;

	device_remove_file(dev, &dev_attr_power_dpm_state);
	device_remove_file(dev, &dev_attr_power_dpm_force_performance_level);
	device_remove_file(dev, &dev_attr_pm_dpm_mclk);
	device_remove_file(dev, &dev_attr_gpu_busy_percent);
	device_remove_file(dev, &dev_attr_gpu_capture_flags);
	device_remove_file(dev, &dev_attr_gpu_pcnt);
	device_remove_file(dev, &dev_attr_gpu_evnt);
	device_remove_file(dev, &dev_attr_gpu_counter);
	mutex_destroy(&adev->pm.mutex);

	return 0;
}
