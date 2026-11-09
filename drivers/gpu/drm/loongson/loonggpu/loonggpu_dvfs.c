#include <linux/delay.h>
#include "loonggpu.h"
#include "loonggpu_dvfs.h"
#include "loonggpu_common.h"
#include "loonggpu_dc_vbios.h"

#define LG2XX_POWER_OPCODE 0x51000000

#define LG2XX_ZIP_WXXB_REG_OFFSET 0x18
#define LG2XX_ZIP_WRDLY_REG_OFFSET 0x40

static void dvfs_set_dpm_funcs(struct loonggpu_device *adev);

static uint32_t dvfs_compute_avg_nums(uint32_t *in, uint32_t count,
				      uint32_t scale)
{
	uint64_t nums = 0;
	int i;

	if (!count)
		return 0;

	for (i = 0; i < count; i++)
		nums += in[i];

	nums *= scale;
	nums /= count;

	return (uint32_t)nums;
}

static int dvfs_set_gpu_power_level(struct loonggpu_device *adev,
				    uint32_t level)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;

	if (!level)
		return 0;

	spin_lock(&dvfs->capture_lock);
	if (dvfs->cur_power_level == level) {
		spin_unlock(&dvfs->capture_lock);
		return 0;
	}

	dvfs->cur_power_level = level;
	spin_unlock(&dvfs->capture_lock);

	if (!loonggpu_dpm || !adev->pm.dpm.bios_support)
		return 0;

	mb();

	loonggpu_cmd_exec(
		adev,
		LG2XX_ICMD32(LG2XX_ICMD32_MOP_FREQ,
			     LG2XX_ICMD32_SOP_POWER_LEVEL_UFRQ),
		dvfs->sclk_table.dvfs_levels[level - 1].level |
			LG2XX_POWER_OPCODE,
		0);

	return 0;
}

static int dvfs_get_gpu_power_level(struct loonggpu_device *adev, u32 *value)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;

	spin_lock(&dvfs->capture_lock);
	*value = dvfs->cur_power_level;
	spin_unlock(&dvfs->capture_lock);

	return 0;
}

static uint32_t dvfs_compute_new_power_level(struct loonggpu_dvfs *dvfs,
					     uint32_t load_avg,
					     uint32_t cur_level)
{
	struct loonggpu_dvfs_single_table *ptable = &dvfs->sclk_table;
	int i;

	for (i = 0; i < ptable->count; i++)
		if (load_avg < ptable->dvfs_levels[i].param1)
			break;

	if (i > dvfs->max_power_level)
		i = dvfs->max_power_level;

	return i ? i : 1;
}

static int dvfs_compute_power_level(struct loonggpu_device *adev)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	uint32_t load_avg, cur_power_level;

	dvfs_get_gpu_power_level(adev, &cur_power_level);
	load_avg = dvfs_compute_avg_nums(dvfs->capture_gpu_load_avg,
					 DVFS_MAX_DPM_AVG_TIMES, 1);
	cur_power_level =
		dvfs_compute_new_power_level(dvfs, load_avg, cur_power_level);

	return dvfs_set_gpu_power_level(adev, cur_power_level);
}

static int dvfs_get_gpu_load(struct loonggpu_device *adev, uint32_t *value)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;

	spin_lock(&dvfs->capture_lock);
	*value = dvfs->capture_gpu_load;
	spin_unlock(&dvfs->capture_lock);

	return 0;
}

static int dvfs_set_gpu_load(struct loonggpu_device *adev, uint32_t value)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;

	spin_lock(&dvfs->capture_lock);
	dvfs->capture_gpu_load = value;
	spin_unlock(&dvfs->capture_lock);

	return 0;
}

static int
dvfs_compute_gpu_load_and_adjust_power_level(struct loonggpu_device *adev)
{
	static uint32_t capture_count = 0;
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	uint32_t gpu_load = 0;
	int ret = 0;

	gpu_load = dvfs_compute_avg_nums(dvfs->capture_data,
					 dvfs->capture_period, 10000);
	dvfs->capture_gpu_load_avg[capture_count++] = gpu_load;
	capture_count %= DVFS_MAX_DPM_AVG_TIMES;

	dvfs_set_gpu_load(adev, gpu_load);

	if (!capture_count) {
		ret = dvfs_compute_power_level(adev);
		if (ret)
			return ret;
	}

	return ret;
}

static enum hrtimer_restart dvfs_capture_htimer_handle(struct hrtimer *htimer)
{
	struct loonggpu_dvfs *dvfs =
		container_of(htimer, struct loonggpu_dvfs, capture_htimer);
	struct loonggpu_dpm *dpm =
		container_of(dvfs, struct loonggpu_dpm, dvfs);
	struct loonggpu_device *adev = dpm->handle;
	int ret = 0;

	dvfs->capture_data[dvfs->capture_counter++] =
		!!RREG32(LOONGGPU_LG2XX_IP_STATE);
	dvfs->capture_counter %= dvfs->capture_period;

	if (!dvfs->capture_counter)
		ret = dvfs_compute_gpu_load_and_adjust_power_level(adev);

	if (ret)
		DRM_ERROR("pm dpm dvfs adjust power level failed\r\n");

	hrtimer_forward_now(htimer,
			    ns_to_ktime(1000000000ull / dvfs->capture_period));
	return HRTIMER_RESTART;
}

static uint64_t dvfs_get_gpu_counter(struct loonggpu_device *adev)
{
	uint64_t count;

	count = RREG32(LOONGGPU_TIME_COUNT_LO);
	count |= (uint64_t)RREG32(LOONGGPU_TIME_COUNT_HI) << 32;

	return count;
}

static int dvfs_start_capture_timer(struct loonggpu_device *adev)
{
	ktime_t ktime;

	if (adev->pm.dpm.dvfs.capture_enable)
		return 0;

	if (adev->pm.dpm.dvfs.capture_period) {
		ktime = ktime_set(
			0,
			1000000000 / adev->pm.dpm.dvfs.capture_period); // 1Hz
		hrtimer_start(&adev->pm.dpm.dvfs.capture_htimer, ktime,
			      HRTIMER_MODE_REL);
		adev->pm.dpm.dvfs.capture_enable = true;
	}

	return 0;
}

static void dvfs_cancel_capture_timer(struct loonggpu_device *adev)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;

	if (!dvfs->capture_enable)
		return;

	hrtimer_cancel(&dvfs->capture_htimer);

	dvfs->capture_enable = false;
	dvfs_set_gpu_load(adev, 0);
}

static void dvfs_stop(struct loonggpu_device *adev)
{
	dvfs_cancel_capture_timer(adev);
}

static void dvfs_enable(struct loonggpu_device *adev, bool enable)
{
	if (!enable) {
		dvfs_stop(adev);
		return;
	}

	dvfs_start_capture_timer(adev);
}

static int dvfs_update_dpm_state(struct loonggpu_device *adev, uint32_t level)
{
	int ret;

	ret = dvfs_set_gpu_power_level(adev, level);
	if (ret)
		DRM_ERROR("pm dpm dvfs set power level failed\r\n");

	return ret;
}

static struct loonggpu_dvfs_single_table default_sclk = {
	.count = 4,
	.dvfs_levels[0] = { true, 321, 2000, 9 }, // freq = 4500 / (5 + 9)
	.dvfs_levels[1] = { true, 500, 4000, 4 },
	.dvfs_levels[2] = { true, 750, 5000, 1 },
	.dvfs_levels[3] = { true, 900, 7000, 0 },
};

static void dvfs_init_single_table(struct loonggpu_device *adev)
{
	struct loonggpu_dvfs_single_table *sclk_table =
		&adev->pm.dpm.dvfs.sclk_table;
	struct loonggpu_dvfs_single_table *mclk_table =
		&adev->pm.dpm.dvfs.mclk_table;
	struct loonggpu_dvfs_single_table  *p_sclk_table = &default_sclk;
	u32 sclk = adev->clock.default_sclk / 100;
	struct dpm_config_resource *dpm_config_res = NULL;
	bool dpm_config_active = false;
	int i, j;

	dpm_config_res = dc_get_vbios_resource(adev->dc->vbios, 0,
				LOONGGPU_RESOURCE_DPM_CONFIG);
	if (dpm_config_res && dpm_config_res->enable) {
		p_sclk_table = &dpm_config_res->sclk_table;
		dpm_config_active = true;
	}

	/* init sclk table */
	for (i = 0, j = 0; i < p_sclk_table->count; i++) {
		if (!p_sclk_table->dvfs_levels[i].enabled)
			continue;

		/* driver module default config sclk = 900 Mhz.
		 * If the obtained frequency is not 900 MHz, it
		 * needs to be corrected.
		 */
		if (!dpm_config_active && sclk != 900)
			p_sclk_table->dvfs_levels[i].value = sclk * 5 /
				(5 + p_sclk_table->dvfs_levels[i].level);

		sclk_table->dvfs_levels[j++] = p_sclk_table->dvfs_levels[i];
	}
	sclk_table->count = j;

	/* init mclk table */
	mclk_table->count = 1;
	for (i = 0; i < mclk_table->count; i++) {
		mclk_table->dvfs_levels[i].enabled = true;
		mclk_table->dvfs_levels[i].value = adev->clock.default_mclk / 100;
	}
}

static void dvfs_init_default_para(struct loonggpu_device *adev)
{
	adev->pm.dpm.dvfs.cur_mclk = adev->clock.default_mclk / 100;
	adev->pm.dpm.forced_level = loonggpu_dpm? LOONGGPU_DPM_FORCED_LEVEL_AUTO :
				    LOONGGPU_DPM_FORCED_LEVEL_HIGH;
	adev->pm.dpm.state = POWER_STATE_TYPE_ONDEMAND;
	adev->pm.dpm_enabled = true;
	adev->pm.dpm.dvfs.capture_period = 500; // 500Hz
	adev->pm.dpm.dvfs.max_sclk = adev->clock.default_sclk / 100;
	adev->pm.dpm.dvfs.max_mclk = adev->clock.default_mclk / 100;
	adev->pm.dpm.dvfs.max_power_level = adev->pm.dpm.dvfs.sclk_table.count;
}

static uint32_t dvfs_get_gfx_sclk(struct loonggpu_device *adev)
{
	struct loonggpu_dvfs_single_table *sclk_table =
		&adev->pm.dpm.dvfs.sclk_table;
	u32 level = 0;

	dvfs_get_gpu_power_level(adev, &level);

	return sclk_table->dvfs_levels[level - 1].value;
}

static int dvfs_print_clock_levels(void *handle,
				   enum loonggpu_dpm_clock_type type, char *buf)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	struct loonggpu_dvfs_single_table *sclk_table =
		&adev->pm.dpm.dvfs.sclk_table;
	struct loonggpu_dvfs_single_table *mclk_table =
		&adev->pm.dpm.dvfs.mclk_table;
	int i, now, size = 0;
	uint32_t clock;

	switch (type) {
	case DPM_SCLK:
		clock = dvfs_get_gfx_sclk(adev);
		for (i = 0; i < sclk_table->count; i++) {
			if (clock > sclk_table->dvfs_levels[i].value)
				continue;
			break;
		}
		now = i;

		for (i = 0; i < sclk_table->count; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n", i,
					sclk_table->dvfs_levels[i].value,
					(i == now) ? "*" : "");
		break;
	case DPM_MCLK:
		clock = adev->pm.dpm.dvfs.cur_mclk;

		for (i = 0; i < mclk_table->count; i++) {
			if (clock > mclk_table->dvfs_levels[i].value)
				continue;
			break;
		}
		now = i;

		for (i = 0; i < mclk_table->count; i++)
			size += sprintf(buf + size, "%d: %uMhz %s\n", i,
					mclk_table->dvfs_levels[i].value,
					(i == now) ? "*" : "");
		break;
	default:
		DRM_ERROR("Error type %d\n", type);
		break;
	}

	return size;
}

static int dvfs_force_clock_level(void *handle,
				  enum loonggpu_dpm_clock_type type,
				  uint32_t mask)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	struct loonggpu_dvfs_single_table *sclk_table =
		&adev->pm.dpm.dvfs.sclk_table;
	struct loonggpu_dvfs_single_table *mclk_table =
		&adev->pm.dpm.dvfs.mclk_table;

	if (adev->pm.dpm.forced_level != LOONGGPU_DPM_FORCED_LEVEL_MANUAL)
		return -EINVAL;

	switch (type) {
	case DPM_SCLK:
		if (mask >= sclk_table->count)
			return -EINVAL;

		if (!sclk_table->dvfs_levels[mask].enabled)
			return -EINVAL;

		dvfs_update_dpm_state(adev, mask + 1);
		break;
	case DPM_MCLK:
		if (mask >= mclk_table->count)
			return -EINVAL;

		if (!mclk_table->dvfs_levels[mask].enabled)
			return -EINVAL;

		adev->pm.dpm.dvfs.cur_mclk =
			mclk_table->dvfs_levels[mask].value;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int dvfs_print_gfx_evnt_regs(struct loonggpu_device *adev, void *value,
				    int *size)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	uint64_t *data_ptr = dvfs->evnt_ptr;
	char *buf = value;
	int count = 0, i;

	if (!(dvfs->capture_flags & LOONGGPU_DVFS_CAPTURE_EVNT))
		return -EINVAL;

	for (i = 0; i < DVFS_MAX_EVNT_REG_NUMS; i++) {
		count += sprintf(buf + count, "%016llx\n", data_ptr[i]);
		if (count + 17 > *size)
			return -EINVAL;
	}

	*size = count;

	return 0;
}

static void dvfs_get_gpu_mem_pcnt(struct loonggpu_device *adev, uint32_t *val,
				  uint32_t reg, uint32_t count)
{
	uint64_t tmp;
	int i, j;

	count <<= 2;

	for (i = reg, j = 0; i < reg + count; i += 8) {
		tmp = loonggpu_cmd_exec(adev,
					LG2XX_ICMD32(LG2XX_ICMD32_MOP_ZIP,
						     LG2XX_ICMD32_SOP_ZIP_CNT),
					i, 0);
		val[j++] = lower_32_bits(tmp);
		val[j++] = upper_32_bits(tmp);
	}
}

static int dvfs_print_gfx_pcnt_regs(struct loonggpu_device *adev, void *value,
				    int *size)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	uint32_t pcnt_dat[DVFS_MAX_PCNT_REG_NUMS];
	char *buf = value;
	int count = 0, i;

	if (!(dvfs->capture_flags & LOONGGPU_DVFS_CAPTURE_PCNT))
		return -EINVAL;

	dvfs_get_gpu_mem_pcnt(adev, pcnt_dat, LG2XX_ZIP_WXXB_REG_OFFSET, 8);
	dvfs_get_gpu_mem_pcnt(adev, &pcnt_dat[8], LG2XX_ZIP_WRDLY_REG_OFFSET,
			      24);

	for (i = 0; i < DVFS_MAX_PCNT_REG_NUMS; i++) {
		count += sprintf(buf + count, "%08x\n", pcnt_dat[i]);
		if (count + 9 > *size)
			return -EINVAL;
	}

	*size = count;

	return 0;
}

static int dvfs_read_sensor(void *handle, int idx, void *value, int *size)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	u32 tmp;
	int ret;

	/* size must be at least 4 bytes for all sensors */
	if (*size < 4)
		return -EINVAL;

	switch (idx) {
	case LOONGGPU_DPM_SENSOR_GFX_SCLK:
		ret = dvfs_get_gpu_power_level(adev, &tmp);
		*((uint32_t *)value) =
			dvfs->sclk_table.dvfs_levels[tmp - 1].value;
		*size = 4;
		return 0;
	case LOONGGPU_DPM_SENSOR_GFX_MCLK:
		*((uint32_t *)value) = (uint16_t)dvfs->cur_mclk;
		*size = 4;
		return 0;
	case LOONGGPU_DPM_SENSOR_GPU_LOAD:
		ret = dvfs_get_gpu_load(adev, &tmp);
		*((uint32_t *)value) = tmp;
		*size = 4;
		return 0;
	case LOONGGPU_DPM_SENSOR_GPU_POWER_LEVEL:
		ret = dvfs_get_gpu_power_level(adev, &tmp);
		*((uint32_t *)value) = tmp * 100 / dvfs->max_power_level;
		*size = 4;
		return 0;
	case LOONGGPU_DPM_SENSOR_GFX_EVNT:
		return dvfs_print_gfx_evnt_regs(adev, value, size);
	case LOONGGPU_DPM_SENSOR_GFX_PCNT:
		return dvfs_print_gfx_pcnt_regs(adev, value, size);
	case LOONGGPU_DPM_SENSOR_GFX_COUNTER:
		if (*size < 8)
			return -EINVAL;
		*((uint64_t *)value) = dvfs_get_gpu_counter(adev);
		*size = 8;
		return 0;
	default:
		return -EINVAL;
	}
}

static int dvfs_dpm_update_pcnt_state(struct loonggpu_device *adev)
{
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	uint32_t sop;

	sop = (dvfs->capture_flags & LOONGGPU_DVFS_CAPTURE_PCNT) ?
		      LG2XX_ICMD32_SOP_ZIP_CNTEN :
		      LG2XX_ICMD32_SOP_ZIP_CNTDIS;

	loonggpu_cmd_exec(adev, LG2XX_ICMD32(LG2XX_ICMD32_MOP_ZIP, sop), 0, 0);
	return 0;
}

static int dvfs_get_capture_flags(void *handle, char *buf)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return sprintf(buf, "%08x\n", adev->pm.dpm.dvfs.capture_flags);
}

static int dvfs_set_capture_flags(void *handle, uint32_t value)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	int ret;

	if (value == dvfs->capture_flags)
		return 0;

	dvfs->capture_flags = value;

	ret = dvfs_dpm_update_pcnt_state(adev);

	return ret;
}

static int dvfs_configure_dpm_mode(struct loonggpu_device *adev)
{
	switch (adev->pm.dpm.state) {
	case POWER_STATE_TYPE_ONDEMAND:
		dvfs_enable(adev, true);
		break;
	default:
		DRM_ERROR("Error configure dpm mode\n");
		break;
	}

	return 0;
}

static int dvfs_update_state(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	struct loonggpu_dvfs *dvfs = &adev->pm.dpm.dvfs;
	int ret = 0;

	switch (adev->pm.dpm.forced_level) {
	case LOONGGPU_DPM_FORCED_LEVEL_AUTO:
		ret = dvfs_configure_dpm_mode(adev);
		return ret;
	case LOONGGPU_DPM_FORCED_LEVEL_LOW:
		ret = dvfs_update_dpm_state(adev, 1);
		break;
	case LOONGGPU_DPM_FORCED_LEVEL_MANUAL:
		ret = dvfs_update_dpm_state(adev, dvfs->max_power_level / 2);
		break;
	case LOONGGPU_DPM_FORCED_LEVEL_HIGH:
		ret = dvfs_update_dpm_state(adev, dvfs->max_power_level);
		break;
	default:
		DRM_ERROR("Error dpm forced level\n");
		break;
	}

	dvfs_enable(adev, false);
	return ret;
}

static int dvfs_capture_buffer_alloc(struct loonggpu_device *adev)
{
	int ret;

	if (adev->pm.dpm.dvfs.evnt_bo)
		return 0;

	ret = loonggpu_bo_create_kernel(adev, LOONGGPU_GPU_PAGE_SIZE, PAGE_SIZE,
					LOONGGPU_GEM_DOMAIN_GTT,
					&adev->pm.dpm.dvfs.evnt_bo,
					&adev->pm.dpm.dvfs.evnt_gpu_addr,
					(void **)&adev->pm.dpm.dvfs.evnt_ptr);
	if (ret)
		return ret;

	memset(adev->pm.dpm.dvfs.evnt_ptr, 0, LOONGGPU_GPU_PAGE_SIZE);
	return ret;
}

static void dvfs_capture_buffer_free(struct loonggpu_device *adev)
{
	if (!adev->pm.dpm.dvfs.evnt_bo)
		return;

	loonggpu_bo_free_kernel(&adev->pm.dpm.dvfs.evnt_bo, NULL, NULL);
	adev->pm.dpm.dvfs.evnt_gpu_addr = 0;
	adev->pm.dpm.dvfs.evnt_ptr = NULL;
}

static int dvfs_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	dvfs_set_dpm_funcs(adev);
	adev->pm.dpm.handle = handle;

	return 0;
}

static int dvfs_sw_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;
	size_t size = sizeof(*adev->pm.dpm.dvfs.capture_data);
	int ret;

	dvfs_init_single_table(adev);
	dvfs_init_default_para(adev);

	spin_lock_init(&adev->pm.dpm.dvfs.capture_lock);

#if defined(LG_HRTIMER_INIT)
	hrtimer_init(&adev->pm.dpm.dvfs.capture_htimer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	adev->pm.dpm.dvfs.capture_htimer.function = &dvfs_capture_htimer_handle;
#else
	hrtimer_setup(&adev->pm.dpm.dvfs.capture_htimer, &dvfs_capture_htimer_handle,
			CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#endif

	size *= adev->pm.dpm.dvfs.capture_period;
	adev->pm.dpm.dvfs.capture_data = kzalloc(size, GFP_KERNEL);
	if (!adev->pm.dpm.dvfs.capture_data) {
		DRM_ERROR("Error alloc capture_data array\n");
		return -ENOMEM;
	}

	ret = dvfs_capture_buffer_alloc(adev);
	if (ret)
		return ret;

	return 0;
}

static int dvfs_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (adev->pm.dpm.dvfs.capture_data) {
		kfree(adev->pm.dpm.dvfs.capture_data);
		adev->pm.dpm.dvfs.capture_data = NULL;
	}

	dvfs_capture_buffer_free(adev);

	return 0;
}

static int dvfs_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = dvfs_update_state(adev);
	if (r)
		return r;

	DRM_INFO("LOONGGPU Dvfs Launch!\n");
	return r;
}

static int dvfs_hw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	dvfs_enable(adev, false);
	dvfs_set_gpu_power_level(adev, adev->pm.dpm.dvfs.max_power_level);

	return 0;
}

static int dvfs_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return dvfs_hw_fini(adev);
}

static int dvfs_resume(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = dvfs_hw_init(adev);

	return r;
}

static bool dvfs_is_idle(void *handle)
{
	return true;
}

static int dvfs_wait_for_idle(void *handle)
{
	return 0;
}

static const struct loonggpu_ip_funcs dvfs_ip_funcs = {
	.name = "dvfs",
	.early_init = dvfs_early_init,
	.late_init = NULL,
	.sw_init = dvfs_sw_init,
	.sw_fini = dvfs_sw_fini,
	.hw_init = dvfs_hw_init,
	.hw_fini = dvfs_hw_fini,
	.suspend = dvfs_suspend,
	.resume = dvfs_resume,
	.is_idle = dvfs_is_idle,
	.wait_for_idle = dvfs_wait_for_idle,
};

static const struct loonggpu_dpm_funcs dpm_funcs = {
	.print_clock_levels = dvfs_print_clock_levels,
	.force_clock_level = dvfs_force_clock_level,
	.read_sensor = dvfs_read_sensor,
	.update_state = dvfs_update_state,
	.get_capture_flags = dvfs_get_capture_flags,
	.set_capture_flags = dvfs_set_capture_flags,
};

static void dvfs_set_dpm_funcs(struct loonggpu_device *adev)
{
	if (adev->pm.dpm_funcs == NULL)
		adev->pm.dpm_funcs = &dpm_funcs;
}

const struct loonggpu_ip_block_version dvfs_ip_block_v1_0_0 = {
	.type = LOONGGPU_IP_BLOCK_TYPE_DVFS,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &dvfs_ip_funcs,
};
