#include "loonggpu.h"
#include "loonggpu_ring.h"

static int loonggpu_queue_mapper_init(struct loonggpu_queue_mapper *mapper,
				    int hw_ip)
{
	if (!mapper)
		return -EINVAL;

	if (hw_ip > LOONGGPU_MAX_IP_NUM)
		return -EINVAL;

	mapper->hw_ip = hw_ip;
	mutex_init(&mapper->lock);

	memset(mapper->queue_map, 0, sizeof(mapper->queue_map));

	return 0;
}

static struct loonggpu_ring *loonggpu_get_cached_map(struct loonggpu_queue_mapper *mapper,
					  int ring)
{
	return mapper->queue_map[ring];
}

static int loonggpu_update_cached_map(struct loonggpu_queue_mapper *mapper,
			     int ring, struct loonggpu_ring *pring)
{
	if (WARN_ON(mapper->queue_map[ring])) {
		DRM_ERROR("Un-expected ring re-map\n");
		return -EINVAL;
	}

	mapper->queue_map[ring] = pring;

	return 0;
}

static int loonggpu_identity_map(struct loonggpu_device *adev,
			       struct loonggpu_queue_mapper *mapper,
			       u32 ring,
			       struct loonggpu_ring **out_ring)
{
	switch (mapper->hw_ip) {
	case LOONGGPU_HW_IP_GFX:
		*out_ring = &adev->gfx.gfx_ring[ring];
		break;
	case LOONGGPU_HW_IP_DMA:
		*out_ring = &adev->xdma.instance[ring].ring;
		break;
	case LOONGGPU_HW_IP_BPIPE:
		*out_ring = &adev->bpipe.ring;
		break;
	case LOONGGPU_HW_IP_EPIPE:
		*out_ring = &adev->epipe.ring;
		break;
	case LOONGGPU_HW_IP_DPIPE:
		*out_ring = &adev->dpipe.ring;
		break;
	default:
		*out_ring = NULL;
		DRM_ERROR("unknown HW IP type: %d\n", mapper->hw_ip);
		return -EINVAL;
	}

	return loonggpu_update_cached_map(mapper, ring, *out_ring);
}

static enum loonggpu_ring_type loonggpu_hw_ip_to_ring_type(int hw_ip)
{
	switch (hw_ip) {
	case LOONGGPU_HW_IP_GFX:
		return LOONGGPU_RING_TYPE_GFX;
	case LOONGGPU_HW_IP_DMA:
		return LOONGGPU_RING_TYPE_XDMA;
	case LOONGGPU_HW_IP_BPIPE:
		return LOONGGPU_RING_TYPE_BPIPE;
	case LOONGGPU_HW_IP_EPIPE:
		return LOONGGPU_RING_TYPE_EPIPE;
	case LOONGGPU_HW_IP_DPIPE:
		return LOONGGPU_RING_TYPE_DPIPE;
	default:
		DRM_ERROR("Invalid HW IP specified %d\n", hw_ip);
		return -1;
	}
}

static int loonggpu_lru_map(struct loonggpu_device *adev,
			  struct loonggpu_queue_mapper *mapper,
			  u32 user_ring, bool lru_pipe_order,
			  struct loonggpu_ring **out_ring)
{
	int r, i, j;
	int ring_type = loonggpu_hw_ip_to_ring_type(mapper->hw_ip);
	int ring_blacklist[LOONGGPU_MAX_RINGS];
	struct loonggpu_ring *ring;

	/* 0 is a valid ring index, so initialize to -1 */
	memset(ring_blacklist, 0xff, sizeof(ring_blacklist));

	for (i = 0, j = 0; i < LOONGGPU_MAX_RINGS; i++) {
		ring = mapper->queue_map[i];
		if (ring)
			ring_blacklist[j++] = ring->idx;
	}

	r = loonggpu_ring_lru_get(adev, ring_type, ring_blacklist,
				j, lru_pipe_order, out_ring);
	if (r)
		return r;

	return loonggpu_update_cached_map(mapper, user_ring, *out_ring);
}

/**
 * loonggpu_queue_mgr_init - init an loonggpu_queue_mgr struct
 *
 * @adev: loonggpu_device pointer
 * @mgr: loonggpu_queue_mgr structure holding queue information
 *
 * Initialize the the selected @mgr (all asics).
 *
 * Returns 0 on success, error on failure.
 */
int loonggpu_queue_mgr_init(struct loonggpu_device *adev,
			  struct loonggpu_queue_mgr *mgr)
{
	int i, r;

	if (!adev || !mgr)
		return -EINVAL;

	memset(mgr, 0, sizeof(*mgr));

	for (i = 0; i < LOONGGPU_MAX_IP_NUM; ++i) {
		r = loonggpu_queue_mapper_init(&mgr->mapper[i], i);
		if (r)
			return r;
	}

	return 0;
}

/**
 * loonggpu_queue_mgr_fini - de-initialize an loonggpu_queue_mgr struct
 *
 * @adev: loonggpu_device pointer
 * @mgr: loonggpu_queue_mgr structure holding queue information
 *
 * De-initialize the the selected @mgr (all asics).
 *
 * Returns 0 on success, error on failure.
 */
int loonggpu_queue_mgr_fini(struct loonggpu_device *adev,
			  struct loonggpu_queue_mgr *mgr)
{
	return 0;
}

/**
 * loonggpu_queue_mgr_map - Map a userspace ring id to an loonggpu_ring
 *
 * @adev: loonggpu_device pointer
 * @mgr: loonggpu_queue_mgr structure holding queue information
 * @hw_ip: HW IP enum
 * @instance: HW instance
 * @ring: user ring id
 * @our_ring: pointer to mapped loonggpu_ring
 *
 * Map a userspace ring id to an appropriate kernel ring. Different
 * policies are configurable at a HW IP level.
 *
 * Returns 0 on success, error on failure.
 */
int loonggpu_queue_mgr_map(struct loonggpu_device *adev,
			 struct loonggpu_queue_mgr *mgr,
			 u32 hw_ip, u32 instance, u32 ring,
			 struct loonggpu_ring **out_ring)
{
	int r, ip_num_rings = 0;
	struct loonggpu_queue_mapper *mapper = &mgr->mapper[hw_ip];

	if (!adev || !mgr || !out_ring)
		return -EINVAL;

	if (hw_ip >= LOONGGPU_MAX_IP_NUM)
		return -EINVAL;

	if (ring >= LOONGGPU_MAX_RINGS)
		return -EINVAL;

	/* Right now all IPs have only one instance - multiple rings. */
	if (instance != 0) {
		DRM_DEBUG("invalid ip instance: %d\n", instance);
		return -EINVAL;
	}

	switch (hw_ip) {
	case LOONGGPU_HW_IP_GFX:
		ip_num_rings = adev->gfx.num_gfx_rings;
		break;
	case LOONGGPU_HW_IP_DMA:
		ip_num_rings = adev->xdma.num_instances;
		break;
	case LOONGGPU_HW_IP_BPIPE:
	case LOONGGPU_HW_IP_EPIPE:
	case LOONGGPU_HW_IP_DPIPE:
		ip_num_rings = 1;
		break;
	default:
		DRM_DEBUG("unknown ip type: %d\n", hw_ip);
		return -EINVAL;
	}

	if (ring >= ip_num_rings) {
		DRM_DEBUG("Ring index:%d exceeds maximum:%d for ip:%d\n",
			  ring, ip_num_rings, hw_ip);
		return -EINVAL;
	}

	mutex_lock(&mapper->lock);

	*out_ring = loonggpu_get_cached_map(mapper, ring);
	if (*out_ring) {
		/* cache hit */
		r = 0;
		goto out_unlock;
	}

	switch (mapper->hw_ip) {
	case LOONGGPU_HW_IP_GFX:
	case LOONGGPU_HW_IP_BPIPE:
	case LOONGGPU_HW_IP_EPIPE:
	case LOONGGPU_HW_IP_DPIPE:
		r = loonggpu_identity_map(adev, mapper, ring, out_ring);
		break;
	case LOONGGPU_HW_IP_DMA:
		r = loonggpu_lru_map(adev, mapper, ring, false, out_ring);
		break;
	default:
		*out_ring = NULL;
		r = -EINVAL;
		DRM_DEBUG("unknown HW IP type: %d\n", mapper->hw_ip);
	}

out_unlock:
	mutex_unlock(&mapper->lock);
	return r;
}
