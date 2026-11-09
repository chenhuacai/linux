#include "loonggpu.h"

/**
 * loonggpu_mm_rdoorbell64 - read a doorbell Qword
 *
 * @adev: loonggpu_device pointer
 * @index: doorbell index
 *
 * Returns the value in the doorbell aperture at the
 * requested doorbell index (VEGA10+).
 */
u64 loonggpu_mm_rdoorbell64(struct loonggpu_device *adev, u32 index)
{
	if (loonggpu_device_skip_hw_access(adev))
		return 0;

	if (index < adev->doorbell.num_kernel_doorbells)
		return atomic64_read((atomic64_t *)(adev->doorbell.cpu_addr + index));

	DRM_ERROR("reading beyond doorbell aperture: 0x%08x!\n", index);
	return 0;
}

/**
 * loonggpu_mm_wdoorbell64 - write a doorbell Qword
 *
 * @adev: loonggpu_device pointer
 * @index: doorbell index
 * @v: value to write
 *
 * Writes @v to the doorbell aperture at the
 * requested doorbell index (VEGA10+).
 */
void loonggpu_mm_wdoorbell64(struct loonggpu_device *adev, u32 index, u64 v)
{
	if (loonggpu_device_skip_hw_access(adev))
		return;

	if (index < adev->doorbell.num_kernel_doorbells)
		atomic64_set((atomic64_t *)(adev->doorbell.cpu_addr + index), v);
	else
		DRM_ERROR("writing beyond doorbell aperture: 0x%08x!\n", index);
}

/**
 * loonggpu_doorbell_index_on_bar - Find doorbell's absolute offset in BAR
 *
 * @adev: loonggpu_device pointer
 * @db_bo: doorbell object's bo
 * @db_index: doorbell relative index in this doorbell object
 *
 * returns doorbell's absolute index in BAR
 */
uint32_t loonggpu_doorbell_index_on_bar(struct loonggpu_device *adev,
				       struct loonggpu_bo *db_bo,
				       uint32_t doorbell_index)
{
	int db_bo_offset;

	db_bo_offset = loonggpu_bo_gpu_offset(db_bo) - adev->doorbell.base;
	/* doorbell index is 64 bit and doorbell's size is 64-bit */
	return db_bo_offset / sizeof(u64) + doorbell_index;
}

/**
 * loonggpu_doorbell_create_kernel_doorbells - Create kernel doorbells for graphics
 *
 * @adev: loonggpu_device pointer
 *
 * Creates doorbells for graphics driver usages.
 * returns 0 on success, error otherwise.
 */
int loonggpu_doorbell_create_kernel_doorbells(struct loonggpu_device *adev)
{
	int r;
	int size;

	/* SI HW does not have doorbells, skip allocation */
	if (adev->doorbell.num_kernel_doorbells == 0)
		return 0;
	
	r = loonggpu_bo_create_kernel(adev,
				    adev->doorbell.size,
				    LOONGGPU_GEM_DOORBELL_ALIGN_SIZE,
				    LOONGGPU_GEM_DOMAIN_VRAM,
				    &adev->doorbell.rdb,
				    &adev->doorbell.rdb_gpu_addr,
				    (void **)&adev->doorbell.rdb_cpu_addr);
	if (r) {
		DRM_ERROR("Failed to allocate kernel reserved_doorbells, err=%d\n", r);
		return r;
	}

	/* Reserve first num_kernel_doorbells (page-aligned) for kernel ops */
	size = ALIGN(adev->doorbell.num_kernel_doorbells * sizeof(u64), PAGE_SIZE);

	r = loonggpu_bo_create_kernel(adev,
				    size,
				    PAGE_SIZE,
				    LOONGGPU_GEM_DOMAIN_DOORBELL,
				    &adev->doorbell.kernel_doorbells,
				    NULL,
				    (void **)&adev->doorbell.cpu_addr);
	if (r) {
		DRM_ERROR("Failed to allocate kernel doorbells, err=%d\n", r);
		return r;
	}

	adev->doorbell.num_kernel_doorbells = size / sizeof(u64);
	return 0;
}

int loonggpu_doorbell_enable(struct loonggpu_device *adev, bool clear)
{
	if (adev->family_type < CHIP_LG210 ||
	    adev->family_type == CHIP_LG211)
		return 0;

	if (clear)
		memset(adev->doorbell.rdb_cpu_addr, 0 , adev->doorbell.size);

	/* Enable doorbell */
	if (adev->rmmio_sc)
		writel((uint32_t)(adev->doorbell.rdb_gpu_addr >> 22),
			((void __iomem *)adev->rmmio_sc) + LG2XX_SC132_DOORBELL_REG);

	loonggpu_cmd_exec(adev, GSCMD(LG2XX_ICMD32_MOP_DOORBELL, LG2XX_ICMD32_SOP_DB_ZEN),
		lower_32_bits(adev->doorbell.rdb_gpu_addr),
		upper_32_bits(adev->doorbell.rdb_gpu_addr));

	loonggpu_cmd_exec(adev, GSCMD(LG2XX_ICMD32_MOP_DOORBELL, LG2XX_ICMD32_SOP_DB_ZEN), 0, 0);
	adev->doorbell.is_enabled = true;
	DRM_INFO("DOORBELL of %uM enabled (table at 0x%016llX).\n",
		 (unsigned)(adev->doorbell.size >> 20),
		 (unsigned long long)adev->doorbell.rdb_gpu_addr);

	if (loonggpu_cwsr_enable)
		loonggpu_cmd_exec(adev, GSCMD(LG2XX_ICMD32_MOP_CWSR, LG2XX_ICMD32_SOP_CWSR_ZEN), 0, 0);

	return 0;
}

int loonggpu_doorbell_disable(struct loonggpu_device *adev)
{
	if (adev->family_type < CHIP_LG210 ||
	    adev->family_type == CHIP_LG211)
		return 0;

	loonggpu_cmd_exec(adev, GSCMD(LG2XX_ICMD32_MOP_DOORBELL, LG2XX_ICMD32_SOP_DB_ZDIS), 0, 0);
	if (adev->rmmio_sc)
		writel(0, ((void __iomem *)adev->rmmio_sc) + LG2XX_SC132_DOORBELL_REG);
	adev->doorbell.is_enabled = false;
	DRM_INFO("DOORBELL of %uM disabled (table at 0x%016llX).\n",
		 (unsigned)(adev->doorbell.size >> 20),
		 (unsigned long long)adev->doorbell.rdb_cpu_addr);

	if (loonggpu_cwsr_enable)
		loonggpu_cmd_exec(adev, GSCMD(LG2XX_ICMD32_MOP_CWSR, LG2XX_ICMD32_SOP_CWSR_ZDIS), 0, 0);

	return 0;
}


/*
 * GPU doorbell aperture helpers function.
 */
/**
 * loonggpu_doorbell_init - Init doorbell driver information.
 *
 * @adev: loonggpu_device pointer
 *
 * Init doorbell driver information (CIK)
 * Returns 0 on success, error on failure.
 */
int loonggpu_doorbell_init(struct loonggpu_device *adev)
{
	/* No doorbell on not LG210 hardware generation */
	if (adev->family_type < CHIP_LG210 ||
	    adev->family_type == CHIP_LG211) {
		adev->doorbell.base = 0;
		adev->doorbell.size = 0;
		adev->doorbell.num_kernel_doorbells = 0;
		return 0;
	}

	if (pci_resource_flags(adev->pdev, 2) & IORESOURCE_UNSET)
		return -EINVAL;

	loonggpu_asic_init_doorbell_index(adev);

	/* doorbell bar mapping */
	adev->doorbell.base = adev->rmmio_base + 0x800000;
	adev->doorbell.size = 0x400000;

	adev->doorbell.num_kernel_doorbells =
		min_t(u64, adev->doorbell.size / sizeof(u64),
		      adev->doorbell_index.max_assignment + 1);
	if (adev->doorbell.num_kernel_doorbells == 0)
		return -EINVAL;

	/*
	 * For Vega, reserve and map two pages on doorbell BAR since SDMA
	 * paging queue doorbell use the second page. The
	 * LOONGGPU_DOORBELL64_MAX_ASSIGNMENT definition assumes all the
	 * doorbells are in the first page. So with paging queue enabled,
	 * the max num_kernel_doorbells should + 1 page (0x400 in dword)
	 */
	adev->doorbell.num_kernel_doorbells += 0x400;

	adev->doorbell.is_enabled = false;
	return 0;
}

/**
 * loonggpu_doorbell_fini - Tear down doorbell driver information.
 *
 * @adev: loonggpu_device pointer
 *
 * Tear down doorbell driver information (CIK)
 */
void loonggpu_doorbell_fini(struct loonggpu_device *adev)
{
	loonggpu_bo_free_kernel(&adev->doorbell.rdb,
			      &adev->doorbell.rdb_gpu_addr,
			      (void **)&adev->doorbell.rdb_cpu_addr);

	loonggpu_bo_free_kernel(&adev->doorbell.kernel_doorbells,
			      NULL,
			      (void **)&adev->doorbell.cpu_addr);
}
