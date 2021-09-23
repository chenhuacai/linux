// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2020-2021 Loongson Technology Ltd.
 * Author:	Lv Chen <lvchen@loongson.cn>
 *		Wang Yang <wangyang@loongson.cn>
 */

#ifndef LOONGSON_IOMMU_H
#define LOONGSON_IOMMU_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <asm/addrspace.h>

#define IOVA_WIDTH		47

/* Bit value definition for I/O PTE fields */
#define IOMMU_PTE_PR		(1ULL << 0)	/* Present */
#define IOMMU_PTE_HP		(1ULL << 1)	/* HugePage */
#define IOMMU_PTE_IR		(1ULL << 2)	/* Readable */
#define IOMMU_PTE_IW		(1ULL << 3)	/* Writeable */
#define IOMMU_PTE_RW		(IOMMU_PTE_PR | IOMMU_PTE_IR | IOMMU_PTE_IW)

#define IOMMU_PTE_PRESENT(pte)	((pte) & IOMMU_PTE_PR)
#define IOMMU_PTE_HUGEPAGE(pte)	((pte) & IOMMU_PTE_HP)

#define iommu_pt_present(shadow_entry)	((*shadow_entry != 0))

/*
 * shadow_entry using kmalloc to request memory space,
 * the memory address requested by kmalloc is ARCH_DMA_MINALIGN-aligned,
 * when the shadow_entry address is not a ARCH_DMA_MINALIGN-aligned
 * address, we think that shadow_entry store large pages
 */
#define iommu_pt_huge(shd_entry_ptr)	 ((*shd_entry_ptr) & IOMMU_PTE_HP)

#define LA_IOMMU_PGSIZE		(SZ_16K | SZ_32M)

#define IOMMU_PT_LEVEL0		0x00
#define IOMMU_PT_LEVEL1		0x01

/* IOMMU page table */
#define IOMMU_PAGE_SHIFT	PAGE_SHIFT
#define IOMMU_PAGE_SIZE		(_AC(1, UL) << IOMMU_PAGE_SHIFT)
#define IOMMU_LEVEL_STRIDE	(IOMMU_PAGE_SHIFT - 3)
#define IOMMU_PTRS_PER_LEVEL	(IOMMU_PAGE_SIZE >> 3)
#define IOMMU_LEVEL_SHIFT(n)	(((n) * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT)
#define IOMMU_LEVEL_SIZE(n)	(_AC(1, UL) << (((n) * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT))
#define IOMMU_LEVEL_MASK(n)	(~(IOMMU_LEVEL_SIZE(n) - 1))
#define IOMMU_LEVEL_MAX	        DIV_ROUND_UP((IOVA_WIDTH - IOMMU_PAGE_SHIFT), IOMMU_LEVEL_STRIDE)
#define IOMMU_PAGE_MASK		(~(IOMMU_PAGE_SIZE - 1))

#define IOMMU_HPAGE_SIZE	(1UL << IOMMU_LEVEL_SHIFT(IOMMU_PT_LEVEL1))
#define IOMMU_HPAGE_MASK	(~(IOMMU_HPAGE_SIZE - 1))

/* Virtio page use size of 16k */
#define LA_VIRTIO_PAGE_SHIFT	14
#define LA_VIRTIO_PAGE_SIZE	(_AC(1, UL) << LA_VIRTIO_PAGE_SHIFT)
#define LA_VIRTIO_PAGE_MASK	(~((1ULL << LA_VIRTIO_PAGE_SHIFT) - 1))

/* Bits of iommu map address space field */
#define LA_IOMMU_PFN_LO			0x0
#define PFN_LO_SHIFT			12
#define LA_IOMMU_PFN_HI			0x4
#define PFN_HI_MASK			0x3ffff
#define LA_IOMMU_VFN_LO			0x8
#define VFN_LO_SHIFT			12
#define LA_IOMMU_VFN_HI			0xC
#define VFN_HI_MASK			0x3ffff

/* wired | index | domain | shift */
#define LA_IOMMU_WIDS			0x10
/* valid | busy | tlbar/aw | cmd */
#define LA_IOMMU_VBTC			0x14
#define IOMMU_PGTABLE_BUSY		(1 << 16)
/* enable |index | valid | domain | bdf */
#define LA_IOMMU_EIVDB			0x18
/* enable | valid | cmd */
#define LA_IOMMU_CMD			0x1C
#define LA_IOMMU_PGD0_LO		0x20
#define LA_IOMMU_PGD0_HI		0x24
#define STEP_PGD			0x8
#define STEP_PGD_SHIFT			3
#define LA_IOMMU_PGD_LO(domain_id)	\
		(LA_IOMMU_PGD0_LO + ((domain_id) << STEP_PGD_SHIFT))
#define LA_IOMMU_PGD_HI(domain_id)	\
		(LA_IOMMU_PGD0_HI + ((domain_id) << STEP_PGD_SHIFT))

#define LA_IOMMU_DIR_CTRL0		0xA0
#define LA_IOMMU_DIR_CTRL1		0xA4
#define LA_IOMMU_DIR_CTRL(x)		(LA_IOMMU_DIR_CTRL0 + ((x) << 2))

#define LA_IOMMU_SAFE_BASE_HI		0xE0
#define LA_IOMMU_SAFE_BASE_LO		0xE4
#define LA_IOMMU_EX_ADDR_LO		0xE8
#define LA_IOMMU_EX_ADDR_HI		0xEC

#define LA_IOMMU_PFM_CNT_EN		0x100

#define LA_IOMMU_RD_HIT_CNT_0		0x110
#define LA_IOMMU_RD_MISS_CNT_O		0x114
#define LA_IOMMU_WR_HIT_CNT_0		0x118
#define LA_IOMMU_WR_MISS_CNT_0		0x11C
#define LA_IOMMU_RD_HIT_CNT_1		0x120
#define LA_IOMMU_RD_MISS_CNT_1		0x124
#define LA_IOMMU_WR_HIT_CNT_1		0x128
#define LA_IOMMU_WR_MISS_CNT_1		0x12C
#define LA_IOMMU_RD_HIT_CNT_2		0x130
#define LA_IOMMU_RD_MISS_CNT_2		0x134
#define LA_IOMMU_WR_HIT_CNT_2		0x138
#define LA_IOMMU_WR_MISS_CNT_2		0x13C

#define MAX_DOMAIN_ID			16
#define MAX_ATTACHED_DEV_ID		16
#define MAX_PAGES_NUM			(SZ_128M / IOMMU_PAGE_SIZE)

#define iommu_ptable_end(addr, end, level)								\
({	unsigned long __boundary = ((addr) + IOMMU_LEVEL_SIZE(level)) & IOMMU_LEVEL_MASK(level);	\
	(__boundary - 1 < (end) - 1) ? __boundary : (end);						\
})

/* To find an entry in an iommu page table directory */
#define iommu_shadow_index(addr, level)		\
		(((addr) >> ((level * IOMMU_LEVEL_STRIDE) + IOMMU_PAGE_SHIFT)) & (IOMMU_PTRS_PER_LEVEL - 1))

/* IOMMU iommu_table entry */
typedef struct { unsigned long iommu_pte; } iommu_pte;

typedef struct loongson_iommu {
	struct list_head	list;
	spinlock_t		domain_bitmap_lock;		/* Lock for domain allocing */
	spinlock_t		dom_info_lock;			/* Lock for priv->list */
	spinlock_t		pgtable_bitmap_lock;		/* Lock for bitmap of page table */
	struct mutex		loongson_iommu_pgtlock;		/* Lock for iommu page table */
	void			*domain_bitmap;			/* Bitmap of global domains */
	void			*devtable_bitmap;		/* Bitmap of devtable */
	void			*pgtable_bitmap;		/* Bitmap of devtable pages for page table */
	struct list_head	dom_list;			/* List of all domain privates */
	u16 			devid;				/* PCI device id of the IOMMU device */
	int			segment;			/* PCI segment# */
	void			*membase;
	void			*pgtbase;
	unsigned long		maxpages;
} loongson_iommu;

struct loongson_iommu_rlookup_entry
{
	struct list_head		list;
	struct loongson_iommu		**loongson_iommu_rlookup_table;
	int				pcisegment;
};

/* shadow page table entry */
typedef struct spt_entry {
	unsigned long		*gmem_ptable;		/* gmemory entry address base*/
	unsigned long		*shadow_ptable;		/* virtual address base for shadow page */
	int			index;			/* index 128M gmem */
	int			dirty;
	int			present;
} spt_entry;

typedef struct iommu_info {
	struct list_head	list;		/* for loongson_iommu_pri->iommu_devlist */
	spt_entry		*shadow_pgd;
	struct loongson_iommu	*iommu;
	spinlock_t		devlock;	/* priv dev list lock */
	struct list_head	dev_list;	/* List of all devices in this domain iommu */
	unsigned int		dev_cnt;	/* devices assigned to this domain iommu */
	short			id;
} iommu_info;

/* One vm is equal to a domain, one domain has a priv */
typedef struct dom_info {
	struct list_head	list;			/* For list of all domains */
	struct list_head	iommu_devlist;
	struct iommu_domain	domain;
	void			*mmio_pgd;		/* 0x10000000~0x8fffffff */
	spinlock_t		lock;			/* Lock for priv->iommu_devlist */
} dom_info;

/* A device for passthrough */
struct loongson_iommu_dev_data {
	struct list_head	list;		/* for iommu_entry->dev_list */
	struct loongson_iommu	*iommu;
	iommu_info		*iommu_entry;
	unsigned short		bdf;
	int			count;
	int			index;		/* index in device table */
};

static inline unsigned long iommu_pgt_v2p(loongson_iommu *iommu, void *va)
{
	return (unsigned long)(va - iommu->pgtbase);
}

static inline unsigned long *iommu_ptable_offset(unsigned long *table_entry,
						unsigned long addr, int level)
{
	return table_entry + iommu_shadow_index(addr, level);
}

static inline unsigned long *iommu_shadow_offset(spt_entry *shadow_entry,
						unsigned long addr, int level)
{
	unsigned long *table_base;

	table_base = shadow_entry->shadow_ptable;

	return table_base + iommu_shadow_index(addr, level);
}

#endif	/* LOONGSON_IOMMU_H */
