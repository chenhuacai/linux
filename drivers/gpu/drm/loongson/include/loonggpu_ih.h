#ifndef __LOONGGPU_IH_H__
#define __LOONGGPU_IH_H__

struct loonggpu_device;

#define LOONGGPU_IH_CLIENTID_LEGACY 0
#define LOONGGPU_IH_CLIENTID_MAX SOC15_IH_CLIENTID_MAX

extern const struct loonggpu_ip_block_version loonggpu_ih_ip_block;

#define LOONGGPU_PAGEFAULT_HASH_BITS 8

enum soc15_ih_clientid {
	SOC15_IH_CLIENTID_IH		= 0x00,
	SOC15_IH_CLIENTID_ACP		= 0x01,
	SOC15_IH_CLIENTID_ATHUB		= 0x02,
	SOC15_IH_CLIENTID_BIF		= 0x03,
	SOC15_IH_CLIENTID_DCE		= 0x04,
	SOC15_IH_CLIENTID_ISP		= 0x05,
	SOC15_IH_CLIENTID_PCIE0		= 0x06,
	SOC15_IH_CLIENTID_RLC		= 0x07,
	SOC15_IH_CLIENTID_SDMA0		= 0x08,
	SOC15_IH_CLIENTID_SDMA1		= 0x09,
	SOC15_IH_CLIENTID_SE0SH		= 0x0a,
	SOC15_IH_CLIENTID_SE1SH		= 0x0b,
	SOC15_IH_CLIENTID_SE2SH		= 0x0c,
	SOC15_IH_CLIENTID_SE3SH		= 0x0d,
	SOC15_IH_CLIENTID_SYSHUB	= 0x0e,
	SOC15_IH_CLIENTID_UVD1		= 0x0e,
	SOC15_IH_CLIENTID_THM		= 0x0f,
	SOC15_IH_CLIENTID_UVD		= 0x10,
	SOC15_IH_CLIENTID_VCE0		= 0x11,
	SOC15_IH_CLIENTID_VMC		= 0x12,
	SOC15_IH_CLIENTID_XDMA		= 0x13,
	SOC15_IH_CLIENTID_GRBM_CP	= 0x14,
	SOC15_IH_CLIENTID_ATS		= 0x15,
	SOC15_IH_CLIENTID_ROM_SMUIO	= 0x16,
	SOC15_IH_CLIENTID_DF		= 0x17,
	SOC15_IH_CLIENTID_VCE1		= 0x18,
	SOC15_IH_CLIENTID_PWR		= 0x19,
	SOC15_IH_CLIENTID_UTCL2		= 0x1b,
	SOC15_IH_CLIENTID_EA		= 0x1c,
	SOC15_IH_CLIENTID_UTCL2LOG	= 0x1d,
	SOC15_IH_CLIENTID_MP0		= 0x1e,
	SOC15_IH_CLIENTID_MP1		= 0x1f,

	SOC15_IH_CLIENTID_MAX,

	SOC15_IH_CLIENTID_VCN		= SOC15_IH_CLIENTID_UVD
};

/*
 * R6xx+ IH ring
 */
struct loonggpu_ih_ring {
	struct loonggpu_bo	*ring_obj;
	volatile uint32_t	*ring;
	unsigned		rptr;
	unsigned		ring_size;
	uint64_t		gpu_addr;
	uint32_t		ptr_mask;
	atomic_t		lock;
	bool                    enabled;
	unsigned		wptr_offs;
	unsigned		rptr_offs;
	bool			use_bus_addr;
	dma_addr_t		rb_dma_addr; /* only used when use_bus_addr = true */
	/* For waiting on IH processing at checkpoint. */
	wait_queue_head_t wait_process;
};

#define LOONGGPU_IH_SRC_DATA_MAX_SIZE_DW 4

struct loonggpu_iv_entry {
	unsigned client_id;
	unsigned src_id;
	unsigned ring_id;
	unsigned vmid;
	unsigned vmid_src;
	uint64_t timestamp;
	unsigned timestamp_src;
	unsigned pasid;
	unsigned pasid_src;
	unsigned src_data[LOONGGPU_IH_SRC_DATA_MAX_SIZE_DW];
	const uint32_t *iv_entry;
};

int loonggpu_ih_ring_init(struct loonggpu_device *adev, unsigned ring_size,
			bool use_bus_addr);
void loonggpu_ih_ring_fini(struct loonggpu_device *adev);
int loonggpu_ih_process(struct loonggpu_device *adev);
int loonggpu_ih_wait_on_checkpoint_process_ts(struct loonggpu_device *adev,
					    struct loonggpu_ih_ring *ih);

#endif /* __LOONGGPU_IH_H__ */
