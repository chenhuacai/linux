#ifndef __LOONGGPU_PM_H__
#define __LOONGGPU_PM_H__

#define LOONGGPU_PM_MODE_ONDEMAND       0
#define LOONGGPU_PM_MODE_USERSPACE      1
#define LOONGGPU_PM_MODE_POWERSAVE      2
#define LOONGGPU_PM_MODE_PERFORMACE     3

#define LOONGGPU_PM_STR_MAX 512

#define loonggpu_dpm_print_clock_levels(adev, type, buf) \
		((adev)->pm.dpm_funcs->print_clock_levels((adev)->pm.dpm.handle, type, buf))
#define loonggpu_dpm_force_clock_level(adev, type, buf) \
		((adev)->pm.dpm_funcs->force_clock_level((adev)->pm.dpm.handle, type, buf))
#define loonggpu_dpm_read_sensor(adev, idx, value, size) \
		((adev)->pm.dpm_funcs->read_sensor((adev)->pm.dpm.handle, (idx), (value), (size)))
#define loonggpu_dpm_update_state(adev) \
		((adev)->pm.dpm_funcs->update_state((adev)->pm.dpm.handle))
#define loonggpu_dpm_set_capture_flags(adev, flags) \
		((adev)->pm.dpm_funcs->set_capture_flags((adev)->pm.dpm.handle, flags))
#define loonggpu_dpm_get_capture_flags(adev, buf) \
		((adev)->pm.dpm_funcs->get_capture_flags((adev)->pm.dpm.handle, buf))
int loonggpu_pm_sysfs_init(struct loonggpu_device *adev);
int loonggpu_pm_sysfs_exit(struct loonggpu_device *adev);

#endif /* __LOONGGPU_PM_H__ */
