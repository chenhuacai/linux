/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "kcd_priv.h"
#include "kcd_mqd_manager.h"
#include "loonggpu_lgkcd.h"
#include "kcd_mqd_manager_lg2xx.h"

static inline struct lg2xx_mqd *get_mqd(void *mqd)
{
	return (struct lg2xx_mqd *)mqd;
}

static struct kcd_mem_obj *allocate_mqd(struct kcd_node *node,
		struct queue_properties *q)
{
	struct kcd_mem_obj *mqd_mem_obj;
	int size = sizeof(struct lg2xx_mqd);

	if (kcd_gtt_sa_allocate(node, size, &mqd_mem_obj))
		return NULL;

	return mqd_mem_obj;
}

static void init_mqd(struct mqd_manager *mm, void **mqd,
			struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
			struct queue_properties *q,
			uint32_t pasid)
{
	uint64_t addr;
	struct lg2xx_mqd *m;
	int size;

	m = (struct lg2xx_mqd *) mqd_mem_obj->cpu_ptr;
	addr = mqd_mem_obj->gpu_addr;

	size = sizeof(struct lg2xx_mqd);

	memset(m, 0, size);

	/* Init mqd ... */
	m->pasid = pasid;

	*mqd = m;
	if (gart_addr)
		*gart_addr = addr;
	mm->update_mqd(mm, m, q);
}

static void update_mqd(struct mqd_manager *mm, void *mqd,
		       struct queue_properties *q)
{
	struct lg2xx_mqd *m;

	m = get_mqd(mqd);

	/* update mqd ... */
	m->length = q->queue_size / 4;
	m->base_addr_lo	= lower_32_bits((uint64_t)q->queue_address);
	m->base_addr_hi	= upper_32_bits((uint64_t)q->queue_address);
	m->rd_ptr_lo = lower_32_bits((uint64_t)q->read_ptr);
	m->rd_ptr_hi = upper_32_bits((uint64_t)q->read_ptr);
	m->wr_ptr_lo = lower_32_bits((uint64_t)q->write_ptr);
	m->wr_ptr_hi = upper_32_bits((uint64_t)q->write_ptr);
	q->is_active = QUEUE_IS_ACTIVE(*q);
	m->queue_id = q->queue_id;
	m->queue_type = q->type | (q->is_active ? 0x10 : 0x00);
	m->doorbell_offset = q->doorbell_off;
	m->cwsr_lo = lower_32_bits((uint64_t)q->ctx_save_restore_area_address);
	m->cwsr_hi = upper_32_bits((uint64_t)q->ctx_save_restore_area_address);
	m->cwsr_size = q->ctx_save_restore_area_size;
}

static void restore_mqd(struct mqd_manager *mm, void **mqd,
			struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
			struct queue_properties *qp,
			const void *mqd_src)
{
	uint64_t addr;
	struct lg2xx_mqd *m;

	m = (struct lg2xx_mqd *) mqd_mem_obj->cpu_ptr;
	addr = mqd_mem_obj->gpu_addr;

	memcpy(m, mqd_src, sizeof(*m));

	*mqd = m;
	if (gart_addr)
		*gart_addr = addr;

	qp->is_active = 0;
}


static void init_mqd_hiq(struct mqd_manager *mm, void **mqd,
			struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
			struct queue_properties *q,
			uint32_t pasid)
{
	struct lg2xx_mqd *m;

	init_mqd(mm, mqd, mqd_mem_obj, gart_addr, q, pasid);

	m = get_mqd(*mqd);

}

static void init_mqd_sdma(struct mqd_manager *mm, void **mqd,
		struct kcd_mem_obj *mqd_mem_obj, uint64_t *gart_addr,
		struct queue_properties *q,
		uint32_t pasid)
{
	struct lg2xx_mqd *m;
	int size;

	m = (struct lg2xx_mqd *) mqd_mem_obj->cpu_ptr;

	size = sizeof(struct lg2xx_mqd);

	memset(m, 0, size);

	m->pasid = pasid;
	*mqd = m;
	if (gart_addr)
		*gart_addr = mqd_mem_obj->gpu_addr;

	mm->update_mqd(mm, m, q);
}

#if defined(CONFIG_DEBUG_FS)

static int debugfs_show_mqd(struct seq_file *m, void *data)
{
	seq_hex_dump(m, "    ", DUMP_PREFIX_OFFSET, 32, 4,
		     data, sizeof(struct lg2xx_mqd), false);
	return 0;
}

static int debugfs_show_mqd_sdma(struct seq_file *m, void *data)
{
	seq_hex_dump(m, "    ", DUMP_PREFIX_OFFSET, 32, 4,
		     data, sizeof(struct lg2xx_mqd), false);
	return 0;
}

#endif

static uint32_t read_doorbell_id(void *mqd)
{
	return 0;
}

struct mqd_manager *mqd_manager_init_lg2xx(enum KCD_MQD_TYPE type,
		struct kcd_node *dev)
{
	struct mqd_manager *mqd;

	if (WARN_ON(type >= KCD_MQD_TYPE_MAX))
		return NULL;

	mqd = kzalloc(sizeof(*mqd), GFP_KERNEL);
	if (!mqd)
		return NULL;

	mqd->dev = dev;

	switch (type) {
	case KCD_MQD_TYPE_CP:
		pr_debug("%s@%i\n", __func__, __LINE__);
		mqd->allocate_mqd = allocate_mqd;
		mqd->init_mqd = init_mqd;
		mqd->free_mqd = kcd_free_mqd_cp;
		mqd->update_mqd = update_mqd;
		mqd->mqd_size = sizeof(struct lg2xx_mqd);
		mqd->mqd_stride = kcd_mqd_stride;
		mqd->restore_mqd = restore_mqd;
#if defined(CONFIG_DEBUG_FS)
		mqd->debugfs_show_mqd = debugfs_show_mqd;
#endif
		pr_debug("%s@%i\n", __func__, __LINE__);
		break;
	case KCD_MQD_TYPE_HIQ:
		pr_debug("%s@%i\n", __func__, __LINE__);
		mqd->allocate_mqd = allocate_hiq_mqd;
		mqd->init_mqd = init_mqd_hiq;
		mqd->free_mqd = free_mqd_hiq_sdma;
		mqd->load_mqd = kcd_hiq_load_mqd_kiq;
		mqd->update_mqd = update_mqd;
		mqd->destroy_mqd = kcd_hiq_destroy_mqd_kiq;
		mqd->mqd_size = sizeof(struct lg2xx_mqd);
		mqd->mqd_stride = kcd_mqd_stride;
#if defined(CONFIG_DEBUG_FS)
		mqd->debugfs_show_mqd = debugfs_show_mqd;
#endif
		mqd->read_doorbell_id = read_doorbell_id;
		pr_debug("%s@%i\n", __func__, __LINE__);
		break;
	case KCD_MQD_TYPE_SDMA:
		pr_debug("%s@%i\n", __func__, __LINE__);
		mqd->allocate_mqd = allocate_sdma_mqd;
		mqd->init_mqd = init_mqd_sdma;
		mqd->free_mqd = free_mqd_hiq_sdma;
		mqd->update_mqd = update_mqd;
		mqd->restore_mqd = restore_mqd;
		mqd->mqd_size = sizeof(struct lg2xx_mqd);
		mqd->mqd_stride = kcd_mqd_stride;
#if defined(CONFIG_DEBUG_FS)
		mqd->debugfs_show_mqd = debugfs_show_mqd_sdma;
#endif
		pr_debug("%s@%i\n", __func__, __LINE__);
		break;
	default:
		kfree(mqd);
		return NULL;
	}

	return mqd;
}
