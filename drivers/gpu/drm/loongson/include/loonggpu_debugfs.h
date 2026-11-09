#ifndef __LOONGGPU_DEBUGFS_H__
#define __LOONGGPU_DEBUGFS_H__

struct loonggpu_debugfs {
	const struct drm_info_list	*files;
	unsigned		num_files;
};

int loonggpu_debugfs_regs_init(struct loonggpu_device *adev);
void loonggpu_debugfs_regs_cleanup(struct loonggpu_device *adev);
int loonggpu_debugfs_init(struct loonggpu_device *adev);
int loonggpu_debugfs_add_files(struct loonggpu_device *adev,
			     const struct drm_info_list *files,
			     unsigned nfiles);
int loonggpu_debugfs_fence_init(struct loonggpu_device *adev);
int loonggpu_debugfs_firmware_init(struct loonggpu_device *adev);
int loonggpu_debugfs_gem_init(struct loonggpu_device *adev);
int loonggpu_debugfs_sema_init(struct loonggpu_device *adev);

#endif /* __LOONGGPU_DEBUGFS_H__ */
