#ifndef __LOONGGPU_SCHEDULER_HELPER_H__
#define __LOONGGPU_SCHEDULER_HELPER_H__

#include <drm/gpu_scheduler.h>

#define LG_DRM_SCHED_PRIORITY_MAX DRM_SCHED_PRIORITY_COUNT
#define LG_DRM_SCHED_PRIORITY_HIGH_HW DRM_SCHED_PRIORITY_HIGH
#define LG_DRM_SCHED_PRIORITY_HIGH_SW DRM_SCHED_PRIORITY_HIGH
#define LG_DRM_SCHED_PRIORITY_INVALID DRM_SCHED_PRIORITY_COUNT

#define LG_DRM_SCHED_PRIORITY_LOW DRM_SCHED_PRIORITY_LOW

#define LG_DRM_SCHED_PRIORITY_UNSET LOONGGPU_CTX_PRIORITY_UNSET

static inline int lg_drm_sched_entity_init(struct drm_sched_entity *entity,
                                          enum drm_sched_priority priority,
                                          struct drm_gpu_scheduler **sched_list,
                                          unsigned int num_sched_list,
                                          struct drm_sched_rq **rq_list,
                                          unsigned int num_rq_list,
                                          atomic_t *guilty)
{
       return drm_sched_entity_init(entity, priority, sched_list, num_sched_list, guilty);
}
#endif /* __LOONGGPU_SCHEDULER_HELPER_H__ */
