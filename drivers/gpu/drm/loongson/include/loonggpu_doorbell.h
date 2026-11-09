#ifndef LOONGGPU_DOORBELL_H
#define LOONGGPU_DOORBELL_H

#define LOONGGPU_GEM_DOORBELL_ALIGN_SHIFT 8
#define LOONGGPU_GEM_DOORBELL_ALIGN_SIZE (PAGE_SIZE<<LOONGGPU_GEM_DOORBELL_ALIGN_SHIFT)

/*
 * GPU doorbell structures, functions & helpers
 */
struct loonggpu_doorbell {
	/* doorbell mmio */
	resource_size_t		base;
	resource_size_t		size;

	/* Number of doorbells reserved for loonggpu kernel driver */
	u32 num_kernel_doorbells;

	/* Kernel doorbells */
	struct loonggpu_bo *kernel_doorbells;

	/* For CPU access of doorbells */
	uint64_t *cpu_addr;

	/* Reserved doorbells for loonggpu */
	struct loonggpu_bo *rdb;
	uint64_t rdb_gpu_addr;
	uint64_t *rdb_cpu_addr;
	bool is_enabled;
	
};

/* Reserved doorbells for loonggpu (including multimedia).
 * KCD can use all the rest in the 2M doorbell bar.
 * For asic before vega10, doorbell is 32-bit, so the
 * index/offset is in dword. For vega10 and after, doorbell
 * can be 64-bit, so the index defined is in qword.
 */
struct loonggpu_doorbell_index {
	uint32_t kiq;
	uint32_t gfx_ring0;
	uint32_t sdma_engine[8];
	uint32_t ih;
	uint32_t first_non_cp;
	uint32_t last_non_cp;
	uint32_t max_assignment;
	/* Per engine SDMA doorbell size in dword */
	uint32_t sdma_doorbell_range;
};

enum LOONGGPU_DOORBELL_ASSIGNMENT {
	LOONGGPU_DOORBELL_KIQ                     = 0x000,
	LOONGGPU_DOORBELL_GFX_RING0               = 0x020,
	LOONGGPU_DOORBELL_sDMA_ENGINE0            = 0x1E0,
	LOONGGPU_DOORBELL_MAX_ASSIGNMENT          = 0x3FF,
	LOONGGPU_DOORBELL_INVALID                 = 0xFFFF
};

u64 loonggpu_mm_rdoorbell64(struct loonggpu_device *adev, u32 index);
void loonggpu_mm_wdoorbell64(struct loonggpu_device *adev, u32 index, u64 v);

/*
 * GPU doorbell aperture helpers function.
 */
int loonggpu_doorbell_enable(struct loonggpu_device *adev, bool clear);
int loonggpu_doorbell_disable(struct loonggpu_device *adev);
int loonggpu_doorbell_init(struct loonggpu_device *adev);
void loonggpu_doorbell_fini(struct loonggpu_device *adev);
int loonggpu_doorbell_create_kernel_doorbells(struct loonggpu_device *adev);
uint32_t loonggpu_doorbell_index_on_bar(struct loonggpu_device *adev,
				       struct loonggpu_bo *db_bo,
				       uint32_t doorbell_index);

#define RDOORBELL64(index) loonggpu_mm_rdoorbell64(adev, (index))
#define WDOORBELL64(index, v) loonggpu_mm_wdoorbell64(adev, (index), (v))

#endif
