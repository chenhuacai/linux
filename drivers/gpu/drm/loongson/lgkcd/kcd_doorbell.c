#include "kcd_priv.h"
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/idr.h>

/*
 * This extension supports a kernel level doorbells management for the
 * kernel queues using the first doorbell page reserved for the kernel.
 */

/*
 * Each device exposes a doorbell aperture, a PCI MMIO aperture that
 * receives 32-bit writes that are passed to queues as wptr values.
 * The doorbells are intended to be written by applications as part
 * of queueing work on user-mode queues.
 * We assign doorbells to applications in PAGE_SIZE-sized and aligned chunks.
 * We map the doorbell address space into user-mode when a process creates
 * its first queue on each device.
 * Although the mapping is done by KCD, it is equivalent to an mmap of
 * the /dev/kcd with the particular device encoded in the mmap offset.
 * There will be other uses for mmap of /dev/kcd, so only a range of
 * offsets (KCD_MMAP_DOORBELL_START-END) is used for doorbells.
 */

/* # of doorbell bytes allocated for each process. */
size_t kcd_doorbell_process_slice(struct kcd_dev *kcd)
{
	return roundup(kcd->device_info.doorbell_size *
			KCD_MAX_NUM_OF_QUEUES_PER_PROCESS,
			PAGE_SIZE);
}

/* Doorbell calculations for device init. */
int kcd_doorbell_init(struct kcd_dev *kcd)
{
	int size = PAGE_SIZE;
	int r;

	/*
	 * Todo: KCD kernel level operations need only one doorbell for
	 * ring test/HWS. So instead of reserving a whole page here for
	 * kernel, reserve and consume a doorbell from existing KGD kernel
	 * doorbell page.
	 */

	/* Bitmap to dynamically allocate doorbells from kernel page */
	kcd->doorbell_bitmap = bitmap_zalloc(size / sizeof(u64), GFP_KERNEL);
	if (!kcd->doorbell_bitmap) {
		DRM_ERROR("Failed to allocate kernel doorbell bitmap\n");
		return -ENOMEM;
	}

	/* Alloc a doorbell page for KCD kernel usages */
	r = loonggpu_bo_create_kernel(kcd->adev,
				    size,
				    PAGE_SIZE,
				    LOONGGPU_GEM_DOMAIN_DOORBELL,
				    &kcd->doorbells,
				    NULL,
				    (void **)&kcd->doorbell_kernel_ptr);
	if (r) {
		pr_err("failed to allocate kernel doorbells\n");
		bitmap_free(kcd->doorbell_bitmap);
		return r;
	}

	pr_info("Doorbell kernel address == %08llx\n", (uint64_t)kcd->doorbell_kernel_ptr);
	return 0;
}

void kcd_doorbell_fini(struct kcd_dev *kcd)
{
	bitmap_free(kcd->doorbell_bitmap);
	loonggpu_bo_free_kernel(&kcd->doorbells, NULL,
			     (void **)&kcd->doorbell_kernel_ptr);
}

int kcd_doorbell_mmap(struct kcd_node *dev, struct kcd_process *process,
		      struct vm_area_struct *vma)
{
	phys_addr_t address;
	struct kcd_process_device *pdd;

	/*
	 * For simplicitly we only allow mapping of the entire doorbell
	 * allocation of a single device & process.
	 */
	if (vma->vm_end - vma->vm_start != kcd_doorbell_process_slice(dev->kcd))
		return -EINVAL;

	pdd = kcd_get_process_device_data(dev, process);
	if (!pdd)
		return -EINVAL;

	/* Calculate physical address of doorbell */
	address = kcd_get_process_doorbells(pdd);
	if (!address)
		return -ENOMEM;

	vm_flags_set(vma, VM_IO | VM_DONTCOPY | VM_DONTEXPAND | VM_NORESERVE| VM_DONTDUMP | VM_PFNMAP);

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	pr_debug("Mapping doorbell page\n"
		 "     target user address == 0x%08llX\n"
		 "     physical address    == 0x%08llX\n"
		 "     vm_flags            == 0x%04lX\n"
		 "     size                == 0x%04lX\n",
		 (unsigned long long) vma->vm_start, address, vma->vm_flags,
		 kcd_doorbell_process_slice(dev->kcd));


	return io_remap_pfn_range(vma,
				vma->vm_start,
				address >> PAGE_SHIFT,
				kcd_doorbell_process_slice(dev->kcd),
				vma->vm_page_prot);
}


/* get kernel iomem pointer for a doorbell */
void __iomem *kcd_get_kernel_doorbell(struct kcd_dev *kcd,
					unsigned int *doorbell_off)
{
	u32 inx;

	mutex_lock(&kcd->doorbell_mutex);
	inx = find_first_zero_bit(kcd->doorbell_bitmap, PAGE_SIZE / sizeof(u64));

	__set_bit(inx, kcd->doorbell_bitmap);
	mutex_unlock(&kcd->doorbell_mutex);

	if (inx >= KCD_MAX_NUM_OF_QUEUES_PER_PROCESS)
		return NULL;

	*doorbell_off = loonggpu_doorbell_index_on_bar(kcd->adev, kcd->doorbells, inx);

	pr_info("Get kernel queue doorbell\n"
			"     doorbell offset   == 0x%08X\n"
			"     doorbell index    == 0x%x\n",
		*doorbell_off, inx);

	return kcd->doorbell_kernel_ptr + inx;
}

void kcd_release_kernel_doorbell(struct kcd_dev *kcd, u64 __iomem *db_addr)
{
	unsigned int inx;

	inx = (unsigned int)(db_addr - kcd->doorbell_kernel_ptr);

	mutex_lock(&kcd->doorbell_mutex);
	__clear_bit(inx, kcd->doorbell_bitmap);
	mutex_unlock(&kcd->doorbell_mutex);
}

void write_kernel_doorbell64(void __iomem *db, u64 value)
{
	if (db) {
		WARN(((unsigned long)db & 7) != 0,
		     "Unaligned 64-bit doorbell");
		writeq(value, (u64 __iomem *)db);
		pr_debug("writing %llu to doorbell address %08llx\n", value, (uint64_t)db);
	}
}

static int init_doorbell_bitmap(struct qcm_process_device *qpd,
				struct kcd_dev *dev)
{
	unsigned int i;
	int range_start = dev->shared_resources.non_cp_doorbells_start;
	int range_end = dev->shared_resources.non_cp_doorbells_end;

	/* Mask out doorbells reserved for SDMA, IH, and VCN on SOC15. */
	pr_debug("reserved doorbell 0x%03x - 0x%03x\n", range_start, range_end);
	pr_debug("reserved doorbell 0x%03x - 0x%03x\n",
			range_start + KCD_QUEUE_DOORBELL_MIRROR_OFFSET,
			range_end + KCD_QUEUE_DOORBELL_MIRROR_OFFSET);

	for (i = 0; i < KCD_MAX_NUM_OF_QUEUES_PER_PROCESS / 2; i++) {
		if (i >= range_start && i <= range_end) {
			__set_bit(i, qpd->doorbell_bitmap);
			__set_bit(i + KCD_QUEUE_DOORBELL_MIRROR_OFFSET,
				  qpd->doorbell_bitmap);
		}
	}

	return 0;
}

phys_addr_t kcd_get_process_doorbells(struct kcd_process_device *pdd)
{
	struct loonggpu_device *adev = pdd->dev->adev;
	uint32_t first_db_index;

	if (!pdd->qpd.proc_doorbells) {
		if (kcd_alloc_process_doorbells(pdd->dev->kcd, pdd))
			/* phys_addr_t 0 is error */
			return 0;
	}

	first_db_index = loonggpu_doorbell_index_on_bar(adev, pdd->qpd.proc_doorbells, 0);
	return adev->doorbell.base + first_db_index * sizeof(uint64_t);
}

int kcd_alloc_process_doorbells(struct kcd_dev *kcd, struct kcd_process_device *pdd)
{
	int r;
	struct qcm_process_device *qpd = &pdd->qpd;

	/* Allocate bitmap for dynamic doorbell allocation */
	qpd->doorbell_bitmap = bitmap_zalloc(KCD_MAX_NUM_OF_QUEUES_PER_PROCESS,
					     GFP_KERNEL);
	if (!qpd->doorbell_bitmap) {
		DRM_ERROR("Failed to allocate process doorbell bitmap\n");
		return -ENOMEM;
	}

	r = init_doorbell_bitmap(&pdd->qpd, kcd);
	if (r) {
		DRM_ERROR("Failed to initialize process doorbells\n");
		r = -ENOMEM;
		goto err;
	}

	/* Allocate doorbells for this process */
	r = loonggpu_bo_create_kernel(kcd->adev,
				    kcd_doorbell_process_slice(kcd),
				    PAGE_SIZE,
				    LOONGGPU_GEM_DOMAIN_DOORBELL,
				    &qpd->proc_doorbells,
				    NULL,
				    NULL);
	if (r) {
		DRM_ERROR("Failed to allocate process doorbells\n");
		goto err;
	}
	return 0;

err:
	bitmap_free(qpd->doorbell_bitmap);
	qpd->doorbell_bitmap = NULL;
	return r;
}

void kcd_free_process_doorbells(struct kcd_dev *kcd, struct kcd_process_device *pdd)
{
	struct qcm_process_device *qpd = &pdd->qpd;

	if (qpd->doorbell_bitmap) {
		bitmap_free(qpd->doorbell_bitmap);
		qpd->doorbell_bitmap = NULL;
	}

	loonggpu_bo_free_kernel(&qpd->proc_doorbells, NULL, NULL);
}
