// SPDX-License-Identifier: GPL-2.0
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2026 Loongson Technology Ltd.
 * Author:	Xianglai Li <lixianglai@loongson.cn>
 */

#include <linux/acpi.h>
#include <linux/printk.h>
#include "iommu.h"
#include "iommu_mem.h"

#define ALLOC_MEM SZ_64M
#define ALLOC_PAGE_SIZE IOMMU_PAGE_SIZE
#define ALLOC_PAGES (ALLOC_MEM / ALLOC_PAGE_SIZE)

static struct loongson_iommu_mem {
	ulong vaddr;
	void *mem_bitmap;
	unsigned long bitmap_sz;
	spinlock_t bitmap_lock;
	bool init_failed;
} iommu_mem;

void *loongson_iommu_alloc_page(void)
{
	void *page;
	unsigned long index;

	if (iommu_mem.init_failed) {
		pr_info("%s iommu mem init failed!!\n", __func__);
		return NULL;
	}

	spin_lock(&iommu_mem.bitmap_lock);
	index = find_first_zero_bit(iommu_mem.mem_bitmap,
				iommu_mem.bitmap_sz);
	if (index < iommu_mem.bitmap_sz)
		__set_bit(index, iommu_mem.mem_bitmap);
	spin_unlock(&iommu_mem.bitmap_lock);

	if (index >= iommu_mem.bitmap_sz) {
		pr_info("%s Insufficient memory index %lu bitmap_sz %lu\n",
				__func__, index, iommu_mem.bitmap_sz);
		return NULL;
	}

	page = (void *)(iommu_mem.vaddr + index * ALLOC_PAGE_SIZE);
	memset(page, 0, ALLOC_PAGE_SIZE);
	pr_debug("%s Using iommu api to alloc memory %lx %lx\n",
			__func__, (ulong)page, iommu_mem.vaddr);

	return page;
}
EXPORT_SYMBOL(loongson_iommu_alloc_page);

void loongson_iommu_free_page(void *page)
{
	unsigned long index, offset;

	if (iommu_mem.init_failed) {
		pr_info("%s iommu mem init failed!!\n", __func__);
		return;
	}

	offset = (ulong)page - iommu_mem.vaddr;
	index = offset / ALLOC_PAGE_SIZE;
	if (index >= iommu_mem.bitmap_sz) {
		pr_info("%s Using the wrong api to free memory %lx %lx\n",
				__func__, (ulong)page, iommu_mem.vaddr);
		return;
	}

	spin_lock(&iommu_mem.bitmap_lock);
	__clear_bit(index, iommu_mem.mem_bitmap);
	spin_unlock(&iommu_mem.bitmap_lock);
}
EXPORT_SYMBOL(loongson_iommu_free_page);

static int __init loongson_iommu_mem_init(void)
{
	int bytes;
	acpi_status status;
	struct page *pages;
	struct acpi_table_header *ivrs_base;

	status = acpi_get_table("IVRS", 0, &ivrs_base);
	if (status == AE_NOT_FOUND) {
		iommu_mem.init_failed = true;
		pr_info("%s get ivrs table failed\n", __func__);
		return 0;
	}
	acpi_put_table(ivrs_base);

	pages = alloc_contig_pages(ALLOC_PAGES, GFP_KERNEL, numa_mem_id(), &node_online_map);
	if (!pages) {
		iommu_mem.init_failed = true;
		pr_info("%s Unable to alloc memory for iommu page table\n", __func__);
		return 0;
	}

	bytes = ALLOC_PAGES / 8;
	iommu_mem.vaddr = (ulong)page_address(pages);
	iommu_mem.bitmap_sz = (ALLOC_PAGES % 8) ? (bytes + 1) : bytes;
	iommu_mem.mem_bitmap = bitmap_zalloc(iommu_mem.bitmap_sz, GFP_KERNEL);

	spin_lock_init(&iommu_mem.bitmap_lock);
	pr_info("%s alloc iommu page table mem %lx bitmap %lx map_size %lu\n",
			__func__, iommu_mem.vaddr,
			(ulong)iommu_mem.mem_bitmap, iommu_mem.bitmap_sz);

	if (!iommu_mem.mem_bitmap) {
		iommu_mem.init_failed = true;
		pr_info("%s Failed to allocate bitmap for iommu\n", __func__);
	}

	return 0;
}
arch_initcall(loongson_iommu_mem_init);
