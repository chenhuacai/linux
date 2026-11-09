#ifndef __LOONGGPU_MN_H__
#define __LOONGGPU_MN_H__

/*
 * MMU Notifier
 */
struct loonggpu_mn;

enum loonggpu_mn_type {
	LOONGGPU_MN_TYPE_GFX,
	LOONGGPU_MN_TYPE_VDD,
};

#if defined(CONFIG_MMU_NOTIFIER)
void loonggpu_mn_lock(struct loonggpu_mn *mn);
void loonggpu_mn_unlock(struct loonggpu_mn *mn);
struct loonggpu_mn *loonggpu_mn_get(struct loonggpu_device *adev,
				enum loonggpu_mn_type type);
int loonggpu_mn_register(struct loonggpu_bo *bo, unsigned long addr);
void loonggpu_mn_unregister(struct loonggpu_bo *bo);
#else
static inline void loonggpu_mn_lock(struct loonggpu_mn *mn) {}
static inline void loonggpu_mn_unlock(struct loonggpu_mn *mn) {}
static inline struct loonggpu_mn *loonggpu_mn_get(struct loonggpu_device *adev,
					      enum loonggpu_mn_type type)
{
	return NULL;
}
static inline int loonggpu_mn_register(struct loonggpu_bo *bo, unsigned long addr)
{
	return -ENODEV;
}
static inline void loonggpu_mn_unregister(struct loonggpu_bo *bo) {}
#endif

#endif /* __LOONGGPU_MN_H__ */
