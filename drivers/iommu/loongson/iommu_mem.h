/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Loongson IOMMU Driver
 *
 * Copyright (C) 2026-2027 Loongson Technology Ltd.
 * Author:	Xianglai Li <lixianglai@loongson.cn>
 */

#ifndef LOONGSON_IOMMU_MEM_H
#define LOONGSON_IOMMU_MEM_H

#define iommu_virt_to_phys(address) TO_PHYS((unsigned long)address)
#define iommu_phys_to_virt(address) ((void *)TO_UNCACHE((unsigned long)address))

void *loongson_iommu_alloc_page(void);
void loongson_iommu_free_page(void *page);

#endif
