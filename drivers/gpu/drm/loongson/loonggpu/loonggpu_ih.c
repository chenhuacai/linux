#include <linux/delay.h>
#include "loonggpu.h"
#include "loonggpu_ih.h"
#include "loonggpu_irq.h"
#include "loonggpu_lgkcd.h"

static u32 lg2xx_ih_get_rptr(struct loonggpu_device *adev);

static void loonggpu_ih_set_interrupt_funcs(struct loonggpu_device *adev);

/**
 * loonggpu_ih_enable_interrupts - Enable the interrupt ring buffer
 *
 * @adev: loonggpu_device pointer
 *
 * Enable the interrupt ring buffer  .
 */
static void loonggpu_ih_enable_interrupts(struct loonggpu_device *adev)
{
	adev->irq.ih.enabled = true;
}

/**
 * loonggpu_ih_disable_interrupts - Disable the interrupt ring buffer
 *
 * @adev: loonggpu_device pointer
 *
 * Disable the interrupt ring buffer  .
 */
static void loonggpu_ih_disable_interrupts(struct loonggpu_device *adev)
{
	adev->irq.ih.enabled = false;
	adev->irq.ih.rptr = 0;
}

static uint16_t lg2xx_ih_get_hw_int_flags(struct loonggpu_device *adev)
{
	uint16_t flags;
	int rb_bufsz = adev->irq.ih.ring_size / 4;

	flags = LG2XX_INT_CFG0_VMID;
	flags |= adev->irq.ih.use_bus_addr ? LG2XX_INT_CFG0_UMAP :
			LG2XX_INT_CFG0_MAP;
	switch (rb_bufsz / 4)
	{
	case 256:
		flags |= LG2XX_INT_CFG0_SIZE_256;
		break;
	case 1024:
		flags |= (adev->family_type < CHIP_LG210) ?
			LG2XX_INT_CFG0_SIZE_1024 : LG210_INT_CFG0_SIZE_1024;
		break;
	case 2048:
		flags |= (adev->family_type < CHIP_LG210) ?
			LG2XX_INT_CFG0_SIZE_2048 : LG210_INT_CFG0_SIZE_2048;
		break;
	default:
		DRM_ERROR("%s Illegal rb_bufsz  %d\n", __FUNCTION__, rb_bufsz);
		break;
	}

	flags |= LG2XX_INT_CFG0_ENABLE;
	return flags;
}

static void lg2xx_wait_hw_int_clear(struct loonggpu_device *adev)
{
	uint32_t count = 0;

	while (1) {
		if (!(le32_to_cpu(RREG32(LOONGGPU_HOST_INT)) & LG2XX_INT_CLEAR))
			break;

		udelay(1);
		if (count++ > 1000000)
			DRM_ERROR("%s: timeout waiting for interrupt clear\n", __func__);
	}
}

/**
 * loonggpu_ih_irq_init - init and enable the interrupt ring
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate a ring buffer for the interrupt controller,
 * enable the RLC, disable interrupts, enable the IH
 * ring buffer and enable it  .
 * Called at device load and reume.
 * Returns 0 for success, errors for failure.
 */
static int loonggpu_ih_irq_init(struct loonggpu_device *adev)
{
	int rb_bufsz;
	u64 wptr_off;
	u32 int_reg;
	u16 flags;

	/* disable irqs */
	loonggpu_ih_disable_interrupts(adev);

	rb_bufsz = adev->irq.ih.ring_size / 4;

	if (adev->family_type == CHIP_LG100) {
		wptr_off = adev->irq.ih.use_bus_addr ? adev->irq.ih.rb_dma_addr
				: adev->wb.gpu_addr;
		WREG32(LOONGGPU_INT_CB_SIZE_OFFSET, rb_bufsz);
		WREG32(LOONGGPU_INT_CB_BASE_LO_OFFSET, lower_32_bits(wptr_off));
		WREG32(LOONGGPU_INT_CB_BASE_HI_OFFSET, upper_32_bits(wptr_off));
		/* set rptr, wptr to 0 */
		WREG32(LOONGGPU_INT_CB_WPTR_OFFSET, 0);
		WREG32(LOONGGPU_INT_CB_RPTR_OFFSET, 0);
	} else if (adev->family_type == CHIP_LG200 ||
		   adev->family_type == CHIP_LG210 ||
		   adev->family_type == CHIP_LG211) {
		wptr_off = adev->irq.ih.use_bus_addr ? adev->irq.ih.rb_dma_addr
				: adev->irq.ih.gpu_addr;
		flags = lg2xx_ih_get_hw_int_flags(adev);
		loonggpu_cmd_exec(adev,
			LG2XX_ICMD32i(LG2XX_ICMD32_MOP_EXC,
				LG2XX_ICMD32_SOP_EXC_HBEQ, flags),
			lower_32_bits(wptr_off), upper_32_bits(wptr_off));

		/* clear rptr and wptr */
		int_reg = le32_to_cpu(RREG32(LOONGGPU_HOST_INT));
		WREG32(LOONGGPU_HOST_INT, int_reg | LG2XX_INT_CLEAR);
		lg2xx_wait_hw_int_clear(adev);
		adev->irq.ih.rptr = lg2xx_ih_get_rptr(adev);
	}

	pci_set_master(adev->pdev);

	/* enable interrupts */
	loonggpu_ih_enable_interrupts(adev);

	return 0;
}

/**
 * loonggpu_ih_irq_disable - disable interrupts
 *
 * @adev: loonggpu_device pointer
 *
 * Disable interrupts on the hw  .
 */
static void loonggpu_ih_irq_disable(struct loonggpu_device *adev)
{
	loonggpu_ih_disable_interrupts(adev);

	/* Wait and acknowledge irq */
	mdelay(1);
}

static u32 ih_func_get_wptr(struct loonggpu_device *adev)
{
	u32 wptr;

	if (adev->irq.ih.use_bus_addr)
		wptr = le32_to_cpu(RREG32(LOONGGPU_INT_CB_WPTR_OFFSET));
	else
		wptr = le32_to_cpu(RREG32(LOONGGPU_INT_CB_WPTR_OFFSET));

	return (wptr & adev->irq.ih.ptr_mask);
}

static u32 lg2xx_ih_get_rptr(struct loonggpu_device *adev)
{
	return ((le32_to_cpu(RREG32(LOONGGPU_HOST_INT)) >> 16) & 0xfff) * 4;
}

static void lg2xx_ih_set_rptr(struct loonggpu_device *adev)
{
	u32 int_reg;

	int_reg = RREG32(LOONGGPU_HOST_INT);
	int_reg = (int_reg & 0xfffff000) | adev->irq.ih.rptr >> 2;
	WREG32(LOONGGPU_HOST_INT, int_reg);

	adev->irq.ih.rptr = lg2xx_ih_get_rptr(adev);
}

static u32 lg2xx_ih_func_get_wptr(struct loonggpu_device *adev)
{
	u32 int_reg;
	u32 rptr, wptr;

	int_reg = le32_to_cpu(RREG32(LOONGGPU_HOST_INT));

	if (int_reg & LG2XX_INT_AFULL) {
		rptr = ((int_reg>>16) & 0xfff) * 4;
		wptr = (int_reg & 0xfff) * 4;
		DRM_DEBUG("%s: interrupt overflow, rptr = %d, wptr = %d\n", __func__,rptr, wptr);
	} else
		wptr = (int_reg & 0xfff) * 4;

	return wptr;
}

static bool ih_func_prescreen_iv(struct loonggpu_device *adev)
{
	u32 ring_index = adev->irq.ih.rptr;
	u16 pasid;

	switch (le32_to_cpu(adev->irq.ih.ring[ring_index]) & 0xff) {
	case LOONGGPU_SRCID_GFX_PAGE_INV_FAULT:
	case LOONGGPU_SRCID_GFX_MEM_PROT_FAULT:
		pasid = le32_to_cpu(adev->irq.ih.ring[ring_index + 2]) >> 16;
		if (!pasid || loonggpu_vm_pasid_fault_credit(adev, pasid))
			return true;
		break;
	default:
		return true;
	}

	adev->irq.ih.rptr += 4;
	return false;
}

static bool lg2xx_ih_func_prescreen_iv(struct loonggpu_device *adev)
{
	u32 ring_index = adev->irq.ih.rptr;
	u16 pasid;

	switch (le32_to_cpu(adev->irq.ih.ring[ring_index]) & 0xff) {
	case LOONGGPU_LG200_SRCID_MMU_PAGE_FAULT:
	case LOONGGPU_LG200_SRCID_MMU_PORT_FAULT:
		pasid = le32_to_cpu(adev->irq.ih.ring[ring_index + 2]) >> 16;
		if (!pasid || loonggpu_vm_pasid_fault_credit(adev, pasid))
			return true;
		break;
	default:
		return true;
	}

	adev->irq.ih.rptr += 4;
	return false;
}

static void ih_func_decode_iv(struct loonggpu_device *adev,
				 struct loonggpu_iv_entry *entry)
{
	/* wptr/rptr are in bytes! */
	u32 ring_index = adev->irq.ih.rptr;
	uint32_t dw[4];

	dw[0] = le32_to_cpu(adev->irq.ih.ring[ring_index + 0]);
	dw[1] = le32_to_cpu(adev->irq.ih.ring[ring_index + 1]);
	dw[2] = le32_to_cpu(adev->irq.ih.ring[ring_index + 2]);
	dw[3] = le32_to_cpu(adev->irq.ih.ring[ring_index + 3]);

	DRM_DEBUG("ih_func_decode_iv dw0 %x dw1 %x dw2 %x dw3 %x \n", dw[0], dw[1], dw[2], dw[3]);

	entry->client_id = LOONGGPU_IH_CLIENTID_LEGACY;
	entry->src_id = dw[0] & 0xff;
	entry->src_data[0] = dw[1] & 0xffffffff;
	entry->ring_id = dw[2] & 0xff;
	entry->vmid = (dw[2] >> 8) & 0xff;
	entry->pasid = (dw[2] >> 16) & 0xffff;
	entry->src_data[1] = dw[3];

	/* wptr/rptr are in bytes! */
	adev->irq.ih.rptr += 4;
}

static void lg2xx_ih_func_decode_iv(struct loonggpu_device *adev,
				 struct loonggpu_iv_entry *entry)
{
	/* wptr/rptr are in bytes! */
	u32 ring_index = adev->irq.ih.rptr;
	uint32_t dw[4];

	dw[0] = le32_to_cpu(adev->irq.ih.ring[ring_index + 0]);
	dw[1] = le32_to_cpu(adev->irq.ih.ring[ring_index + 1]);
	dw[2] = le32_to_cpu(adev->irq.ih.ring[ring_index + 2]);
	dw[3] = le32_to_cpu(adev->irq.ih.ring[ring_index + 3]);

	DRM_DEBUG("ih_func_decode_iv dw0 %x dw1 %x dw2 %x dw3 %x \n", dw[0], dw[1], dw[2], dw[3]);

	entry->client_id = LOONGGPU_IH_CLIENTID_LEGACY;
	entry->src_id = dw[0] & 0xff;          // 0 - 7
	entry->pasid = (dw[0] >> 8) & 0xffff;  // 8 - 23
	                                       // 24 - 27 reversed
	entry->vmid = (dw[0] >> 28) & 0xf;     // 28 - 31

	/* dw[1] : Hardware restrictions are not available */

	entry->src_data[0] = dw[2];
	entry->src_data[1] = dw[3];
	entry->ring_id = 0;

	adev->irq.ih.rptr += 4;
}

static void ih_func_set_rptr(struct loonggpu_device *adev)
{
	WREG32(LOONGGPU_INT_CB_RPTR_OFFSET, adev->irq.ih.rptr);
}

static void lg2xx_ih_func_set_rptr(struct loonggpu_device *adev)
{
	lg2xx_ih_set_rptr(adev);
}

static int loonggpu_ih_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_ih_set_interrupt_funcs(adev);

	return 0;
}

static int loonggpu_ih_sw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (adev->family_type == CHIP_LG100)
		r = loonggpu_ih_ring_init(adev, 4 * 1024, true);
	else if (adev->family_type == CHIP_LG200 ||
		 adev->family_type == CHIP_LG210 ||
		 adev->family_type == CHIP_LG211)
		r = loonggpu_ih_ring_init(adev, 8 * 4096, true);
	else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	if (r)
		return r;

	r = loonggpu_irq_init(adev);
	if (r)
		return r;

	r = loonggpu_hw_sema_mgr_init(adev);

	return r;
}

static int loonggpu_ih_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_irq_fini(adev);
	loonggpu_ih_ring_fini(adev);
	loonggpu_hw_sema_mgr_fini(adev);

	return 0;
}

static int loonggpu_ih_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = loonggpu_ih_irq_init(adev);
	if (r)
		return r;

	return 0;
}

static int loonggpu_ih_hw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	loonggpu_ih_irq_disable(adev);

	return 0;
}

static int loonggpu_ih_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return loonggpu_ih_hw_fini(adev);
}

static int loonggpu_ih_resume(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return loonggpu_ih_hw_init(adev);
}

static bool loonggpu_ih_is_idle(void *handle)
{

	return true;
}

static int loonggpu_ih_wait_for_idle(void *handle)
{
	return 0;
}

static const struct loonggpu_ip_funcs loonggpu_ih_ip_funcs = {
	.name = "loonggpu_ih",
	.early_init = loonggpu_ih_early_init,
	.late_init = NULL,
	.sw_init = loonggpu_ih_sw_init,
	.sw_fini = loonggpu_ih_sw_fini,
	.hw_init = loonggpu_ih_hw_init,
	.hw_fini = loonggpu_ih_hw_fini,
	.suspend = loonggpu_ih_suspend,
	.resume = loonggpu_ih_resume,
	.is_idle = loonggpu_ih_is_idle,
	.wait_for_idle = loonggpu_ih_wait_for_idle,
};

static const struct loonggpu_ih_funcs loonggpu_ih_funcs = {
	.get_wptr = ih_func_get_wptr,
	.prescreen_iv = ih_func_prescreen_iv,
	.decode_iv = ih_func_decode_iv,
	.set_rptr = ih_func_set_rptr
};

static const struct loonggpu_ih_funcs loonggpu_ih_lg2xx_funcs = {
	.get_wptr = lg2xx_ih_func_get_wptr,
	.prescreen_iv = lg2xx_ih_func_prescreen_iv,
	.decode_iv = lg2xx_ih_func_decode_iv,
	.set_rptr = lg2xx_ih_func_set_rptr
};

static void loonggpu_ih_set_interrupt_funcs(struct loonggpu_device *adev)
{
	if (adev->irq.ih_funcs != NULL)
		return ;

	switch (adev->family_type)
	{
	case CHIP_LG100:
		adev->irq.ih_funcs = &loonggpu_ih_funcs;
		break;
	case CHIP_LG200:
	case CHIP_LG210:
	case CHIP_LG211:
		adev->irq.ih_funcs = &loonggpu_ih_lg2xx_funcs;
		break;
	default:
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);
		break;
	}
}

const struct loonggpu_ip_block_version loonggpu_ih_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_IH,
	.major = 3,
	.minor = 0,
	.rev = 0,
	.funcs = &loonggpu_ih_ip_funcs,
};

/**
 * loonggpu_ih_ring_alloc - allocate memory for the IH ring
 *
 * @adev: loonggpu_device pointer
 *
 * Allocate a ring buffer for the interrupt controller.
 * Returns 0 for success, errors for failure.
 */
static int loonggpu_ih_ring_alloc(struct loonggpu_device *adev)
{
	int r;

	/* Allocate ring buffer */
	if (adev->irq.ih.ring_obj == NULL) {
		r = loonggpu_bo_create_kernel(adev, adev->irq.ih.ring_size,
					    PAGE_SIZE, LOONGGPU_GEM_DOMAIN_GTT,
					    &adev->irq.ih.ring_obj,
					    &adev->irq.ih.gpu_addr,
					    (void **)&adev->irq.ih.ring);
		if (r) {
			DRM_ERROR("loonggpu: failed to create ih ring buffer (%d).\n", r);
			return r;
		}
	}
	return 0;
}

/**
 * loonggpu_ih_ring_init - initialize the IH state
 *
 * @adev: loonggpu_device pointer
 *
 * Initializes the IH state and allocates a buffer
 * for the IH ring buffer.
 * Returns 0 for success, errors for failure.
 */
int loonggpu_ih_ring_init(struct loonggpu_device *adev, unsigned ring_size,
			bool use_bus_addr)
{
	u32 rb_bufsz;
	int r;

	/* Align ring size */
	rb_bufsz = order_base_2(ring_size / 4);
	ring_size = (1 << rb_bufsz) * 4;
	adev->irq.ih.ring_size = ring_size;
	adev->irq.ih.ptr_mask = adev->irq.ih.ring_size / 4 - 1;
	adev->irq.ih.rptr = 0;
	adev->irq.ih.use_bus_addr = use_bus_addr;
	init_waitqueue_head(&adev->irq.ih.wait_process);

	if (adev->irq.ih.use_bus_addr) {
		if (!adev->irq.ih.ring) {
			/* add 8 bytes for the rptr/wptr shadows and
			 * add them to the end of the ring allocation.
			 */
			adev->irq.ih.ring = dma_alloc_coherent(&adev->pdev->dev,
					adev->irq.ih.ring_size + 8, &adev->irq.ih.rb_dma_addr, GFP_ATOMIC);
			if (adev->irq.ih.ring == NULL)
				return -ENOMEM;
			memset((void *)adev->irq.ih.ring, 0, adev->irq.ih.ring_size + 8);
			adev->irq.ih.wptr_offs = (adev->irq.ih.ring_size / 4) + 0;
			adev->irq.ih.rptr_offs = (adev->irq.ih.ring_size / 4) + 1;
		}
		return 0;
	} else {
		r = loonggpu_device_wb_get(adev, &adev->irq.ih.wptr_offs);
		if (r) {
			dev_err(adev->dev, "(%d) ih wptr_offs wb alloc failed\n", r);
			return r;
		}

		r = loonggpu_device_wb_get(adev, &adev->irq.ih.rptr_offs);
		if (r) {
			loonggpu_device_wb_free(adev, adev->irq.ih.wptr_offs);
			dev_err(adev->dev, "(%d) ih rptr_offs wb alloc failed\n", r);
			return r;
		}

		return loonggpu_ih_ring_alloc(adev);
	}
}

/**
 * loonggpu_ih_ring_fini - tear down the IH state
 *
 * @adev: loonggpu_device pointer
 *
 * Tears down the IH state and frees buffer
 * used for the IH ring buffer.
 */
void loonggpu_ih_ring_fini(struct loonggpu_device *adev)
{
	if (adev->irq.ih.use_bus_addr) {
		if (adev->irq.ih.ring) {
			/* add 8 bytes for the rptr/wptr shadows and
			 * add them to the end of the ring allocation.
			 */
			dma_free_coherent(&adev->pdev->dev, adev->irq.ih.ring_size + 8,
				(void *)adev->irq.ih.ring, adev->irq.ih.rb_dma_addr);
			adev->irq.ih.ring = NULL;
		}
	} else {
		loonggpu_bo_free_kernel(&adev->irq.ih.ring_obj,
				      &adev->irq.ih.gpu_addr,
				      (void **)&adev->irq.ih.ring);
		loonggpu_device_wb_free(adev, adev->irq.ih.wptr_offs);
		loonggpu_device_wb_free(adev, adev->irq.ih.rptr_offs);
	}
}

/**
 * loonggpu_ih_wait_on_checkpoint_process_ts - wait to process IVs up to checkpoint
 *
 * @adev: loonggpu_device pointer
 * @ih: ih ring to process
 *
 * Used to ensure ring has processed IVs up to the checkpoint write pointer.
 */
int loonggpu_ih_wait_on_checkpoint_process_ts(struct loonggpu_device *adev,
					struct loonggpu_ih_ring *ih)
{
	uint32_t checkpoint_wptr;
	long timeout = HZ;

	if (!ih->enabled || adev->shutdown)
		return -ENODEV;

	checkpoint_wptr = loonggpu_ih_get_wptr(adev);
	/* Order wptr with ring data. */
	rmb();

	return wait_event_interruptible_timeout(ih->wait_process,
		    ih->rptr == loonggpu_ih_get_wptr(adev), timeout);
}

/**
 * loonggpu_ih_process - interrupt handler
 *
 * @adev: loonggpu_device pointer
 *
 * Interrupt hander  , walk the IH ring.
 * Returns irq process return code.
 */
int loonggpu_ih_process(struct loonggpu_device *adev)
{
	struct loonggpu_iv_entry entry;
	u32 wptr;

	if (!adev->irq.ih.enabled || adev->shutdown)
		return IRQ_NONE;

	if (adev->family_type == CHIP_LG100) {
		if (!adev->irq.msi_enabled)
			WREG32(LOONGGPU_HOST_INT, 0);
	} else if (adev->family_type == CHIP_LG200 ||
		   adev->family_type == CHIP_LG210 ||
		   adev->family_type == CHIP_LG211) {
		;
	} else
		DRM_ERROR("%s Illegal Family type %d\n", __FUNCTION__, adev->family_type);

	wptr = loonggpu_ih_get_wptr(adev);

restart_ih:
	/* is somebody else already processing irqs? */
	if (atomic_xchg(&adev->irq.ih.lock, 1))
		return IRQ_NONE;

	DRM_DEBUG("%s: rptr %d, wptr %d\n", __func__, adev->irq.ih.rptr, wptr);

	/* Order reading of wptr vs. reading of IH ring data */
	rmb();

	while (adev->irq.ih.rptr != wptr) {
		u32 ring_index = adev->irq.ih.rptr;

		/* Prescreening of high-frequency interrupts */
		if (!loonggpu_ih_prescreen_iv(adev)) {
			adev->irq.ih.rptr &= adev->irq.ih.ptr_mask;
			continue;
		}

		/* Before dispatching irq to IP blocks, send it to lgkcd */
		loonggpu_lgkcd_interrupt(adev,
				(const void *) &adev->irq.ih.ring[ring_index]);

		entry.iv_entry = (const uint32_t *)
			&adev->irq.ih.ring[ring_index];
		loonggpu_ih_decode_iv(adev, &entry);
		adev->irq.ih.rptr &= adev->irq.ih.ptr_mask;

		loonggpu_irq_dispatch(adev, &entry);
	}
	loonggpu_ih_set_rptr(adev);
	atomic_set(&adev->irq.ih.lock, 0);

	wake_up_all(&adev->irq.ih.wait_process);

	/* make sure wptr hasn't changed while processing */
	wptr = loonggpu_ih_get_wptr(adev);
	if (wptr != adev->irq.ih.rptr)
		goto restart_ih;

	return IRQ_HANDLED;
}
