// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2020-2021 Loongson Technology Ltd.
 * Author:	Lv Chen <lvchen@loongson.cn>
 *		Wang Yang <wangyang@loongson.cn>
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "iommu.h"

#define LOOP_TIMEOUT		100000
#define IOVA_START		(SZ_256M)
#define IOVA_END0		(SZ_2G + SZ_256M)

#define IVRS_HEADER_LENGTH		48
#define ACPI_IVHD_TYPE_MAX_SUPPORTED	0x40
#define IVHD_DEV_ALL                    0x01
#define IVHD_DEV_SELECT                 0x02
#define IVHD_DEV_SELECT_RANGE_START     0x03
#define IVHD_DEV_RANGE_END              0x04
#define IVHD_DEV_ALIAS                  0x42
#define IVHD_DEV_EXT_SELECT             0x46
#define IVHD_DEV_ACPI_HID		0xf0

#define IVHD_HEAD_TYPE10		0x10
#define IVHD_HEAD_TYPE11		0x11
#define IVHD_HEAD_TYPE40		0x40

#define MAX_BDF_NUM			0xffff

#define RLOOKUP_TABLE_ENTRY_SIZE	(sizeof(void *))

/*
 * structure describing one IOMMU in the ACPI table. Typically followed by one
 * or more ivhd_entrys.
 */
struct ivhd_header {
	u8 type;
	u8 flags;
	u16 length;
	u16 devid;
	u16 cap_ptr;
	u64 mmio_phys;
	u16 pci_seg;
	u16 info;
	u32 efr_attr;

	/* Following only valid on IVHD type 11h and 40h */
	u64 efr_reg; /* Exact copy of MMIO_EXT_FEATURES */
	u64 res;
} __attribute__((packed));

/*
 * A device entry describing which devices a specific IOMMU translates and
 * which requestor ids they use.
 */
struct ivhd_entry {
	u8 type;
	u16 devid;
	u8 flags;
	u32 ext;
	u32 hidh;
	u64 cid;
	u8 uidf;
	u8 uidl;
	u8 uid;
} __attribute__((packed));

LIST_HEAD(loongson_iommu_list);			/* list of all IOMMUs in the system */
LIST_HEAD(loongson_rlookup_iommu_list);

static u32 rlookup_table_size;			/* size if the rlookup table */
static int loongson_iommu_target_ivhd_type;
u16	loongson_iommu_last_bdf;		/* largest PCI device id we have to handle */

int loongson_iommu_disable;
static struct iommu_ops loongson_iommu_ops;

static void iommu_write_regl(loongson_iommu *iommu, unsigned long off, u32 val)
{
	*(u32 *)(iommu->membase + off) = val;
	iob();
}

static u32 iommu_read_regl(loongson_iommu *iommu, unsigned long off)
{
	u32 val;

	val = *(u32 *)(iommu->membase + off);
	iob();
	return val;
}

static void iommu_translate_disable(loongson_iommu *iommu)
{
	u32 val;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	val = iommu_read_regl(iommu, LA_IOMMU_EIVDB);

	/* Disable */
	val &= ~(1 << 31);
	iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

	/* Write cmd */
	val = iommu_read_regl(iommu, LA_IOMMU_CMD);
	val &= 0xfffffffc;
	iommu_write_regl(iommu, LA_IOMMU_CMD, val);
}

static void iommu_translate_enable(loongson_iommu *iommu)
{
	u32 val = 0;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	val = iommu_read_regl(iommu, LA_IOMMU_EIVDB);

	/* Enable */
	val |= (1 << 31);
	iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

	/* Write cmd */
	val = iommu_read_regl(iommu, LA_IOMMU_CMD);
	val &= 0xfffffffc;
	iommu_write_regl(iommu, LA_IOMMU_CMD, val);
}

static bool loongson_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static dom_info *to_dom_info(struct iommu_domain *dom)
{
	return container_of(dom, dom_info, domain);
}

/*
 * Check whether the system has a priv.
 * If yes, it returns 1 and if not, it returns 0
 */
static int has_dom(loongson_iommu *iommu)
{
	spin_lock(&iommu->dom_info_lock);
	while (!list_empty(&iommu->dom_list)) {
		spin_unlock(&iommu->dom_info_lock);
		return 1;
	}
	spin_unlock(&iommu->dom_info_lock);

	return 0;
}

static int update_dev_table(struct loongson_iommu_dev_data *dev_data, int flag)
{
	u32 val = 0;
	int index;
	unsigned short bdf;
	loongson_iommu *iommu;
	u16 domain_id;

	if (dev_data == NULL) {
		pr_err("%s dev_data is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	if (dev_data->iommu_entry == NULL) {
		pr_err("%s iommu_entry is NULL", __func__);
		return 0;
	}

	iommu = dev_data->iommu;
	domain_id = dev_data->iommu_entry->id;
	bdf = dev_data->bdf;

	/* Set device table */
	if (flag) {
		index = find_first_zero_bit(iommu->devtable_bitmap, MAX_ATTACHED_DEV_ID);
		if (index < MAX_ATTACHED_DEV_ID) {
			__set_bit(index, iommu->devtable_bitmap);
			dev_data->index = index;
		} else {
			pr_err("%s get id from dev table failed\n", __func__);
			return 0;
		}

		pr_info("%s bdf %x domain_id %d index %x"
				" iommu segment %d flag %x\n",
				__func__, bdf, domain_id, index,
				iommu->segment, flag);

		val = bdf & 0xffff;
		val |= ((domain_id & 0xf) << 16);	/* domain id */
		val |= ((index & 0xf) << 24);		/* index */
		val |= (0x1 << 20);			/* valid */
		val |= (0x1 << 31);			/* enable */
		iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

		val = iommu_read_regl(iommu, LA_IOMMU_CMD);
		val &= 0xfffffffc;
		iommu_write_regl(iommu, LA_IOMMU_CMD, val);
	} else {
		/* Flush device table */
		index = dev_data->index;
		pr_info("%s bdf %x domain_id %d index %x"
				" iommu segment %d flag %x\n",
				__func__, bdf, domain_id, index,
				iommu->segment, flag);

		val = iommu_read_regl(iommu, LA_IOMMU_EIVDB);
		val &= ~(0x7fffffff);
		val |= ((index & 0xf) << 24);		/* index */
		iommu_write_regl(iommu, LA_IOMMU_EIVDB, val);

		val = iommu_read_regl(iommu, LA_IOMMU_CMD);
		val &= 0xfffffffc;
		iommu_write_regl(iommu, LA_IOMMU_CMD, val);

		if (index < MAX_ATTACHED_DEV_ID)
			__clear_bit(index, iommu->devtable_bitmap);
	}

	return 0;
}

static void flush_iotlb(loongson_iommu *iommu)
{
	u32 val, cmd;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	val = iommu_read_regl(iommu, LA_IOMMU_VBTC);
	val &= ~0x1f;

	/* Flush all tlb */
	val |= 0x5;
	iommu_write_regl(iommu, LA_IOMMU_VBTC, val);

	cmd = iommu_read_regl(iommu, LA_IOMMU_CMD);
	cmd &= 0xfffffffc;
	iommu_write_regl(iommu, LA_IOMMU_CMD, cmd);
}

static int flush_pgtable_is_busy(loongson_iommu *iommu)
{
	u32 val;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	val = iommu_read_regl(iommu, LA_IOMMU_VBTC);

	return val & IOMMU_PGTABLE_BUSY;
}

static int __iommu_flush_iotlb_all(loongson_iommu *iommu)
{
	u32 retry = 0;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return 0;
	}

	flush_iotlb(iommu);
	while (flush_pgtable_is_busy(iommu)) {
		if (retry == LOOP_TIMEOUT) {
			pr_err("Loongson-IOMMU: iotlb flush busy\n");
			return -EIO;
		}
		retry++;
		udelay(1);
	}
	iommu_translate_enable(iommu);

	return 0;
}

static void priv_flush_iotlb_pde(loongson_iommu *iommu)
{
	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	__iommu_flush_iotlb_all(iommu);
}

static void do_attach(iommu_info *info, struct loongson_iommu_dev_data *dev_data)
{
	if (!dev_data->count)
		return;

	dev_data->iommu_entry = info;

	spin_lock(&info->devlock);
	list_add(&dev_data->list, &info->dev_list);
	info->dev_cnt += 1;
	spin_unlock(&info->devlock);

	update_dev_table(dev_data, 1);
	if (info->dev_cnt > 0)
		priv_flush_iotlb_pde(dev_data->iommu);
}

static void do_detach(struct loongson_iommu_dev_data *dev_data)
{
	iommu_info *iommu_entry = NULL;

	if (dev_data == NULL) {
		pr_err("%s dev_data is NULL", __func__);
		return;
	}

	if (dev_data->count)
		return;

	iommu_entry = dev_data->iommu_entry;
	if (iommu_entry == NULL) {
		pr_err("%s iommu_entry is NULL", __func__);
		return;
	}

	list_del(&dev_data->list);
	iommu_entry->dev_cnt -= 1;

	update_dev_table(dev_data, 0);
	dev_data->iommu_entry = NULL;
}

static void cleanup_iommu_entry(iommu_info *iommu_entry)
{
	struct loongson_iommu_dev_data *dev_data = NULL;

	spin_lock(&iommu_entry->devlock);
	while (!list_empty(&iommu_entry->dev_list)) {
		dev_data = list_first_entry(&iommu_entry->dev_list,
				struct loongson_iommu_dev_data, list);
		do_detach(dev_data);
	}

	spin_unlock(&iommu_entry->devlock);
}

static int domain_id_alloc(loongson_iommu *iommu)
{
	int id = -1;

	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return id;
	}

	spin_lock(&iommu->domain_bitmap_lock);
	id = find_first_zero_bit(iommu->domain_bitmap, MAX_DOMAIN_ID);
	if (id < MAX_DOMAIN_ID)
		__set_bit(id, iommu->domain_bitmap);
	else
		pr_err("Loongson-IOMMU: Alloc domain id over max domain id\n");

	spin_unlock(&iommu->domain_bitmap_lock);

	return id;
}

static void domain_id_free(loongson_iommu *iommu, int id)
{
	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	spin_lock(&iommu->domain_bitmap_lock);
	if ((id >= 0) && (id < MAX_DOMAIN_ID))
		__clear_bit(id, iommu->domain_bitmap);

	spin_unlock(&iommu->domain_bitmap_lock);
}

/*
 *  This function adds a private domain to the global domain list
 */
static void add_domain_to_list(loongson_iommu *iommu, dom_info *priv)
{
	spin_lock(&iommu->dom_info_lock);
	list_add(&priv->list, &iommu->dom_list);
	spin_unlock(&iommu->dom_info_lock);
}

static void del_domain_from_list(loongson_iommu *iommu, dom_info *priv)
{
	spin_lock(&iommu->dom_info_lock);
	list_del(&priv->list);
	spin_unlock(&iommu->dom_info_lock);
}

static spt_entry *iommu_zalloc_page(loongson_iommu *iommu)
{
	int index;
	void *addr;
	spt_entry *shd_entry;

	spin_lock(&iommu->pgtable_bitmap_lock);
	index = find_first_zero_bit(iommu->pgtable_bitmap, iommu->maxpages);
	if (index < iommu->maxpages)
		__set_bit(index, iommu->pgtable_bitmap);
	spin_unlock(&iommu->pgtable_bitmap_lock);

	shd_entry = NULL;
	if (index < iommu->maxpages) {
		shd_entry = kmalloc(sizeof(*shd_entry), GFP_KERNEL);
		if (!shd_entry) {
			pr_err("%s alloc memory for shadow page entry failed\n", __func__);
			goto fail;
		}

		shd_entry->shadow_ptable = (unsigned long *)get_zeroed_page(GFP_KERNEL);
		if (!shd_entry->shadow_ptable) {
			pr_err("Loongson-IOMMU: get zeroed page err\n");
			kfree(shd_entry);
			goto fail;
		}

		addr = iommu->pgtbase + index * IOMMU_PAGE_SIZE;
		memset(addr, 0x0, IOMMU_PAGE_SIZE);
		shd_entry->index = index;
		shd_entry->gmem_ptable = addr;
	}

	return shd_entry;
fail:
	spin_lock(&iommu->pgtable_bitmap_lock);
	__clear_bit(index, iommu->pgtable_bitmap);
	spin_unlock(&iommu->pgtable_bitmap_lock);
	return NULL;
}

static void iommu_free_page(loongson_iommu *iommu, spt_entry *shadw_entry)
{
	void *addr;

	if (shadw_entry->index < iommu->maxpages) {
		addr = shadw_entry->gmem_ptable;
		memset(addr, 0x0, IOMMU_PAGE_SIZE);

		spin_lock(&iommu->pgtable_bitmap_lock);
		__clear_bit(shadw_entry->index, iommu->pgtable_bitmap);
		spin_unlock(&iommu->pgtable_bitmap_lock);

		shadw_entry->index = -1;
		free_page((unsigned long)shadw_entry->shadow_ptable);
		shadw_entry->shadow_ptable = NULL;
		shadw_entry->gmem_ptable = NULL;
		kfree(shadw_entry);
	}
}

static void free_pagetable_one_level(iommu_info *iommu_entry, spt_entry *shd_entry, int level)
{
	int i;
	unsigned long *psentry;
	spt_entry *shd_entry_tmp;
	loongson_iommu *iommu = iommu_entry->iommu;

	psentry = (unsigned long *)shd_entry;
	if (level == IOMMU_PT_LEVEL1) {
		if (iommu_pt_present(psentry) && (!iommu_pt_huge(psentry)))
			iommu_free_page(iommu, shd_entry);
		return;
	}

	for (i = 0; i < IOMMU_PTRS_PER_LEVEL; i++) {
		psentry = shd_entry->shadow_ptable + i;
		if (!iommu_pt_present(psentry))
			continue;

		shd_entry_tmp = (spt_entry *)(*psentry);
		free_pagetable_one_level(iommu_entry, shd_entry_tmp, level - 1);
	}

	iommu_free_page(iommu, shd_entry);
}

static void free_pagetable(iommu_info *iommu_entry)
{
	spt_entry *shd_entry;
	loongson_iommu *iommu;

	iommu = iommu_entry->iommu;
	shd_entry = iommu_entry->shadow_pgd;
	free_pagetable_one_level(iommu_entry, shd_entry, IOMMU_LEVEL_MAX);
	iommu_entry->shadow_pgd = NULL;
}

static dom_info *dom_info_alloc(void)
{
	dom_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL) {
		pr_err("%s alloc memory for info failed\n", __func__);
		return NULL;
	}

	/* 0x10000000~0x8fffffff */
	info->mmio_pgd = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 6);
	if (info->mmio_pgd == NULL) {
		pr_err("%s alloc memory for virtio pgtable failed\n", __func__);
		kfree(info);
		return NULL;
	}

	INIT_LIST_HEAD(&info->iommu_devlist);
	spin_lock_init(&info->lock);
	return info;
}

static void dom_info_free(dom_info *info)
{
	/* 0x10000000~0x8fffffff */
	if (info->mmio_pgd) {
		free_pages((unsigned long)info->mmio_pgd, 6);
		info->mmio_pgd = NULL;
	}

	kfree(info);
}

static struct iommu_domain *loongson_iommu_domain_alloc(unsigned int type)
{
	dom_info *info;

	switch (type) {
	case IOMMU_DOMAIN_UNMANAGED:
		info = dom_info_alloc();
		if (info == NULL)
			return NULL;

		info->domain.geometry.aperture_start	= 0;
		info->domain.geometry.aperture_end	= ~0ULL;
		info->domain.geometry.force_aperture	= true;
		break;

	default:
		return NULL;
	}

	return &info->domain;
}

void domain_deattach_iommu(dom_info *priv, iommu_info *iommu_entry)
{
	loongson_iommu *iommu = NULL;

	if (priv == NULL) {
		pr_err("%s priv is NULL", __func__);
		return;
	}

	if (iommu_entry == NULL) {
		pr_err("%s iommu_entry is NULL", __func__);
		return;
	}

	if (iommu_entry->dev_cnt != 0)
		return;

	iommu = iommu_entry->iommu;
	if (iommu == NULL) {
		pr_err("%s iommu is NULL", __func__);
		return;
	}

	domain_id_free(iommu_entry->iommu, iommu_entry->id);

	mutex_lock(&iommu->loongson_iommu_pgtlock);
	free_pagetable(iommu_entry);
	mutex_unlock(&iommu->loongson_iommu_pgtlock);

	spin_lock(&priv->lock);
	list_del(&iommu_entry->list);
	spin_unlock(&priv->lock);

	kfree(iommu_entry);
	del_domain_from_list(iommu, priv);
}

static void loongson_iommu_domain_free(struct iommu_domain *domain)
{

	dom_info *priv;
	loongson_iommu *iommu = NULL;
	struct iommu_info *iommu_entry, *iommu_entry_temp;

	priv = to_dom_info(domain);

	spin_lock(&priv->lock);
	list_for_each_entry_safe(iommu_entry, iommu_entry_temp, &priv->iommu_devlist, list) {
		iommu = iommu_entry->iommu;

		if (iommu_entry->dev_cnt > 0)
			cleanup_iommu_entry(iommu_entry);

		spin_unlock(&priv->lock);
		domain_deattach_iommu(priv, iommu_entry);
		spin_lock(&priv->lock);

		__iommu_flush_iotlb_all(iommu);

		if (!has_dom(iommu))
			iommu_translate_disable(iommu);

	}
	spin_unlock(&priv->lock);

	dom_info_free(priv);
}

struct loongson_iommu_rlookup_entry *lookup_rlooptable(int pcisegment)
{
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	list_for_each_entry(rlookupentry, &loongson_rlookup_iommu_list, list) {
		if (rlookupentry->pcisegment == pcisegment)
			return rlookupentry;
	}

	return NULL;
}

loongson_iommu *find_iommu_by_dev(struct pci_dev  *pdev)
{
	int pcisegment;
	unsigned short devid;
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;
	loongson_iommu *iommu = NULL;

	devid = pdev->devfn & 0xff;

	pcisegment = pci_domain_nr(pdev->bus);

	rlookupentry = lookup_rlooptable(pcisegment);
	if (rlookupentry == NULL) {
		pr_info("%s find segment %d rlookupentry failed\n", __func__,
				pcisegment);
		return iommu;
	}

	iommu = rlookupentry->loongson_iommu_rlookup_table[devid];

	return iommu;
}

static int iommu_init_device(struct device *dev)
{
	unsigned char busnum;
	unsigned short bdf, devid;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus *bus = pdev->bus;
	struct loongson_iommu_dev_data *dev_data;
	loongson_iommu *iommu = NULL;

	bdf = pdev->devfn & 0xff;
	busnum = bus->number;
	if (busnum != 0) {
		while (bus->parent->parent)
			bus = bus->parent;
		bdf = bus->self->devfn & 0xff;
	}

	if (dev_iommu_priv_get(dev)) {
		pr_info("Loongson-IOMMU: bdf:0x%x has added\n", bdf);
		return 0;
	}

	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	devid = PCI_DEVID(bus->number, bdf);
	dev_data->bdf = devid;

	pci_info(pdev, "%s devid %x bus %x\n", __func__, devid, busnum);
	iommu = find_iommu_by_dev(pdev);
	if (iommu == NULL)
		pci_info(pdev, "%s find iommu failed by dev\n", __func__);

	/* The initial state is 0, and 1 is added only when attach dev */
	dev_data->count = 0;
	dev_data->iommu = iommu;

	dev_iommu_priv_set(dev, dev_data);

	return 0;
}

static struct iommu_device *loongson_iommu_probe_device(struct device *dev)
{
	int ret = 0;

	ret = iommu_init_device(dev);
	if (ret < 0)
		pr_err("Loongson-IOMMU: unable to alloc memory for dev_data\n");

	return 0;
}

static struct iommu_group *loongson_iommu_device_group(struct device *dev)
{
	struct iommu_group *group;

	/*
	 * We don't support devices sharing stream IDs other than PCI RID
	 * aliases, since the necessary ID-to-device lookup becomes rather
	 * impractical given a potential sparse 32-bit stream ID space.
	 */
	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
		group = generic_device_group(dev);

	return group;
}

static void loongson_iommu_release_device(struct device *dev)
{
	struct loongson_iommu_dev_data *dev_data;

	dev_data = dev_iommu_priv_get(dev);
	dev_iommu_priv_set(dev, NULL);
	kfree(dev_data);
}

iommu_info *get_first_iommu_entry(dom_info *priv)
{
	struct iommu_info *iommu_entry;

	if (priv == NULL) {
		pr_err("%s priv is NULL", __func__);
		return NULL;
	}

	iommu_entry = list_first_entry_or_null(&priv->iommu_devlist,
			struct iommu_info, list);

	return iommu_entry;
}

iommu_info *get_iommu_entry(dom_info *priv, loongson_iommu *iommu)
{
	struct iommu_info *iommu_entry;

	spin_lock(&priv->lock);
	list_for_each_entry(iommu_entry, &priv->iommu_devlist, list) {
		if (iommu_entry->iommu == iommu) {
			spin_unlock(&priv->lock);
			return iommu_entry;
		}
	}
	spin_unlock(&priv->lock);

	return NULL;
}

iommu_info *domain_attach_iommu(dom_info *priv, loongson_iommu *iommu)
{
	unsigned long pgd_pa;
	u32 dir_ctrl, pgd_lo, pgd_hi;
	struct iommu_info *iommu_entry = NULL;
	spt_entry *shd_entry = NULL;

	iommu_entry = get_iommu_entry(priv, iommu);
	if (iommu_entry)
		return iommu_entry;

	iommu_entry = kzalloc(sizeof(struct iommu_info), GFP_KERNEL);
	if (iommu_entry == NULL) {
		pr_info("%s alloc memory for iommu_entry failed\n", __func__);
		return NULL;
	}

	INIT_LIST_HEAD(&iommu_entry->dev_list);
	iommu_entry->iommu = iommu;
	iommu_entry->id = domain_id_alloc(iommu);
	if (iommu_entry->id == -1) {
		pr_info("%s alloc id for domain failed\n", __func__);
		kfree(iommu_entry);
		return NULL;
	}

	shd_entry = iommu_zalloc_page(iommu);
	if (!shd_entry) {
		pr_info("%s alloc shadow page entry err\n", __func__);
		domain_id_free(iommu, iommu_entry->id);
		kfree(iommu_entry);
		return NULL;
	}

	iommu_entry->shadow_pgd = shd_entry;
	dir_ctrl = (IOMMU_LEVEL_STRIDE << 26) | (IOMMU_LEVEL_SHIFT(2) << 20);
	dir_ctrl |= (IOMMU_LEVEL_STRIDE <<  16) | (IOMMU_LEVEL_SHIFT(1) << 10);
	dir_ctrl |= (IOMMU_LEVEL_STRIDE << 6) | IOMMU_LEVEL_SHIFT(0);
	pgd_pa = iommu_pgt_v2p(iommu, shd_entry->gmem_ptable);
	pgd_hi = pgd_pa >> 32;
	pgd_lo = pgd_pa & 0xffffffff;
	iommu_write_regl(iommu, LA_IOMMU_DIR_CTRL(iommu_entry->id), dir_ctrl);
	iommu_write_regl(iommu, LA_IOMMU_PGD_HI(iommu_entry->id), pgd_hi);
	iommu_write_regl(iommu, LA_IOMMU_PGD_LO(iommu_entry->id), pgd_lo);

	spin_lock(&priv->lock);
	list_add(&iommu_entry->list, &priv->iommu_devlist);
	spin_unlock(&priv->lock);

	add_domain_to_list(iommu, priv);
	pr_info("%s iommu_entry->iommu %lx id %x\n", __func__,
	       (unsigned long)iommu_entry->iommu, iommu_entry->id);

	return iommu_entry;
}

static struct loongson_iommu_dev_data *iommu_get_devdata(dom_info *info,
		loongson_iommu *iommu, unsigned long bdf)
{
	struct iommu_info *entry;
	struct loongson_iommu_dev_data *dev_data;

	entry = get_iommu_entry(info, iommu);
	if (!entry)
		return NULL;

	/* Find from priv list */
	spin_lock(&entry->devlock);
	list_for_each_entry(dev_data, &entry->dev_list, list) {
		if (dev_data->bdf == bdf) {
			spin_unlock(&entry->devlock);
			return dev_data;
		}
	}

	spin_unlock(&entry->devlock);
	return NULL;
}

static int loongson_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	unsigned short bdf;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus *bus = pdev->bus;
	unsigned char busnum = pdev->bus->number;
	struct loongson_iommu_dev_data *dev_data;
	dom_info *priv = to_dom_info(domain);
	loongson_iommu *iommu;
	iommu_info *iommu_entry = NULL;

	bdf = pdev->devfn & 0xff;
	if (busnum != 0) {
		while (bus->parent->parent)
			bus = bus->parent;
		bdf = bus->self->devfn & 0xff;
	}

	dev_data = (struct loongson_iommu_dev_data *)dev_iommu_priv_get(dev);
	if (dev_data == NULL) {
		pci_info(pdev, "%s dev_data is Invalid\n", __func__);
		return 0;
	}

	iommu = dev_data->iommu;
	if (iommu == NULL) {
		pci_info(pdev, "%s iommu is Invalid\n", __func__);
		return 0;
	}

	pci_info(pdev, "%s busnum %x bdf %x priv %lx iommu %lx\n", __func__,
			busnum, bdf, (unsigned long)priv, (unsigned long)iommu);
	dev_data = iommu_get_devdata(priv, iommu, bdf);
	if (dev_data) {
		dev_data->count++;
		pci_info(pdev, "Loongson-IOMMU: bdf 0x%x devfn %x has attached,"
				" count:0x%x\n",
			bdf, pdev->devfn, dev_data->count);
		return 0;
	} else {
		dev_data = (struct loongson_iommu_dev_data *)dev_iommu_priv_get(dev);
	}

	iommu_entry = domain_attach_iommu(priv, iommu);
	if (iommu_entry == NULL) {
		pci_info(pdev, "domain attach iommu failed\n");
		return 0;
	}

	dev_data->count++;
	do_attach(iommu_entry, dev_data);

	return 0;
}

static unsigned long *iommu_get_spte(spt_entry *entry, unsigned long iova, int level)
{
	int i;
	unsigned long *pte;

	if (level > (IOMMU_LEVEL_MAX - 1))
		return NULL;

	for (i = IOMMU_LEVEL_MAX - 1; i >= level; i--) {
		pte  = iommu_shadow_offset(entry, iova, i);
		if (!iommu_pt_present(pte))
			break;

		if (iommu_pt_huge(pte))
			break;

		entry = (spt_entry *)(*pte);
	}

	return pte;
}

static int _iommu_alloc_ptable(loongson_iommu *iommu,
		unsigned long *psentry, unsigned long *phwentry)
{
	unsigned long pte;
	iommu_pte *new_phwentry;
	spt_entry *new_shd_entry;

	if (!iommu_pt_present(psentry)) {
		new_shd_entry = iommu_zalloc_page(iommu);
		if (!new_shd_entry) {
			pr_err("Loongson-IOMMU: new_shd_entry alloc err\n");
			return -ENOMEM;
		}
		/* fill shd_entry */
		*psentry = (unsigned long)new_shd_entry;
		/* fill gmem phwentry */
		new_phwentry = (iommu_pte *)new_shd_entry->gmem_ptable;
		pte = iommu_pgt_v2p(iommu, new_phwentry) & IOMMU_PAGE_MASK;
		pte |= IOMMU_PTE_RW;
		*phwentry = pte;
	}

	return 0;
}

static size_t iommu_ptw_map(loongson_iommu *iommu, spt_entry *shd_entry,
		unsigned long start, unsigned long end, phys_addr_t pa, int level)
{
	int ret, huge;
	unsigned long pte;
	unsigned long next, old, step;
	unsigned long *psentry, *phwentry;

	old = start;
	psentry = iommu_shadow_offset(shd_entry, start, level);
	phwentry = iommu_ptable_offset(shd_entry->gmem_ptable, start, level);
	if (level == IOMMU_PT_LEVEL0) {
		pa = pa & IOMMU_PAGE_MASK;
		do {
			pte =  pa | IOMMU_PTE_RW;
			*phwentry = pte;
			*psentry = pte;
			psentry++;
			phwentry++;
			start += IOMMU_PAGE_SIZE;
			pa += IOMMU_PAGE_SIZE;
		} while (start < end);

		return start - old;
	}

	do {
		next = iommu_ptable_end(start, end, level);
		step = next - start;

		huge = 0;
		if ((level == IOMMU_PT_LEVEL1) && (step == IOMMU_HPAGE_SIZE))
			if (!iommu_pt_present(psentry) || iommu_pt_huge(psentry))
				huge = 1;

		if (huge) {
			pte =  (pa & IOMMU_HPAGE_MASK) | IOMMU_PTE_RW | IOMMU_PTE_HP;
			*phwentry = pte;
			*psentry = pte;
		} else {
			ret = _iommu_alloc_ptable(iommu, psentry, phwentry);
			if (ret != 0)
				break;
			iommu_ptw_map(iommu, (spt_entry *)*psentry, start, next, pa, level - 1);
		}

		psentry++;
		phwentry++;
		pa += step;
		start = next;
	} while (start < end);

	return start - old;
}

static int dev_map_page(iommu_info *iommu_entry, unsigned long start,
			phys_addr_t pa, size_t size)
{
	int ret = 0;
	spt_entry *entry;
	phys_addr_t end;
	size_t map_size;
	loongson_iommu *iommu;

	end = start + size;
	iommu = iommu_entry->iommu;

	mutex_lock(&iommu->loongson_iommu_pgtlock);
	entry = iommu_entry->shadow_pgd;
	map_size = iommu_ptw_map(iommu, entry, start, end, pa, IOMMU_LEVEL_MAX - 1);
	if (map_size != size)
		ret = -EFAULT;

	if (has_dom(iommu))
		__iommu_flush_iotlb_all(iommu);
	mutex_unlock(&iommu->loongson_iommu_pgtlock);

	return ret;
}

static size_t iommu_ptw_unmap(loongson_iommu *iommu, spt_entry *shd_entry,
		unsigned long start, unsigned long end, int level)
{
	unsigned long next, old;
	unsigned long *psentry, *phwentry;

	old = start;
	psentry = iommu_shadow_offset(shd_entry, start, level);
	phwentry = iommu_ptable_offset(shd_entry->gmem_ptable, start, level);
	if (level == IOMMU_PT_LEVEL0) {
		do {
			*phwentry++ = 0;
			*psentry++ = 0;
			start += IOMMU_PAGE_SIZE;
		} while (start < end);
	} else {
		do {
			next = iommu_ptable_end(start, end, level);
			if (!iommu_pt_present(psentry))
				continue;

			if (iommu_pt_huge(psentry)) {
				if ((next - start) != IOMMU_HPAGE_SIZE)
					pr_err("Map pte on hugepage not supported now\n");
				*phwentry = 0;
				*psentry = 0;
			} else
				iommu_ptw_unmap(iommu, (spt_entry *)*psentry, start, next, level - 1);
		} while (psentry++, phwentry++, start = next, start < end);
	}

	return start - old;
}

static int iommu_map_page(dom_info *priv, unsigned long start,
			phys_addr_t pa, size_t size, int prot, gfp_t gfp)
{

	unsigned long *pte;
	int ret = 0;
	iommu_info *iommu_entry = NULL;

	/* 0x10000000~0x8fffffff */
	if ((start >= IOVA_START) && (start < IOVA_END0)) {
		start -= IOVA_START;
		pte = (unsigned long *)priv->mmio_pgd;
		while (size > 0) {
			pte[start >> LA_VIRTIO_PAGE_SHIFT] =
					pa & LA_VIRTIO_PAGE_MASK;
			size -= IOMMU_PAGE_SIZE;
			start += IOMMU_PAGE_SIZE;
			pa += IOMMU_PAGE_SIZE;
		}
		return 0;
	}

	spin_lock(&priv->lock);
	list_for_each_entry(iommu_entry, &priv->iommu_devlist, list) {
		ret |= dev_map_page(iommu_entry, start, pa, size);
	}
	spin_unlock(&priv->lock);

	return ret;
}

static size_t iommu_unmap_page(iommu_info *iommu_entry, unsigned long start, size_t size)
{
	loongson_iommu *iommu;
	spt_entry *entry;
	size_t unmap_len;
	unsigned long end;

	end = start + size;
	iommu = iommu_entry->iommu;
	mutex_lock(&iommu->loongson_iommu_pgtlock);
	entry = iommu_entry->shadow_pgd;
	unmap_len = iommu_ptw_unmap(iommu, entry, start, end, (IOMMU_LEVEL_MAX - 1));

	if (has_dom(iommu))
		__iommu_flush_iotlb_all(iommu);
	mutex_unlock(&iommu->loongson_iommu_pgtlock);
	return unmap_len;
}

static size_t domain_unmap_page(dom_info *info, unsigned long start, size_t size)
{
	unsigned long *pte;
	size_t unmap_len = 0;
	iommu_info *entry;

	/* 0x10000000~0x8fffffff */
	if ((start >= IOVA_START) && (start < IOVA_END0)) {
		start -= IOVA_START;
		pte = (unsigned long *)info->mmio_pgd;
		while (size > 0) {
			pte[start >> LA_VIRTIO_PAGE_SHIFT] = 0;
			size -= 0x4000;
			unmap_len += 0x4000;
			start += 0x4000;
		}
		unmap_len += size;

		return unmap_len;
	}

	spin_lock(&info->lock);
	list_for_each_entry(entry, &info->iommu_devlist, list)
		unmap_len = iommu_unmap_page(entry, start, size);
	spin_unlock(&info->lock);

	return unmap_len;
}

static int loongson_iommu_map(struct iommu_domain *domain, unsigned long iova,
			      phys_addr_t pa, size_t len, size_t count, int prot, gfp_t gfp, size_t *mapped)
{
	int ret;
	dom_info *priv = to_dom_info(domain);

	ret = iommu_map_page(priv, iova, pa, len, prot, GFP_KERNEL);
	if (ret == 0)
		*mapped = len;

	return ret;
}

static size_t loongson_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
				   size_t size, size_t count, struct iommu_iotlb_gather *gather)
{
	dom_info *priv = to_dom_info(domain);

	return domain_unmap_page(priv, iova, size);
}

static phys_addr_t loongson_iommu_iova_to_pa(struct iommu_domain *domain,
						dma_addr_t iova)
{
	unsigned long pa, offset, tmpva, page_size, page_mask;
	dom_info *priv = to_dom_info(domain);
	unsigned long *psentry, *pte;
	int ret = 0;
	spt_entry *entry;
	loongson_iommu *iommu;
	iommu_info *iommu_entry = NULL;

	/* 0x10000000~0x8fffffff */
	if ((iova >= IOVA_START) && (iova < IOVA_END0)) {
		tmpva = iova & LA_VIRTIO_PAGE_MASK;
		pte = (unsigned long *)priv->mmio_pgd;
		offset = iova & ((1ULL << LA_VIRTIO_PAGE_SHIFT) - 1);
		pa = pte[(tmpva - IOVA_START) >> 14] + offset;

		return pa;
	}

	iommu_entry = get_first_iommu_entry(priv);
	if (iommu_entry == NULL) {
		pr_err("%s iova:0x%llx iommu_entry is invalid\n",
				__func__, iova);
		ret = -EFAULT;
		return ret;
	}

	iommu = iommu_entry->iommu;

	mutex_lock(&iommu->loongson_iommu_pgtlock);
	entry = iommu_entry->shadow_pgd;
	psentry = iommu_get_spte(entry, iova, IOMMU_PT_LEVEL0);
	mutex_unlock(&iommu->loongson_iommu_pgtlock);

	if (!psentry || !iommu_pt_present(psentry)) {
		ret = -EFAULT;
		pr_warn_once("Loongson-IOMMU: shadow pte is null or not present with iova %llx \n", iova);
		return ret;
	}

	if (iommu_pt_huge(psentry)) {
		page_size = IOMMU_HPAGE_SIZE;
		page_mask = IOMMU_HPAGE_MASK;
	} else {
		page_size = IOMMU_PAGE_SIZE;
		page_mask = IOMMU_PAGE_MASK;
	}

	pa = *psentry & page_mask;
	pa |= (iova & (page_size - 1));

	return (phys_addr_t)pa;
}

static phys_addr_t loongson_iommu_iova_to_phys(struct iommu_domain *domain,
					dma_addr_t iova)
{
	phys_addr_t pa;

	pa = loongson_iommu_iova_to_pa(domain, iova);

	return pa;
}

static void loongson_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	int ret;
	dom_info *priv = to_dom_info(domain);
	iommu_info *iommu_entry;
	loongson_iommu *iommu;

	spin_lock(&priv->lock);
	list_for_each_entry(iommu_entry, &priv->iommu_devlist, list) {
		iommu = iommu_entry->iommu;

		ret = __iommu_flush_iotlb_all(iommu);
	}
	spin_unlock(&priv->lock);
}

static void loongson_iommu_iotlb_sync(struct iommu_domain *domain,
				      struct iommu_iotlb_gather *gather)
{
	loongson_iommu_flush_iotlb_all(domain);
}

static struct iommu_ops loongson_iommu_ops = {
	.capable = loongson_iommu_capable,
	.domain_alloc = loongson_iommu_domain_alloc,
	.probe_device = loongson_iommu_probe_device,
	.release_device = loongson_iommu_release_device,
	.device_group = loongson_iommu_device_group,
	.pgsize_bitmap = LA_IOMMU_PGSIZE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= loongson_iommu_attach_dev,
		.free		= loongson_iommu_domain_free,
		.map_pages	= loongson_iommu_map,
		.unmap_pages	= loongson_iommu_unmap,
		.iova_to_phys	= loongson_iommu_iova_to_phys,
		.iotlb_sync	= loongson_iommu_iotlb_sync,
		.flush_iotlb_all = loongson_iommu_flush_iotlb_all,
	}
};

loongson_iommu *loongarch_get_iommu(struct pci_dev *pdev)
{
	int pcisegment;
	unsigned short devid;
	loongson_iommu *iommu = NULL;

	devid = pdev->devfn & 0xff;
	pcisegment = pci_domain_nr(pdev->bus);

	list_for_each_entry(iommu, &loongson_iommu_list, list) {
		if ((iommu->segment == pcisegment) &&
		    (iommu->devid == devid)) {
			return iommu;
		}
	}

	return NULL;
}

static int loongson_iommu_probe(struct pci_dev *pdev,
				const struct pci_device_id *ent)
{
	int ret = 1;
	int bitmap_sz = 0;
	int tmp;
	struct loongson_iommu *iommu = NULL;
	resource_size_t base, size;

	iommu = loongarch_get_iommu(pdev);
	if (iommu == NULL) {
		pci_info(pdev, "%s can't find iommu\n", __func__);
		return -ENODEV;
	}

	base = pci_resource_start(pdev, 0);
	size = pci_resource_len(pdev, 0);
	if (!request_mem_region(base, size, "loongson_iommu")) {
		pci_err(pdev, "can't reserve mmio registers\n");
		return -ENOMEM;
	}

	iommu->membase = ioremap(base, size);
	if (iommu->membase == NULL) {
		pci_info(pdev, "%s iommu pci dev bar0 is NULL\n", __func__);
		return ret;
	}

	base = pci_resource_start(pdev, 2);
	size = pci_resource_len(pdev, 2);
	if (!request_mem_region(base, size, "loongson_iommu")) {
		pci_err(pdev, "can't reserve mmio registers\n");
		return -ENOMEM;
	}
	iommu->pgtbase = ioremap(base, size);
	if (iommu->pgtbase == NULL)
		return -ENOMEM;

	iommu->maxpages = size / IOMMU_PAGE_SIZE;
	pr_info("iommu membase %p pgtbase %p pgtsize %llx maxpages %lx\n", iommu->membase, iommu->pgtbase, size, iommu->maxpages);
	tmp = MAX_DOMAIN_ID / 8;
	bitmap_sz = (MAX_DOMAIN_ID % 8) ? (tmp + 1) : tmp;
	iommu->domain_bitmap = bitmap_zalloc(bitmap_sz, GFP_KERNEL);
	if (iommu->domain_bitmap == NULL) {
		pr_err("Loongson-IOMMU: domain bitmap alloc err bitmap_sz:%d\n", bitmap_sz);
		goto out_err;
	}

	tmp = MAX_ATTACHED_DEV_ID / 8;
	bitmap_sz = (MAX_ATTACHED_DEV_ID % 8) ? (tmp + 1) : tmp;
	iommu->devtable_bitmap = bitmap_zalloc(bitmap_sz, GFP_KERNEL);
	if (iommu->devtable_bitmap == NULL) {
		pr_err("Loongson-IOMMU: devtable bitmap alloc err bitmap_sz:%d\n", bitmap_sz);
		goto out_err_1;
	}

	tmp = iommu->maxpages / 8;
	bitmap_sz = (iommu->maxpages % 8) ? (tmp + 1) : tmp;
	iommu->pgtable_bitmap = bitmap_zalloc(bitmap_sz, GFP_KERNEL);
	if (iommu->pgtable_bitmap == NULL) {
		pr_err("Loongson-IOMMU: pgtable bitmap alloc err bitmap_sz:%d\n", bitmap_sz);
		goto out_err_2;
	}

	return 0;

out_err_2:
	kfree(iommu->devtable_bitmap);
	iommu->devtable_bitmap = NULL;
out_err_1:
	kfree(iommu->domain_bitmap);
	iommu->domain_bitmap = NULL;
out_err:

	return ret;
}

static void loongson_iommu_remove(struct pci_dev *pdev)
{
	struct  loongson_iommu *iommu = NULL;

	iommu = loongarch_get_iommu(pdev);
	if (iommu == NULL)
		return;

	if (iommu->domain_bitmap != NULL) {
		kfree(iommu->domain_bitmap);
		iommu->domain_bitmap = NULL;
	}

	if (iommu->devtable_bitmap != NULL) {
		kfree(iommu->devtable_bitmap);
		iommu->devtable_bitmap = NULL;
	}

	if (iommu->pgtable_bitmap != NULL) {
		kfree(iommu->pgtable_bitmap);
		iommu->pgtable_bitmap = NULL;
	}

	iommu->membase = NULL;
	iommu->pgtbase = NULL;
}

static int __init check_ivrs_checksum(struct acpi_table_header *table)
{
	int i;
	u8 checksum = 0, *p = (u8 *)table;

	for (i = 0; i < table->length; ++i)
		checksum += p[i];
	if (checksum != 0) {
		/* ACPI table corrupt */
		pr_err("IVRS invalid checksum\n");
		return -ENODEV;
	}

	return 0;
}

struct loongson_iommu_rlookup_entry *create_rlookup_entry(int pcisegment)
{
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	rlookupentry = kzalloc(sizeof(struct loongson_iommu_rlookup_entry), GFP_KERNEL);
	if (rlookupentry == NULL)
		return rlookupentry;

	rlookupentry->pcisegment = pcisegment;

	/* IOMMU rlookup table - find the IOMMU for a specific device */
	rlookupentry->loongson_iommu_rlookup_table = (void *)__get_free_pages(
			GFP_KERNEL | __GFP_ZERO, get_order(rlookup_table_size));
	if (rlookupentry->loongson_iommu_rlookup_table == NULL) {
		kfree(rlookupentry);
		rlookupentry = NULL;
	} else {
		list_add(&rlookupentry->list, &loongson_rlookup_iommu_list);
	}

	return rlookupentry;
}

/* Writes the specific IOMMU for a device into the rlookup table */
static void __init set_iommu_for_device(loongson_iommu *iommu,
		u16 devid)
{
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	rlookupentry = lookup_rlooptable(iommu->segment);
	if (rlookupentry == NULL)
		rlookupentry = create_rlookup_entry(iommu->segment);

	if (rlookupentry != NULL)
		rlookupentry->loongson_iommu_rlookup_table[devid] = iommu;
}

static inline u32 get_ivhd_header_size(struct ivhd_header *h)
{
	u32 size = 0;

	switch (h->type) {
	case IVHD_HEAD_TYPE10:
		size = 24;
		break;
	case IVHD_HEAD_TYPE11:
	case IVHD_HEAD_TYPE40:
		size = 40;
		break;
	}
	return size;
}

static inline void update_last_devid(u16 devid)
{
	if (devid > loongson_iommu_last_bdf)
		loongson_iommu_last_bdf = devid;
}

/*
 * This function calculates the length of a given IVHD entry
 */
static inline int ivhd_entry_length(u8 *ivhd)
{
	u32 type = ((struct ivhd_entry *)ivhd)->type;

	if (type < 0x80) {
		return 0x04 << (*ivhd >> 6);
	} else if (type == IVHD_DEV_ACPI_HID) {
		/* For ACPI_HID, offset 21 is uid len */
		return *((u8 *)ivhd + 21) + 22;
	}
	return 0;
}

/*
 * After reading the highest device id from the IOMMU PCI capability header
 * this function looks if there is a higher device id defined in the ACPI table
 */
static int __init find_last_devid_from_ivhd(struct ivhd_header *h)
{
	u8 *p = (void *)h, *end = (void *)h;
	struct ivhd_entry *dev;

	u32 ivhd_size = get_ivhd_header_size(h);

	if (!ivhd_size) {
		pr_err("loongson-iommu: Unsupported IVHD type %#x\n", h->type);
		return -EINVAL;
	}

	p += ivhd_size;
	end += h->length;

	while (p < end) {
		dev = (struct ivhd_entry *)p;
		switch (dev->type) {
		case IVHD_DEV_ALL:
			/* Use maximum BDF value for DEV_ALL */
			update_last_devid(MAX_BDF_NUM);
			break;
		case IVHD_DEV_SELECT:
		case IVHD_DEV_RANGE_END:
		case IVHD_DEV_ALIAS:
		case IVHD_DEV_EXT_SELECT:
			/* all the above subfield types refer to device ids */
			update_last_devid(dev->devid);
			break;
		default:
			break;
		}
		p += ivhd_entry_length(p);
	}

	WARN_ON(p != end);

	return 0;
}

/*
 * Iterate over all IVHD entries in the ACPI table and find the highest device
 * id which we need to handle. This is the first of three functions which parse
 * the ACPI table. So we check the checksum here.
 */
static int __init find_last_devid_acpi(struct acpi_table_header *table)
{
	u8 *p = (u8 *)table, *end = (u8 *)table;
	struct ivhd_header *h;

	p += IVRS_HEADER_LENGTH;

	end += table->length;
	while (p < end) {
		h = (struct ivhd_header *)p;
		if (h->type == loongson_iommu_target_ivhd_type) {
			int ret = find_last_devid_from_ivhd(h);

			if (ret)
				return ret;
		}

		if (h->length == 0)
			break;

		p += h->length;
	}

	if (p != end)
		return -EINVAL;


	return 0;
}

/*
 * Takes a pointer to an loongarch IOMMU entry in the ACPI table and
 * initializes the hardware and our data structures with it.
 */
static int __init init_iommu_from_acpi(loongson_iommu *iommu,
					struct ivhd_header *h)
{
	u8 *p = (u8 *)h;
	u8 *end = p;
	u16 devid = 0, devid_start = 0;
	u32 dev_i, ivhd_size;
	struct ivhd_entry *e;

	/*
	 * Done. Now parse the device entries
	 */
	ivhd_size = get_ivhd_header_size(h);
	if (!ivhd_size) {
		pr_err("loongarch iommu: Unsupported IVHD type %#x\n", h->type);
		return -EINVAL;
	}

	if (h->length == 0)
		return -EINVAL;

	p += ivhd_size;
	end += h->length;

	while (p < end) {
		e = (struct ivhd_entry *)p;
		switch (e->type) {
		case IVHD_DEV_ALL:
			for (dev_i = 0; dev_i <= loongson_iommu_last_bdf; ++dev_i)
				set_iommu_for_device(iommu, dev_i);
			break;

		case IVHD_DEV_SELECT:
			pr_info("  DEV_SELECT\t\t\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid = e->devid;
			set_iommu_for_device(iommu, devid);
			break;

		case IVHD_DEV_SELECT_RANGE_START:
			pr_info("  DEV_SELECT_RANGE_START\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid_start = e->devid;
			break;

		case IVHD_DEV_RANGE_END:
			pr_info("  DEV_RANGE_END\t\t devid: %02x:%02x.%x\n",
				PCI_BUS_NUM(e->devid), PCI_SLOT(e->devid), PCI_FUNC(e->devid));

			devid = e->devid;
			for (dev_i = devid_start; dev_i <= devid; ++dev_i)
				set_iommu_for_device(iommu, dev_i);
			break;

		default:
			break;
		}

		p += ivhd_entry_length(p);
	}

	return 0;
}

/*
 * This function clues the initialization function for one IOMMU
 * together and also allocates the command buffer and programs the
 * hardware. It does NOT enable the IOMMU. This is done afterwards.
 */
static int __init init_iommu_one(loongson_iommu *iommu, struct ivhd_header *h)
{
	int ret;
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	spin_lock_init(&iommu->domain_bitmap_lock);
	spin_lock_init(&iommu->dom_info_lock);
	spin_lock_init(&iommu->pgtable_bitmap_lock);
	mutex_init(&iommu->loongson_iommu_pgtlock);

	/* Add IOMMU to internal data structures */
	INIT_LIST_HEAD(&iommu->dom_list);

	list_add_tail(&iommu->list, &loongson_iommu_list);

	/*
	 * Copy data from ACPI table entry to the iommu struct
	 */
	iommu->devid   = h->devid;
	iommu->segment = h->pci_seg;

	ret = init_iommu_from_acpi(iommu, h);
	if (ret) {
		pr_err("%s init iommu from acpi failed\n", __func__);
		return ret;
	}

	rlookupentry = lookup_rlooptable(iommu->segment);
	if (rlookupentry != NULL) {
		/*
		 * Make sure IOMMU is not considered to translate itself.
		 * The IVRS table tells us so, but this is a lie!
		 */
		rlookupentry->loongson_iommu_rlookup_table[iommu->devid] = NULL;
	}

	return 0;
}

/*
 * Iterates over all IOMMU entries in the ACPI table, allocates the
 * IOMMU structure and initializes it with init_iommu_one()
 */
static int __init init_iommu_all(struct acpi_table_header *table)
{
	int ret;
	u8 *p = (u8 *)table, *end = (u8 *)table;
	struct ivhd_header *h;
	loongson_iommu *iommu;

	end += table->length;
	p += IVRS_HEADER_LENGTH;

	while (p < end) {
		h = (struct ivhd_header *)p;

		if (h->length == 0)
			break;

		if (*p == loongson_iommu_target_ivhd_type) {
			pr_info("device: %02x:%02x.%01x seg: %d\n",
				PCI_BUS_NUM(h->devid), PCI_SLOT(h->devid), PCI_FUNC(h->devid), h->pci_seg);

			iommu = kzalloc(sizeof(loongson_iommu), GFP_KERNEL);
			if (iommu == NULL) {
				pr_info("%s alloc memory for iommu failed\n", __func__);
				return -ENOMEM;
			}

			ret = init_iommu_one(iommu, h);
			if (ret) {
				kfree(iommu);
				pr_info("%s init iommu failed\n", __func__);
				return ret;
			}
		}
		p += h->length;
	}

	if (p != end)
		return -EINVAL;

	return 0;
}

/**
 * get_highest_supported_ivhd_type - Look up the appropriate IVHD type
 * @ivrs          Pointer to the IVRS header
 *
 * This function search through all IVDB of the maximum supported IVHD
 */
static u8 get_highest_supported_ivhd_type(struct acpi_table_header *ivrs)
{
	u8 *base = (u8 *)ivrs;
	struct ivhd_header *ivhd = (struct ivhd_header *)(base + IVRS_HEADER_LENGTH);
	u8 last_type = ivhd->type;
	u16 devid = ivhd->devid;

	while (((u8 *)ivhd - base < ivrs->length) &&
	       (ivhd->type <= ACPI_IVHD_TYPE_MAX_SUPPORTED) &&
	       (ivhd->length > 0)) {
		u8 *p = (u8 *) ivhd;

		if (ivhd->devid == devid)
			last_type = ivhd->type;
		ivhd = (struct ivhd_header *)(p + ivhd->length);
	}

	return last_type;
}

static inline unsigned long tbl_size(int entry_size)
{
	unsigned int shift = PAGE_SHIFT +
			 get_order(((int)loongson_iommu_last_bdf + 1) * entry_size);

	return 1UL << shift;
}

static int __init loongson_iommu_ivrs_init(void)
{
	int ret = 0;
	acpi_status status;
	struct acpi_table_header *ivrs_base;

	status = acpi_get_table("IVRS", 0, &ivrs_base);
	if (status == AE_NOT_FOUND) {
		pr_info("%s get ivrs table failed\n", __func__);
		return -ENODEV;
	}

	/*
	 * Validate checksum here so we don't need to do it when
	 * we actually parse the table
	 */
	ret = check_ivrs_checksum(ivrs_base);
	if (ret)
		goto out;

	loongson_iommu_target_ivhd_type = get_highest_supported_ivhd_type(ivrs_base);
	pr_info("Using IVHD type %#x\n", loongson_iommu_target_ivhd_type);

	/*
	 * First parse ACPI tables to find the largest Bus/Dev/Func
	 * we need to handle. Upon this information the shared data
	 * structures for the IOMMUs in the system will be allocated
	 */
	ret = find_last_devid_acpi(ivrs_base);
	if (ret) {
		pr_err("%s find last devid failed\n", __func__);
		goto out;
	}

	rlookup_table_size = tbl_size(RLOOKUP_TABLE_ENTRY_SIZE);

	/*
	 * now the data structures are allocated and basically initialized
	 * start the real acpi table scan
	 */
	ret = init_iommu_all(ivrs_base);

out:
	/* Don't leak any ACPI memory */
	acpi_put_table(ivrs_base);
	ivrs_base = NULL;

	return ret;
}

static int __init loongson_iommu_ivrs_init_stub(void)
{
	u32 dev_i;
	loongson_iommu *iommu;
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	/* Use maximum BDF value for DEV_ALL */
	update_last_devid(MAX_BDF_NUM);

	rlookup_table_size = tbl_size(RLOOKUP_TABLE_ENTRY_SIZE);

	iommu = kzalloc(sizeof(loongson_iommu), GFP_KERNEL);
	if (iommu == NULL) {
		pr_info("%s alloc memory for iommu failed\n", __func__);
		return -ENOMEM;
	}

	spin_lock_init(&iommu->domain_bitmap_lock);
	spin_lock_init(&iommu->dom_info_lock);
	spin_lock_init(&iommu->pgtable_bitmap_lock);
	mutex_init(&iommu->loongson_iommu_pgtlock);

	/* Add IOMMU to internal data structures */
	INIT_LIST_HEAD(&iommu->dom_list);

	list_add_tail(&iommu->list, &loongson_iommu_list);

	/*
	 * Copy data from ACPI table entry to the iommu struct
	 */
	iommu->devid = 0xd0;
	iommu->segment = 0;

	for (dev_i = 0; dev_i <= loongson_iommu_last_bdf; ++dev_i)
		set_iommu_for_device(iommu, dev_i);

	rlookupentry = lookup_rlooptable(iommu->segment);
	if (rlookupentry != NULL) {
		/*
		 * Make sure IOMMU is not considered to translate itself.
		 * The IVRS table tells us so, but this is a lie!
		 */
		rlookupentry->loongson_iommu_rlookup_table[iommu->devid] = NULL;
	}

	return 0;
}

static void free_iommu_rlookup_entry(void)
{
	loongson_iommu *iommu = NULL;
	struct loongson_iommu_rlookup_entry *rlookupentry = NULL;

	while (!list_empty(&loongson_iommu_list)) {
		iommu = list_first_entry(&loongson_iommu_list, loongson_iommu, list);
		list_del(&iommu->list);
		kfree(iommu);
	}

	while (!list_empty(&loongson_rlookup_iommu_list)) {
		rlookupentry = list_first_entry(&loongson_rlookup_iommu_list,
				struct loongson_iommu_rlookup_entry, list);

		list_del(&rlookupentry->list);
		if (rlookupentry->loongson_iommu_rlookup_table != NULL) {
			free_pages(
			(unsigned long)rlookupentry->loongson_iommu_rlookup_table,
			get_order(rlookup_table_size));

			rlookupentry->loongson_iommu_rlookup_table = NULL;
		}

		kfree(rlookupentry);
	}
}

static int __init loonson_iommu_setup(char *str)
{
	if (!str)
		return -EINVAL;

	while (*str) {
		if (!strncmp(str, "on", 2)) {
			loongson_iommu_disable = 0;
			pr_info("IOMMU enabled\n");
		} else if (!strncmp(str, "off", 3)) {
			loongson_iommu_disable = 1;
			pr_info("IOMMU disabled\n");
		}
		str += strcspn(str, ",");
		while (*str == ',')
			str++;
	}
	return 0;
}
__setup("loongson_iommu=", loonson_iommu_setup);

static const struct pci_device_id loongson_iommu_pci_tbl[] = {
	{ PCI_DEVICE(0x14, 0x7a1f) },
	{ 0, }
};

static struct pci_driver loongson_iommu_driver = {
	.name = "loongson-iommu",
	.probe	= loongson_iommu_probe,
	.remove	= loongson_iommu_remove,
	.id_table = loongson_iommu_pci_tbl,
};

static int __init loongson_iommu_driver_init(void)
{
	int ret = 0;

	if (!loongson_iommu_disable) {
		ret = loongson_iommu_ivrs_init();
		if (ret < 0) {
			free_iommu_rlookup_entry();
			pr_err("Failed to init iommu by ivrs\n");
			ret = loongson_iommu_ivrs_init_stub();
			if (ret < 0) {
				free_iommu_rlookup_entry();
				pr_err("Failed to init iommu by stub\n");
				return ret;
			}
		}

		ret = pci_register_driver(&loongson_iommu_driver);
		if (ret < 0) {
			pr_err("Failed to register IOMMU driver\n");
			return ret;
		}
	}

	return ret;
}

static void __exit loongson_iommu_driver_exit(void)
{
	if (!loongson_iommu_disable) {
		free_iommu_rlookup_entry();
		pci_unregister_driver(&loongson_iommu_driver);
	}
}

module_init(loongson_iommu_driver_init);
module_exit(loongson_iommu_driver_exit);
