#ifndef __LOONGGPU_HELPER_H__
#define __LOONGGPU_HELPER_H__

#include "loonggpu_drm.h"
#include <linux/aperture.h>
#include <linux/console.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/mmu_context.h>
#include <linux/kthread.h>
#include <linux/cpufreq.h>
#include <linux/dma-fence-array.h>
#include <linux/pwm.h>
#include <linux/vgaarb.h>
#include <drm/drm_bridge.h>
#include <drm/drm_cache.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_vblank.h>
#include <drm/gpu_scheduler.h>
#include <drm/clients/drm_fbdev_helper.h>
#include <drm/ttm/ttm_bo.h>
#include "loonggpu.h"
#include "loonggpu_dc_helper.h"

/*
 * drm_sched_hw_job_reset() was replaced with drm_seched_stop.
 *
 * fix case 7.
 */
static inline void lg_drm_sched_stop(struct drm_gpu_scheduler *sched,
				     struct drm_sched_job *bad)
{
	drm_sched_stop(sched, bad);
}

/*
 * Use drm_sched_job_cleanup() .
 *
 */
static inline void lg_drm_sched_job_cleanup(struct drm_sched_job *job)
{
	drm_sched_job_cleanup(job);
}

/*
 * If the function drm_fb_helper_fill_info() is not present,
 * implement it.
 *
 * fix case 22.
 */
static inline void lg_drm_fb_helper_fill_info(struct fb_info *info,
					      struct drm_fb_helper *fb_helper,
					      struct drm_fb_helper_surface_size *sizes)
{
	drm_fb_helper_fill_info(info, fb_helper, sizes);
}

/*
 * fix case 1/10/13
 */
static inline int lg_loonggpu_ttm_bo_device_init(struct loonggpu_device *adev,
					      struct ttm_device_funcs *driver)
{
	return ttm_device_init(&adev->mman.bdev,
				driver, adev->dev,
				adev->ddev->anon_inode->i_mapping,
				adev->ddev->vma_offset_manager,
				(adev->need_swiotlb ?
				TTM_ALLOCATION_POOL_USE_DMA_ALLOC : 0) |
				(dma_addressing_limited(adev->dev) ?
				TTM_ALLOCATION_POOL_USE_DMA32 : 0));
}

/*
 * fix case 25.
 */
static inline int lg_ttm_eu_reserve_buffers(struct ww_acquire_ctx *ticket,
					    struct list_head *list, bool intr,
					    struct list_head *dups, bool del_lru)
{
	return ttm_eu_reserve_buffers(ticket, list, intr, dups);
}

/*
 * fix case 26
 */
static inline int lg_drm_syncobj_find_fence(struct drm_file *file_private,
					    u32 handle, u64 point, u64 flags,
					    struct dma_fence **fence)
{
	return drm_syncobj_find_fence(file_private, handle, point, flags, fence);
}

#define lg_ttm_bo_move_define_args struct ttm_buffer_object *bo, struct ttm_lru_bulk_move *bulk
#define lg_ttm_bo_move_pass_args(buffer_obj, bulk) buffer_obj, bulk

static inline void lg_ttm_bo_move_to_lru_tail(lg_ttm_bo_move_define_args)
{
	ttm_bo_move_to_lru_tail(bo);
}

/*
 * fix case 35
 */
static inline int lg_drm_hdmi_avi_infoframe_from_display_mode(
						struct hdmi_avi_infoframe *frame,
						struct drm_connector *connector,
						const struct drm_display_mode *mode)
{
	return drm_hdmi_avi_infoframe_from_display_mode(frame, connector, mode);
}

/*
 * fix case 5
 */
lg_dma_resv_t *lg_loonggpu_gem_prime_res_obj(struct drm_gem_object *obj);

#if defined(LG_DRM_DRIVER_HAS_GEM_PRIME_RES_OBJ)
#define LG_DRM_DRIVER_GEM_PRIME_RES_OBJ_CALLBACK \
		.gem_prime_res_obj = lg_loonggpu_gem_prime_res_obj,
#else
#define LG_DRM_DRIVER_GEM_PRIME_RES_OBJ_CALLBACK
#endif

/*
 * fix case 24
 */
static inline void lg_ttm_validate_buffer_set_shared(struct ttm_validate_buffer *valid_buf, unsigned int num_shared)
{
	valid_buf->num_shared = num_shared;
}

static inline void lg_ttm_validate_buffer_copy_shared(struct ttm_validate_buffer *dst, struct ttm_validate_buffer *src)
{
	dst->num_shared = src->num_shared;
}

/*
 * fix case 34
 */
static inline bool lg_drm_crtc_state_async_flip(struct drm_crtc_state *state)
{
	return state->async_flip;
}

/*
 * fix case 32
 */
static inline bool lg_drm_need_swiotlb(int dma_bits)
{
	return drm_need_swiotlb(dma_bits);
}

/*
 * fix case 4/28
 */
static inline struct dma_buf *lg_drm_gem_prime_export(struct drm_gem_object *gobj,
						      int flags)
{
	return drm_gem_prime_export(gobj, flags);
}

struct dma_buf *lg_loonggpu_gem_prime_export(
					  struct drm_gem_object *gobj,
					  int flags);

static inline void lg_loongson_screen_state(bool state)
{
}

#define lg_drm_connector_for_each_possible_encoder(connector, encoder, __i)\
	drm_connector_for_each_possible_encoder(connector, encoder)

static inline struct i2c_client *lg_i2c_new_device(struct i2c_adapter *adap,
						   struct i2c_board_info const *info)
{
	return i2c_new_client_device(adap, info);
}

static inline void lg_drm_gem_object_put(struct drm_gem_object *gobj)
{
	drm_gem_object_put(gobj);
}

static inline int lg_drm_bridge_attach(struct drm_encoder *encoder,
					struct drm_bridge *bridge,
					struct drm_bridge *previous,
					int flags)
{
	return drm_bridge_attach(encoder, bridge, previous, (enum drm_bridge_attach_flags)flags);
}

#define lg_bridge_phy_attach_args struct drm_bridge *bridge, struct drm_encoder *encoder, enum drm_bridge_attach_flags flags

#define lg_get_scanout_position_setting(func) .get_scanout_position = func

static inline int lg_drm_fb_helper_init(struct drm_device *dev,
					struct drm_fb_helper *fb_helper,
					int max_conn_count)
{
	return drm_fb_helper_init(dev, fb_helper);
}

static inline struct rw_semaphore *lg_mm_struct_get_sem(struct mm_struct *mm)
{
	return &mm->mmap_lock;
}

static inline struct sg_table *lg_drm_prime_pages_to_sg(struct drm_device *dev,
                                        struct page **pages, unsigned int nr_pages)
{
	return drm_prime_pages_to_sg(dev, pages, nr_pages);
}

static inline int lg_ttm_fbdev_mmap(struct vm_area_struct *vma, struct ttm_buffer_object *bo)
{
	if (vma->vm_pgoff != 0)
		return -EACCES;

        return ttm_bo_mmap_obj(vma, bo);
}

static inline int lg_atomic_read_drm_open_count(struct drm_device *dev)
{
	return atomic_read(&dev->open_count);
}

#define lg_loonggpu_job_timedout_ret enum drm_gpu_sched_stat

#define LG_DRM_GPU_SCHED_STAT_NOMINAL DRM_GPU_SCHED_STAT_RESET

static inline void lg_drm_sched_entity_push_job(struct drm_sched_job *sched_job,
					struct drm_sched_entity *entity)
{
	drm_sched_entity_push_job(sched_job);
}

static inline void lg_drm_sched_job_arm(struct drm_sched_job *job)
{
	drm_sched_job_arm(job);
}

#define lg_sched_ops_set_prepare .prepare_job = loonggpu_job_dependency,

#define LG_DMA_RESV_USAGE_BOOKKEEP	DMA_RESV_USAGE_BOOKKEEP
#define LG_DMA_RESV_USAGE_WRITE		DMA_RESV_USAGE_WRITE
#define LG_DMA_RESV_USAGE_KERNEL	DMA_RESV_USAGE_KERNEL
#define LG_DMA_RESV_USAGE_READ		DMA_RESV_USAGE_READ

static inline int lg_ttm_bo_lock_delayed_workqueue(struct loonggpu_device *adev)
{
	return 0;
}

static inline void lg_ttm_bo_unlock_delayed_workqueue(struct loonggpu_device *adev, int sched)
{
}

static inline struct fb_info *lg_drm_fb_helper_alloc_info(struct drm_fb_helper *helper)
{
	return helper->info;
}

static inline void lg_drm_fb_helper_unregister_info(struct drm_fb_helper *helper)
{
	drm_fb_helper_unregister_info(helper);
}

static inline void lg_set_info_apertures(struct fb_info *info, struct loonggpu_device *adev)
{
}

static inline void lg_drm_fb_helper_prepare(struct drm_device *dev, struct drm_fb_helper *helper,
						unsigned int bpp,
						const struct drm_fb_helper_funcs *funcs)
{
	drm_fb_helper_prepare(dev, helper, bpp, funcs);
}

static inline void lg_drm_fb_helper_initial_config(struct drm_fb_helper *helper, unsigned int bpp)
{
	drm_fb_helper_initial_config(helper);
}

static inline void lg_pwm_free(struct pwm_device *pwm)
{
	pwm_put(pwm);
}

extern int turn_on_lvds(void);

extern int turn_off_lvds(void);

static inline void lg_turn_on_lvds(void)
{
#if 0
	turn_on_lvds();
#endif
}

static inline void lg_turn_off_lvds(void)
{
#if 0
	turn_off_lvds();
#endif
}

#define lg_get_bo_mem_type(bo)		bo->tbo.resource->mem_type
#define lg_get_bo_node_start(bo)	to_ttm_range_mgr_node(bo->tbo.resource)->mm_nodes->start

typedef struct ttm_global lg_ttm_global_t;

static inline void lg_spin_lock_glob_unlock(lg_ttm_global_t *glob,
					  struct loonggpu_device *adev)
{
	spin_unlock(&adev->mman.bdev.lru_lock);
}

static inline long lg_get_user_pages(uint64_t userptr, unsigned num_pages,
				     unsigned int flags, struct page **pages,
				     struct vm_area_struct **vmas)
{
	return get_user_pages(userptr, num_pages, flags, pages);
}

static inline int lg_drm_sched_init(struct loonggpu_ring *ring,
				const struct drm_sched_backend_ops *ops,
				unsigned num_hw_submission,
				unsigned hang_limit,
				long timeout, struct loonggpu_device *adev)
{
	const struct drm_sched_init_args args = {
		.ops = ops,
		.submit_wq = NULL,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = num_hw_submission,
		.hang_limit = hang_limit,
		.timeout = timeout,
		.timeout_wq = NULL,
		.score = NULL,
		.name = ring->name,
		.dev = adev->dev,
	};
	return drm_sched_init(&ring->sched, &args);
}

static inline int lg_drm_irq_install(struct drm_device *ddev, struct pci_dev *pdev,
					int *irq, int vec, irq_handler_t handler)
{
	int r;
	r = pci_irq_vector(pdev, vec);
	if (r < 0)
		return r;
	*irq = r;
	/* PCI devices require shared interrupts. */
	r = request_irq(*irq, handler, IRQF_SHARED,
			ddev->driver->name, ddev);
	return r;
}


static inline void lg_drm_irq_uninstall(struct drm_device *ddev, int irq)
{
	free_irq(irq, ddev);
}

static inline lg_dma_resv_t *lg_tbo_to_resv(struct ttm_buffer_object *tbo)
{
	return tbo->base.resv;
}

static inline lg_dma_resv_t *lg_tbo_to_ttm_resv(struct ttm_buffer_object *tbo)
{
	return &tbo->base._resv;
}

static inline uint64_t lg_mem_to_size(lg_ttm_mem_t *mem)
{
#if defined(LG_TTM_RES_HAS_NUM_PAGES)
	return mem->num_pages << PAGE_SHIFT;
#else
	return mem->size;
#endif
}

static inline uint64_t lg_mem_to_num_pages(lg_ttm_mem_t *mem)
{
#if defined(LG_TTM_RES_HAS_NUM_PAGES)
	return mem->num_pages;
#else
	return mem->size >> PAGE_SHIFT;
#endif
}

static inline uint64_t lg_tbo_to_size(struct ttm_buffer_object *tbo)
{
	return tbo->base.size;
}

static inline int lg_ttm_bo_move_ttm(struct ttm_buffer_object *tbo,
			struct ttm_operation_ctx *ctx, lg_ttm_mem_t *new_mem)
{
	return 0;
}

static inline int lg_ttm_populate_and_map_pages(struct loonggpu_device *adev,
						struct ttm_tt *ttm,
						struct ttm_operation_ctx *ctx)
{
	int ret, i;

	ret = ttm_pool_alloc(&adev->mman.bdev.pool, ttm, ctx);
	if (ret)
		return ret;

	for (i = 0; i < ttm->num_pages; ++i)
		ttm->pages[i]->mapping = adev->mman.bdev.dev_mapping;
	return 0;
}

static inline void lg_ttm_unmap_and_unpopulate_pages(struct loonggpu_device *adev,
						struct ttm_tt *ttm)
{
	int i = 0;

	for (i = 0; i < ttm->num_pages; ++i)
		ttm->pages[i]->mapping = NULL;

	ttm_pool_free(&adev->mman.bdev.pool, ttm);
}

static inline dma_addr_t *lg_tbo_to_dma_address(struct ttm_buffer_object *tbo)
{
	return tbo->ttm->dma_address;
}

static bool loonggpu_drm_sched_dependency_optimized(struct dma_fence* fence,
					struct drm_sched_entity *entity)
{
	struct drm_gpu_scheduler *sched = entity->rq->sched;
	struct drm_sched_fence *s_fence;

	if (!fence || dma_fence_is_signaled(fence))
		return false;
	if (fence->context == entity->fence_context)
		return true;
	s_fence = to_drm_sched_fence(fence);
	if (s_fence && s_fence->sched == sched)
		return true;

	return false;
}

static inline bool lg_drm_sched_dependency_optimized(struct dma_fence *fence,
						struct drm_sched_entity *s_entity)
{
	return loonggpu_drm_sched_dependency_optimized(fence, s_entity);
}

static inline int lg_pwm_apply_state(struct pwm_device *pwm,
				     struct pwm_state *state)
{
	return pwm_apply_might_sleep(pwm, state);
}

static struct pwm_lookup loongson_pwm_lookup;

static void init_loongson_pwm_lookup(int pwm_id, const char *dev_name)
{
	char provider[20];

	sprintf(provider, "LOON0006:0%d", pwm_id);

	loongson_pwm_lookup.provider = provider;
	loongson_pwm_lookup.index = 0;
	loongson_pwm_lookup.dev_id = dev_name;
	loongson_pwm_lookup.con_id = NULL;
	loongson_pwm_lookup.period = 0;
	loongson_pwm_lookup.polarity = PWM_POLARITY_NORMAL;
	loongson_pwm_lookup.module = "pwm-loongson";
}

static inline void lg_pwm_add_table(struct pwm_lookup *table, size_t num)
{
	pwm_add_table(table, num);
}

static inline void lg_pwm_remove_table(void)
{
	/* make sure pwm_lookup structure is valid */
	if (loongson_pwm_lookup.provider) {
		pwm_remove_table(&loongson_pwm_lookup, 1);
		/* clear pwm_lookup structure after remove it */
		memset(&loongson_pwm_lookup, 0, sizeof(struct pwm_lookup));
	}
}

static inline struct pwm_device *lg_pwm_request(struct device *dev,
						const char *pwm_name,
						int pwm, const char *lable)
{
	init_loongson_pwm_lookup(pwm, dev_name(dev));
	lg_pwm_add_table(&loongson_pwm_lookup, 1);
	return pwm_get(dev, NULL);
}

static inline unsigned long lg_get_man_size(lg_ttm_manager_t *man)
{
	return man->size;
}

static inline unsigned long lg_get_tbo_res_size(struct ttm_buffer_object *bo)
{
	return PFN_UP(bo->resource->size) << PAGE_SHIFT;
}

#define lg_gbo_to_gem_obj(bo) bo->tbo.base

static inline void lg_drm_syncobj_add_point(struct drm_syncobj *syncobj,
						void *chain,
						struct dma_fence *fence,
						uint64_t point)
{

	drm_syncobj_add_point(syncobj, (struct dma_fence_chain *)chain, fence, point);
}

static inline bool lg_ring_sched_thread_avai(struct loonggpu_ring *ring)
{
	if (!ring)
		return false;
	if (!drm_sched_wqueue_ready(&ring->sched))
		return false;
	return true;
}

static inline void lg_ring_sched_thread_park(struct loonggpu_ring *ring)
{
	drm_sched_wqueue_stop(&ring->sched);
}

static inline void lg_ring_sched_thread_unpark(struct loonggpu_ring *ring)
{
	drm_sched_wqueue_start(&ring->sched);
}

static inline uint64_t lg_drm_file_get_client_id(struct drm_file *filp)
{
	return filp->client_id;
}

static inline int lg_drm_sched_job_init(struct drm_sched_job *job,
					struct drm_sched_entity *entity,
					u32 credits, void *owner, uint64_t drm_client_id)
{
	return drm_sched_job_init(job, entity, credits, owner, drm_client_id);
}

#define lg_drm_driver_set_lastclose

#define lg_access_ok(type, addr, size) access_ok(addr, size)

#define lg_sysfs_emit sysfs_emit

static inline int lg_ttm_bo_wait(struct ttm_buffer_object *tbo,
				bool interruptible, bool no_wait)
{
	struct ttm_operation_ctx ctx2 = { interruptible, no_wait };
	return ttm_bo_wait_ctx(tbo, &ctx2);
}

static inline void lg_use_mm(struct mm_struct *mm)
{
	kthread_use_mm(mm);
}

static inline void lg_unuse_mm(struct mm_struct *mm)
{
	kthread_unuse_mm(mm);
}

static inline const struct edid *lg_drm_do_get_edid(struct drm_connector *connector,
					int (*get_edid_block)(void *data, u8 *buf,
							      unsigned int block, size_t len),
					void *context)
{
	const struct drm_edid *edid;
	edid = drm_edid_read_custom(connector, get_edid_block, context);
	return drm_edid_raw(edid);
}

static inline int lg_del_timer(struct timer_list *timer)
{
	return timer_delete(timer);
}

static inline int lg_del_timer_sync(struct timer_list *timer)
{
	return timer_delete_sync(timer);
}

#define lg_from_timer(var, callback_timer, timer_fieldname) \
		timer_container_of(var, callback_timer, timer_fieldname);

static inline const struct drm_format_info *lg_drm_get_format_info(struct drm_device *dev,
						const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_get_format_info(dev, mode_cmd->pixel_format, mode_cmd->modifier[0]);
}

static inline void lg_drm_helper_mode_fill_fb_struct(struct drm_device *dev,
						struct drm_framebuffer *fb,
						const struct drm_format_info *info,
						const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_helper_mode_fill_fb_struct(dev, fb, info, mode_cmd);
}

static inline int lg_ida_simple_get(struct ida *ida, unsigned int bits)
{
#if defined(ida_simple_get)
	return ida_simple_get(ida, 1U << (bits - 1), 1U << bits, GFP_KERNEL);
#else
	return ida_alloc_range(ida, 1U << (bits - 1), (1U << bits) - 1, GFP_KERNEL);
#endif
}

static inline void lg_ida_simple_remove(struct ida *ida, unsigned int pasid)
{
#if defined(ida_simple_remove)
	return ida_simple_remove(ida, pasid);
#else
	return ida_free(ida, pasid);
#endif
}

static inline int lg_pci_resize_resource(struct pci_dev *dev, int i, int size,
					int exclude_bars)
{
	return pci_resize_resource(dev, i, size, exclude_bars);
}

static inline struct dma_fence_array *lg_dma_fence_array_create(int num_fences,
						struct dma_fence **fences,
						u64 context, unsigned int seqno,
						bool signal_on_any)
{
	struct dma_fence_array *array;

	array = dma_fence_array_create(num_fences, fences, context, seqno);

	if (!array)
		return NULL;
	/* HACK: emulate removed signal_on_any by poking num_pending. */
	if (signal_on_any)
		atomic_set(&array->num_pending, 1);

	return array;
}

#endif /* __LOONGGPU_HELPER_H__ */
