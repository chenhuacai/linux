#include "loonggpu_drm.h"
#include "loonggpu.h"

#define LOONGGPU_BENCHMARK_ITERATIONS 1024
#define LOONGGPU_BENCHMARK_COMMON_MODES_N 17

static int loonggpu_benchmark_do_move(struct loonggpu_device *adev, unsigned size,
				    uint64_t saddr, uint64_t daddr, int n)
{
	unsigned long start_jiffies;
	unsigned long end_jiffies;
	struct dma_fence *fence = NULL;
	int i, r;

	start_jiffies = jiffies;
	for (i = 0; i < n; i++) {
		struct loonggpu_ring *ring = adev->mman.buffer_funcs_ring;
		r = loonggpu_copy_buffer(ring, saddr, daddr, size, NULL, &fence,
				       false, false);
		if (r)
			goto exit_do_move;
		r = dma_fence_wait(fence, false);
		if (r)
			goto exit_do_move;
		dma_fence_put(fence);
	}
	end_jiffies = jiffies;
	r = jiffies_to_msecs(end_jiffies - start_jiffies);

exit_do_move:
	if (fence)
		dma_fence_put(fence);
	return r;
}


static void loonggpu_benchmark_log_results(int n, unsigned size,
					 unsigned int time,
					 unsigned sdomain, unsigned ddomain,
					 char *kind)
{
	unsigned int throughput = (n * (size >> 10)) / time;
	DRM_INFO("loonggpu: %s %u bo moves of %u kB from"
		 " %d to %d in %u ms, throughput: %u Mb/s or %u MB/s\n",
		 kind, n, size >> 10, sdomain, ddomain, time,
		 throughput * 8, throughput);
}

static void loonggpu_benchmark_move(struct loonggpu_device *adev, unsigned size,
				  unsigned sdomain, unsigned ddomain)
{
	struct loonggpu_bo *dobj = NULL;
	struct loonggpu_bo *sobj = NULL;
	struct loonggpu_bo_param bp;
	uint64_t saddr, daddr;
	int r, n;
	int time;

	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = PAGE_SIZE;
	bp.domain = sdomain;
	bp.flags = 0;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;
	n = LOONGGPU_BENCHMARK_ITERATIONS;
	r = loonggpu_bo_create(adev, &bp, &sobj);
	if (r) {
		goto out_cleanup;
	}
	r = loonggpu_bo_reserve(sobj, false);
	if (unlikely(r != 0))
		goto out_cleanup;
	r = loonggpu_bo_pin(sobj, sdomain);
	if (r) {
		loonggpu_bo_unreserve(sobj);
		goto out_cleanup;
	}
	r = loonggpu_ttm_alloc_gart(&sobj->tbo);
	loonggpu_bo_unreserve(sobj);
	if (r) {
		goto out_cleanup;
	}
	saddr = loonggpu_bo_gpu_offset(sobj);
	bp.domain = ddomain;
	r = loonggpu_bo_create(adev, &bp, &dobj);
	if (r) {
		goto out_cleanup;
	}
	r = loonggpu_bo_reserve(dobj, false);
	if (unlikely(r != 0))
		goto out_cleanup;
	r = loonggpu_bo_pin(dobj, ddomain);
	if (r) {
		loonggpu_bo_unreserve(sobj);
		goto out_cleanup;
	}
	r = loonggpu_ttm_alloc_gart(&dobj->tbo);
	loonggpu_bo_unreserve(dobj);
	if (r) {
		goto out_cleanup;
	}
	daddr = loonggpu_bo_gpu_offset(dobj);

	if (adev->mman.buffer_funcs) {
		time = loonggpu_benchmark_do_move(adev, size, saddr, daddr, n);
		if (time < 0)
			goto out_cleanup;
		if (time > 0)
			loonggpu_benchmark_log_results(n, size, time,
						     sdomain, ddomain, "dma");
	}

out_cleanup:
	/* Check error value now. The value can be overwritten when clean up.*/
	if (r) {
		DRM_ERROR("Error while benchmarking BO move.\n");
	}

	if (sobj) {
		r = loonggpu_bo_reserve(sobj, true);
		if (likely(r == 0)) {
			loonggpu_bo_unpin(sobj);
			loonggpu_bo_unreserve(sobj);
		}
		loonggpu_bo_unref(&sobj);
	}
	if (dobj) {
		r = loonggpu_bo_reserve(dobj, true);
		if (likely(r == 0)) {
			loonggpu_bo_unpin(dobj);
			loonggpu_bo_unreserve(dobj);
		}
		loonggpu_bo_unref(&dobj);
	}
}

void loonggpu_benchmark(struct loonggpu_device *adev, int test_number)
{
	int i;
	static const int common_modes[LOONGGPU_BENCHMARK_COMMON_MODES_N] = {
		640 * 480 * 4,
		720 * 480 * 4,
		800 * 600 * 4,
		848 * 480 * 4,
		1024 * 768 * 4,
		1152 * 768 * 4,
		1280 * 720 * 4,
		1280 * 800 * 4,
		1280 * 854 * 4,
		1280 * 960 * 4,
		1280 * 1024 * 4,
		1440 * 900 * 4,
		1400 * 1050 * 4,
		1680 * 1050 * 4,
		1600 * 1200 * 4,
		1920 * 1080 * 4,
		1920 * 1200 * 4
	};

	switch (test_number) {
	case 1:
		/* simple test, VRAM to GTT and GTT to VRAM */
		loonggpu_benchmark_move(adev, 1024*1024, LOONGGPU_GEM_DOMAIN_GTT,
				      LOONGGPU_GEM_DOMAIN_VRAM);
		loonggpu_benchmark_move(adev, 1024*1024, LOONGGPU_GEM_DOMAIN_VRAM,
				      LOONGGPU_GEM_DOMAIN_GTT);
		break;
	case 2:
		/* simple test, VRAM to VRAM */
		loonggpu_benchmark_move(adev, 1024*1024, LOONGGPU_GEM_DOMAIN_VRAM,
				      LOONGGPU_GEM_DOMAIN_VRAM);
		break;
	case 3:
		/* GTT to VRAM, buffer size sweep, powers of 2 */
		for (i = 1; i <= 16384; i <<= 1)
			loonggpu_benchmark_move(adev, i * LOONGGPU_GPU_PAGE_SIZE,
					      LOONGGPU_GEM_DOMAIN_GTT,
					      LOONGGPU_GEM_DOMAIN_VRAM);
		break;
	case 4:
		/* VRAM to GTT, buffer size sweep, powers of 2 */
		for (i = 1; i <= 16384; i <<= 1)
			loonggpu_benchmark_move(adev, i * LOONGGPU_GPU_PAGE_SIZE,
					      LOONGGPU_GEM_DOMAIN_VRAM,
					      LOONGGPU_GEM_DOMAIN_GTT);
		break;
	case 5:
		/* VRAM to VRAM, buffer size sweep, powers of 2 */
		for (i = 1; i <= 16384; i <<= 1)
			loonggpu_benchmark_move(adev, i * LOONGGPU_GPU_PAGE_SIZE,
					      LOONGGPU_GEM_DOMAIN_VRAM,
					      LOONGGPU_GEM_DOMAIN_VRAM);
		break;
	case 6:
		/* GTT to VRAM, buffer size sweep, common modes */
		for (i = 0; i < LOONGGPU_BENCHMARK_COMMON_MODES_N; i++)
			loonggpu_benchmark_move(adev, common_modes[i],
					      LOONGGPU_GEM_DOMAIN_GTT,
					      LOONGGPU_GEM_DOMAIN_VRAM);
		break;
	case 7:
		/* VRAM to GTT, buffer size sweep, common modes */
		for (i = 0; i < LOONGGPU_BENCHMARK_COMMON_MODES_N; i++)
			loonggpu_benchmark_move(adev, common_modes[i],
					      LOONGGPU_GEM_DOMAIN_VRAM,
					      LOONGGPU_GEM_DOMAIN_GTT);
		break;
	case 8:
		/* VRAM to VRAM, buffer size sweep, common modes */
		for (i = 0; i < LOONGGPU_BENCHMARK_COMMON_MODES_N; i++)
			loonggpu_benchmark_move(adev, common_modes[i],
					      LOONGGPU_GEM_DOMAIN_VRAM,
					      LOONGGPU_GEM_DOMAIN_VRAM);
		break;

	default:
		DRM_ERROR("Unknown benchmark\n");
	}
}
