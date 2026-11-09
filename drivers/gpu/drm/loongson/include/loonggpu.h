#ifndef __LOONGGPU_H__
#define __LOONGGPU_H__

#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/hashtable.h>
#include <linux/dma-fence.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/hrtimer.h>
#include <drm/drm_buddy.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/gpu_scheduler.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_execbuf_util.h>

#include "loonggpu_drm.h"
#include "loonggpu_shared.h"
#include "loonggpu_mode.h"
#include "loonggpu_ih.h"
#include "loonggpu_irq.h"
#include "loonggpu_ttm.h"
#include "loonggpu_sync.h"
#include "loonggpu_ring.h"
#include "loonggpu_vm.h"
#include "loonggpu_mn.h"
#include "loonggpu_gmc.h"
#include "loonggpu_dc.h"
#include "loonggpu_gart.h"
#include "loonggpu_zip_meta.h"
#include "loonggpu_debugfs.h"
#include "loonggpu_job.h"
#include "loonggpu_bo_list.h"
#include "loonggpu_hw_sema.h"
#include "loonggpu_lgkcd.h"
#include "loonggpu_doorbell.h"

/*
 * Modules parameters.
 */
extern int loonggpu_modeset;
extern int loonggpu_vram_limit;
extern int loonggpu_vis_vram_limit;
extern int loonggpu_gart_size;
extern int loonggpu_gtt_size;
extern int loonggpu_moverate;
extern int loonggpu_benchmarking;
extern int loonggpu_testing;
extern int loonggpu_disp_priority;
extern int loonggpu_msi;
extern int loonggpu_lockup_timeout;
extern int loonggpu_gfx_lockup_timeout;
extern int loonggpu_dpm;
extern int loonggpu_runtime_pm;
extern int loonggpu_vm_size;
extern int loonggpu_vm_block_size;
extern int loonggpu_vm_fragment_size;
extern int loonggpu_vm_fault_stop;
extern int loonggpu_vm_debug;
extern int loonggpu_vm_update_mode;
extern int loonggpu_sched_jobs;
extern int loonggpu_sched_hw_submission;
extern int loonggpu_vram_page_split;
extern int loonggpu_job_hang_limit;
extern int loonggpu_gpu_recovery;
extern int loonggpu_using_ram;
extern int loonggpu_write_combine;
extern int loonggpu_noretry;
extern int loonggpu_virtual_apu;
extern int loonggpu_cwsr_enable;
extern int loonggpu_panel_cfg_clk_pol;
extern int loonggpu_panel_cfg_de_pol;
extern int loonggpu_gpu_uart;
extern int loonggpu_ls7a2000_mode_limit;
extern int loonggpu_ls7a2000_mode_limit_soft;
extern int loonggpu_hw_cursor;

extern bool no_system_mem_limit;

#ifdef CONFIG_VDD_LOONGSON_P2P
extern bool loonggpu_pcie_p2p;
#endif

#define GFX_RING_BUF_DWS	256
#define XDMA_RING_BUF_DWS	256
#define LOONGGPU_BYTES_PER_DW           4
#define MAX_GPU_INSTANCE	     64

#define LOONGGPU_KB_SHIFT_BITS          10
#define LOONGGPU_MB_SHIFT_BITS          20
#define LOONGGPU_GB_SHIFT_BITS          30

#define LOONGGPU_SG_THRESHOLD			(256*1024*1024)
#define LOONGGPU_DEFAULT_GTT_SIZE_MB		3072ULL /* 3GB by default */
#define LOONGGPU_WAIT_IDLE_TIMEOUT_IN_MS	        3000
#define LOONGGPU_MAX_USEC_TIMEOUT			100000	/* 100 ms */
#define LOONGGPU_FENCE_JIFFIES_TIMEOUT		(HZ / 2)
/* LOONGGPU_IB_POOL_SIZE must be a power of 2 */
#define LOONGGPU_IB_POOL_SIZE			16
#define LOONGGPU_DEBUGFS_MAX_COMPONENTS		32
#define LOONGGPUFB_CONN_LIMIT			4
#define LOONGGPU_BIOS_NUM_SCRATCH			16
#define LOONGGPU_MAX_COMPUTE_RINGS			1

/* max number of IP instances */
#define LOONGGPU_MAX_XDMA_INSTANCES		2

/* hard reset data */
#define LOONGGPU_ASIC_RESET_DATA                  0x39d5e86b

/* GFX current status */
#define LOONGGPU_GFX_NORMAL_MODE			0x00000000L
#define LOONGGPU_GFX_SAFE_MODE			0x00000001L
#define LOONGGPU_GFX_PG_DISABLED_MODE		0x00000002L
#define LOONGGPU_GFX_CG_DISABLED_MODE		0x00000004L
#define LOONGGPU_GFX_LBPW_DISABLED_MODE		0x00000008L

struct loonggpu_device;
struct loonggpu_ib;
struct loonggpu_cs_parser;
struct loonggpu_job;
struct loonggpu_irq_src;
struct loonggpu_fpriv;
struct loonggpu_bo_va_mapping;

extern const char *loonggpu_family_name[];

enum loonggpu_chip {
	dev_7a2000,
	dev_2k2000,
	dev_2k3000,
	dev_7a1000,
	dev_9a1000
};

enum loonggpu_cp_irq {
	LOONGGPU_CP_IRQ_GFX_EOP = 0,
	LOONGGPU_CP_IRQ_BPIPE_EOP,
	LOONGGPU_CP_IRQ_DPIPE_EOP,
	LOONGGPU_CP_IRQ_EPIPE_EOP,
	LOONGGPU_CP_IRQ_LAST
};

enum loonggpu_xdma_irq {
	LOONGGPU_XDMA_IRQ_TRAP0 = 0,
	LOONGGPU_XDMA_IRQ_TRAP1,
	LOONGGPU_XDMA_IRQ_LAST
};

int loonggpu_device_ip_wait_for_idle(struct loonggpu_device *adev,
				   enum loonggpu_ip_block_type block_type);
bool loonggpu_device_ip_is_idle(struct loonggpu_device *adev,
			      enum loonggpu_ip_block_type block_type);

bool loonggpu_device_is_peer_accessible(struct loonggpu_device *adev,
					struct loonggpu_device *peer_adev);

#define LOONGGPU_MAX_IP_NUM 16

struct loonggpu_ip_block_status {
	bool valid;
	bool sw;
	bool hw;
	bool late_initialized;
	bool hang;
};

struct loonggpu_ip_block_version {
	const enum loonggpu_ip_block_type type;
	const u32 major;
	const u32 minor;
	const u32 rev;
	const struct loonggpu_ip_funcs *funcs;
};

struct loonggpu_ip_block {
	struct loonggpu_ip_block_status status;
	const struct loonggpu_ip_block_version *version;
};

int loonggpu_device_ip_block_version_cmp(struct loonggpu_device *adev,
				       enum loonggpu_ip_block_type type,
				       u32 major, u32 minor);

struct loonggpu_ip_block *
loonggpu_device_ip_get_ip_block(struct loonggpu_device *adev,
			      enum loonggpu_ip_block_type type);

int loonggpu_device_ip_block_add(struct loonggpu_device *adev,
			       const struct loonggpu_ip_block_version *ip_block_version);

/* provided by hw blocks that can move/clear data.  e.g., gfx or xdma */
struct loonggpu_buffer_funcs {
	/* maximum bytes in a single operation */
	uint32_t	copy_max_bytes;

	/* number of dw to reserve per operation */
	unsigned	copy_num_dw;

	/* used for buffer migration */
	void (*emit_copy_buffer)(struct loonggpu_ib *ib,
				 /* src addr in bytes */
				 uint64_t src_offset,
				 /* dst addr in bytes */
				 uint64_t dst_offset,
				 /* number of byte to transfer */
				 uint32_t byte_count);

	/* maximum bytes in a single operation */
	uint32_t	fill_max_bytes;

	/* number of dw to reserve per operation */
	unsigned	fill_num_dw;

	/* used for buffer clearing */
	void (*emit_fill_buffer)(struct loonggpu_ib *ib,
				 /* value to write to memory */
				 uint32_t src_data,
				 /* dst addr in bytes */
				 uint64_t dst_offset,
				 /* number of byte to fill */
				 uint32_t byte_count);
};

/* provided by hw blocks that can write ptes, e.g., xdma */
struct loonggpu_vm_pte_funcs {
	/* number of dw to reserve per operation */
	unsigned	copy_pte_num_dw;

	/* number of dw to reserve per operation */
	unsigned	set_pte_pde_num_dw;

	/* copy pte entries from GART */
	void (*copy_pte)(struct loonggpu_ib *ib,
			 uint64_t pe, uint64_t src,
			 unsigned count);

	/* write pte one entry at a time with addr mapping */
	void (*write_pte)(struct loonggpu_ib *ib, uint64_t pe,
			  uint64_t value, unsigned count,
			  uint32_t incr);
	/* for linear pte/pde updates without addr mapping */
	void (*set_pte_pde)(struct loonggpu_ib *ib,
			    uint64_t pe,
			    uint64_t addr, unsigned count,
			    uint32_t incr, uint64_t flags);
};

/* provided by the ih block */
struct loonggpu_ih_funcs {
	/* ring read/write ptr handling, called from interrupt context */
	u32 (*get_wptr)(struct loonggpu_device *adev);
	bool (*prescreen_iv)(struct loonggpu_device *adev);
	void (*decode_iv)(struct loonggpu_device *adev,
			  struct loonggpu_iv_entry *entry);
	void (*set_rptr)(struct loonggpu_device *adev);
};

/*
 * BIOS.
 */
bool loonggpu_get_bios(struct loonggpu_device *adev);
bool loonggpu_read_bios(struct loonggpu_device *adev);

/*
 * Clocks
 */

#define LOONGGPU_MAX_PPLL 3

struct loonggpu_clock {
	/* 10 Khz units */
	uint32_t default_mclk;
	uint32_t default_sclk;
	uint32_t default_dispclk;
	uint32_t current_dispclk;
	uint32_t dp_extclk;
	uint32_t max_pixel_clock;
};

struct loonggpu_cs_post_dep {
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	u64 point;
};

/*
 * GEM.
 */

#define LOONGGPU_GEM_DOMAIN_MAX		0x3
#define gem_to_loonggpu_bo(gobj) container_of((gobj), struct loonggpu_bo, tbo.base)

void loonggpu_gem_object_free(struct drm_gem_object *obj);
int loonggpu_gem_object_open(struct drm_gem_object *obj,
				struct drm_file *file_priv);
void loonggpu_gem_object_close(struct drm_gem_object *obj,
				struct drm_file *file_priv);
unsigned long loonggpu_gem_timeout(uint64_t timeout_ns);
struct sg_table *loonggpu_gem_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *
loonggpu_gem_prime_import_sg_table(struct drm_device *dev,
				 struct dma_buf_attachment *attach,
				 struct sg_table *sg);
struct dma_buf *loonggpu_gem_prime_export(struct drm_gem_object *gobj, int flags);
struct drm_gem_object *loonggpu_gem_prime_import(struct drm_device *dev,
					    struct dma_buf *dma_buf);
#if defined(LG_DRM_DRIVER_HAS_GEM_PRIME_RES_OBJ)
lg_dma_resv_t *loonggpu_gem_prime_res_obj(struct drm_gem_object *);
#endif
void *loonggpu_gem_prime_vmap(struct drm_gem_object *obj);
void loonggpu_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr);
int loonggpu_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma);

/* sub-allocation manager, it has to be protected by another lock.
 * By conception this is an helper for other part of the driver
 * like the indirect buffer or semaphore, which both have their
 * locking.
 *
 * Principe is simple, we keep a list of sub allocation in offset
 * order (first entry has offset == 0, last entry has the highest
 * offset).
 *
 * When allocating new object we first check if there is room at
 * the end total_size - (last_object_offset + last_object_size) >=
 * alloc_size. If so we allocate new object there.
 *
 * When there is not enough room at the end, we start waiting for
 * each sub object until we reach object_offset+object_size >=
 * alloc_size, this object then become the sub object we return.
 *
 * Alignment can't be bigger than page size.
 *
 * Hole are not considered for allocation to keep things simple.
 * Assumption is that there won't be hole (all object on same
 * alignment).
 */

#define LOONGGPU_SA_NUM_FENCE_LISTS	32

struct loonggpu_sa_manager {
	wait_queue_head_t	wq;
	struct loonggpu_bo	*bo;
	struct list_head	*hole;
	struct list_head	flist[LOONGGPU_SA_NUM_FENCE_LISTS];
	struct list_head	olist;
	unsigned		size;
	uint64_t		gpu_addr;
	void			*cpu_ptr;
	uint32_t		domain;
	uint32_t		align;
};

/* sub-allocation buffer */
struct loonggpu_sa_bo {
	struct list_head		olist;
	struct list_head		flist;
	struct loonggpu_sa_manager	*manager;
	unsigned			soffset;
	unsigned			eoffset;
	struct dma_fence	        *fence;
};

/*
 * GEM objects.
 */
void loonggpu_gem_force_release(struct loonggpu_device *adev);
int loonggpu_gem_object_create(struct loonggpu_device *adev, unsigned long size,
			     int alignment, u32 initial_domain,
			     u64 flags, enum ttm_bo_type type,
			     lg_dma_resv_t *resv,
			     struct drm_gem_object **obj);

int loonggpu_mode_dumb_create(struct drm_file *file_priv,
			    struct drm_device *dev,
			    struct drm_mode_create_dumb *args);
int loonggpu_mode_dumb_mmap(struct drm_file *filp,
			  struct drm_device *dev,
			  uint32_t handle, uint64_t *offset_p);
int loonggpu_fence_slab_init(void);
void loonggpu_fence_slab_fini(void);

/* GS Registers */
#define LOONGGPU_COMMAND				0x0
#define LOONGGPU_STATUS				0x4
#define LOONGGPU_ARGUMENT0				0x8
#define LOONGGPU_ARGUMENT1				0xc
#define LOONGGPU_RETURN0				0x10
#define LOONGGPU_RETURN1				0x14
#define LOONGGPU_GFX_CB_BASE_LO_OFFSET		0x18
#define LOONGGPU_GFX_CB_BASE_HI_OFFSET		0x1c
#define LOONGGPU_GFX_CB_SIZE_OFFSET		0x20
#define LOONGGPU_GFX_CB_WPTR_OFFSET		0x24
#define LOONGGPU_GFX_CB_RPTR_OFFSET		0x28
#define LOONGGPU_XDMA_CB_BASE_LO_OFFSET		0x2c
#define LOONGGPU_XDMA_CB_BASE_HI_OFFSET		0x30
#define LOONGGPU_XDMA_CB_SIZE_OFFSET		0x34
#define LOONGGPU_XDMA_CB_WPTR_OFFSET		0x38
#define LOONGGPU_XDMA_CB_RPTR_OFFSET		0x3c
#define LOONGGPU_INT_CB_BASE_LO_OFFSET		0x40
#define LOONGGPU_INT_CB_BASE_HI_OFFSET		0x44
#define LOONGGPU_INT_CB_SIZE_OFFSET		0x48
#define LOONGGPU_INT_CB_WPTR_OFFSET		0x4c
#define LOONGGPU_INT_CB_RPTR_OFFSET		0x50
/* reserved 0x54 ~ 0x74 */
#define LOONGGPU_RESERVE_START_OFFSET		0x54
#define LOONGGPU_RESERVE_END_OFFSET		0x74

#define LOONGGPU_FW_VERSION_OFFSET			0x78
#define LOONGGPU_HW_FEATURE_OFFSET			0x7c

#define LOONGGPU_EC_CTRL				0x80
#define LOONGGPU_EC_INT				0x84
#define LOONGGPU_HOST_INT				0x88
#define LOONGGPU_HWINF				0x8c
#define LOONGGPU_TIME_COUNT_LO			0x90
#define LOONGGPU_TIME_COUNT_HI			0x94
#define LOONGGPU_FREQ_SCALE			0x9c
#define LOONGGPU_POWER_LEVEL_RET		0xe8

#define LOONGGPU_FW_WPORT				0xf0
#define LOONGGPU_FW_WPTR				0xf4
#define LOONGGPU_FW_CKSUM				0xf8

/* GS Commands */
#define GSCMD(cmd, subcmd)	(((cmd) & 0xFF)  | ((subcmd) & 0xFF) << 8)
#define GSCMDi(cmd, subcmd, i)	(((cmd) & 0xFF)  | ((subcmd) & 0xFF) << 8 | ((i) & 0xF) << 16)

#define GSCMD_HALT		0x00000000 /* stop jobs in GPU, return */
#define GSCMD_PING_5A		0x00000001 /* return 5a5a5a5a in status */
#define GSCMD_PING_A5		0x00000002 /* return a5a5a5a5 in status */
#define GSCMD_LOOP_DRAM		0x00000003 /* loop through DRAM */
#define GSCMD_LOOP_SSRV		0x00000004 /* loop through SSRV */
#define GSCMD_START		0x00000005 /* start processing command buffer */
#define GSCMD_STOP		0x00000006 /*stop processing command buffer */
#define GSCMD_SYNC		0x00000007 /* wait pipeline empty */
#define GSCMD_MMU		0x00000008 /* mmu related op */
#define GSCMD_SETREG		0x00000009 /* internal reg op */
#define GSCMD_PIPE		0x0000000A /* op pipeline */
#define GSCMD_ZIP		0x0000000B /* op zip */
#define GSCMD_PIPE_FLUSH	1 /* op pipeline */
#define GSCMD_FREQ		0x0000000C

#define GSCMD_STS_NULL		0x00000000
#define GSCMD_STS_BOOT		0xB007B007 /* BOOT */
#define GSCMD_STS_DONE		0xD02ED02E /* DONE */
#define GSCMD_STS_RUN		0xFFFF0000 /* RUNING, lower 16bit can store total command count */

#define EC_CTRL_RUN		0x01
#define EC_CTRL_STOP		0x00

/* GS Packets */
#define GSPKT(op, n)	(((op) & 0xFF) | ((n) & 0xFFFF) << 16)

#define	GSPKT_NOP			0x80
#define	GSPKT_WRITE			0x81
#define GSPKT_INDIRECT			0x82
#define GSPKT_FENCE			0x83
#define GSPKT_TRAP			0x84
#define GSPKT_POLL			0x85
#define 	POLL_CONDITION(x)		((x) << 8)
		/* 0 - true
		 * 1 - <
		 * 2 - <=
		 * 3 - ==
		 * 4 - !=
		 * 5 - >=
		 * 6 - >
		 */
#define 	POLL_REG_MEM(x)			((x) << 12)
		/* 0 - reg
		 * 1 - mem
		 */
#define		POLL_TIMES_INTERVAL(t, i)	((t) << 16 | (i))
#define GSPKT_WPOLL			0x86
#define	GSPKT_READ			0x87

/* DRAW 0x89 */
#define GSPKT_VM_BIND			0x8A

#define GSPKT_XDMA_COPY			0xc0

/* 0 - register
 * 1 - memory
 */
#define	READ_SRC_SEL(x)			((x) << 9)
#define	WRITE_DST_SEL(x)		((x) << 8)
#define	WRITE_WAIT			(1 << 15)

/* lg2xx compatible firmware command definitions */
/* Immediate command mode */
#define LG2XX_ICMD32(mop, sop)				(((mop) & 0xFF)  | ((sop) & 0xFF) << 8)
#define LG2XX_ICMD32i(mop, sop, cfg)			(((mop) & 0xFF)  | ((sop) & 0xFF) << 8 | (cfg & 0xFFFF) << 16)

#define LOONGGPU_LG2XX_COMMAND				0x0
#define LOONGGPU_LG2XX_STATUS				0x4
#define LOONGGPU_LG2XX_ARGUMENT0				0x8
#define LOONGGPU_LG2XX_ARGUMENT1				0xc
#define LOONGGPU_LG2XX_RETURN0				0x10
#define LOONGGPU_LG2XX_RETURN1				0x14
#define LOONGGPU_LG2XX_GPIPE_CB_WPTR_OFFSET		0x18
#define LOONGGPU_LG2XX_GPIPE_CB_RPTR_OFFSET		0x1c
#define LOONGGPU_LG2XX_XDMA_CB_WPTR_OFFSET			0x20
#define LOONGGPU_LG2XX_XDMA_CB_RPTR_OFFSET			0x24
#define LOONGGPU_LG2XX_RESERVED_REG_0x28			0x28
#define LOONGGPU_LG2XX_RESERVED_REG_0x2C			0x2c
#define LOONGGPU_LG2XX_BPIPE_CB_WPTR_OFFSET		0x30
#define LOONGGPU_LG2XX_BPIPE_CB_RPTR_OFFSET		0x34
#define LOONGGPU_LG2XX_CPIPE_CB_WPTR_OFFSET		0x38
#define LOONGGPU_LG2XX_CPIPE_CB_RPTR_OFFSET		0x3c
#define LOONGGPU_LG2XX_FW_VERSION_OFFSET		0x40
#define LOONGGPU_LG2XX_IP_STATE				0x44
#define LOONGGPU_LG2XX_DPIPE_CB_WPTR_OFFSET         	0x48
#define LOONGGPU_LG2XX_DPIPE_CB_RPTR_OFFSET         	0x4c
#define LOONGGPU_LG2XX_EPIPE_CB_WPTR_OFFSET         	0x50
#define LOONGGPU_LG2XX_EPIPE_CB_RPTR_OFFSET         	0x54
#define LOONGGPU_LG2XX_DPM_STATE			0xe8

#define LOONGGPU_LG2XX_TIME_COUNT_LO			0x90
#define LOONGGPU_LG2XX_TIME_COUNT_HI			0x94

#define LG2XX_ICMD32_MOP_GPIPE		0x00000001
	#define LG2XX_ICMD32_SOP_GPIPE_BGQ	0x00000001
	#define LG2XX_ICMD32_SOP_GPIPE_GQSZ	0x00000002
	#define LG2XX_ICMD32_SOP_GPIPE_UBGQ	0x00000003
	#define LG2XX_ICMD32_SOP_GPIPE_RESET	0x00000004
#define LG2XX_ICMD32_MOP_BPIPE		0x00000002
	#define LG2XX_ICMD32_SOP_BPIPE_BBQ	0x00000001
	#define LG2XX_ICMD32_SOP_BPIPE_BQSZ	0x00000002
	#define LG2XX_ICMD32_SOP_BPIPE_UBBQ	0x00000003
#define LG2XX_ICMD32_MOP_CPIPE		0x00000003
	#define LG2XX_ICMD32_SOP_CPIPE_BCQ	0x00000001
	#define LG2XX_ICMD32_SOP_CPIPE_CQSZ	0x00000002
	#define LG2XX_ICMD32_SOP_CPIPE_UBCQ	0x00000003
	#define LG2XX_ICMD32_SOP_CPIPE_BCDB	0x00000004
	#define LG2XX_ICMD32_SOP_CPIPE_UBCDB	0x00000005
	#define LG2XX_ICMD32_SOP_CPIPE_ATOMIC	0x00000006
#define LG2XX_ICMD32_MOP_XDMA		0x00000004
	#define LG2XX_ICMD32_SOP_XDMA_BDQ	0x00000001
	#define LG2XX_ICMD32_SOP_XDMA_DQSZ	0x00000002
	#define LG2XX_ICMD32_SOP_XDMA_UBDQ	0x00000003
#define LG2XX_ICMD32_MOP_SYNC		0x00000005
	#define LG2XX_ICMD32_SOP_SYNC_GSYNC	0x00000001
	#define LG2XX_ICMD32_SOP_SYNC_BSYNC	0x00000002
	#define LG2XX_ICMD32_SOP_SYNC_CSYNC	0x00000003
#define LG2XX_ICMD32_MOP_MMU		0x00000006
	#define LG2XX_ICMD32_SOP_MMU_MMUEN	0x00000001
	#define LG2XX_ICMD32_SOP_MMU_UDIR	0x00000002
	#define LG2XX_ICMD32_SOP_MMU_USAFE	0x00000003
	#define LG2XX_ICMD32_SOP_MMU_UPG	0x00000004
	#define LG2XX_ICMD32_SOP_MMU_UEXP	0x00000005
	#define LG2XX_ICMD32_SOP_MMU_FTLB	0x00000006
	#define LG2XX_ICMD32_SOP_MMU_UVMADDR	0x00000007
	#define LG2XX_ICMD32_SOP_MMU_UVMSZ	0x00000008
	#define LG2XX_ICMD32_SOP_MMU_RETRY	0x00000009
#define LG2XX_ICMD32_MOP_ZIP		0x00000007
	#define LG2XX_ICMD32_SOP_ZIP_ZEN	0x00000001
	#define LG2XX_ICMD32_SOP_ZIP_ZDIS	0x00000002
	#define LG2XX_ICMD32_SOP_ZIP_UTAGADDR	0x00000003
	#define LG2XX_ICMD32_SOP_ZIP_UTAGMASK	0x00000004
	#define LG2XX_ICMD32_SOP_ZIP_CNT	0x00000006
	#define LG2XX_ICMD32_SOP_ZIP_CNTEN	0x00000007
	#define LG2XX_ICMD32_SOP_ZIP_CNTDIS	0x00000008
#define LG2XX_ICMD32_MOP_FREQ		0x00000008
	#define LG2XX_ICMD32_SOP_FREQ_UFRQ	0x00000000
	#define LG2XX_ICMD32_SOP_POWER_LEVEL_UFRQ	0x00000001
#define LG2XX_ICMD32_MOP_EXC		0x00000009
	#define LG2XX_ICMD32_SOP_EXC_BEQ	0x00000001
	#define LG2XX_ICMD32_SOP_EXC_EQSZ	0x00000002
	#define LG2XX_ICMD32_SOP_EXC_UBEQ	0x00000003
	#define LG2XX_ICMD32_SOP_EXC_HBEQ	0x00000004
#define LG2XX_ICMD32_MOP_DOORBELL	0x0000000a
	#define LG2XX_ICMD32_SOP_DB_ZEN		0x00000001
	#define LG2XX_ICMD32_SOP_DB_ZDIS	0x00000002
#define LG2XX_ICMD32_MOP_CWSR	0x0000000b
	#define LG2XX_ICMD32_SOP_CWSR_ZEN	0x00000001
	#define LG2XX_ICMD32_SOP_CWSR_ZDIS	0x00000002
#define LG2XX_ICMD32_MOP_DPIPE         0x0000000c
	#define LG2XX_ICMD32_SOP_DPIPE_BBQ      0x00000001
	#define LG2XX_ICMD32_SOP_DPIPE_BQSZ     0x00000002
	#define LG2XX_ICMD32_SOP_DPIPE_UBBQ     0x00000003
#define LG2XX_ICMD32_MOP_EPIPE         0x0000000d
	#define LG2XX_ICMD32_SOP_EPIPE_BBQ      0x00000001
	#define LG2XX_ICMD32_SOP_EPIPE_BQSZ     0x00000002
	#define LG2XX_ICMD32_SOP_EPIPE_UBBQ     0x00000003
#define LG2XX_ICMD32_MOP_VRAM         0x0000000e
	#define LG2XX_ICMD32_SOP_VRAM_BINDADDR  0x00000001
	#define LG2XX_ICMD32_SOP_VRAM_READ      0x00000002
	#define LG2XX_ICMD32_SOP_VRAM_WRITE     0x00000003

/* Stream command mode */
#define LG2XX_SCMD32(op, cfg)				(((op) & 0xFF)  | ((cfg) & 0xFFFFFF) << 8)

#define LG2XX_SCMD32_OP_NOP				0x00 /* no operation */
#define LG2XX_SCMD32_OP_STSCFG				0x01
#define LG2XX_SCMD32_OP_SBMT				0x03
#define LG2XX_SCMD32_OP_VMID				0x0F
#define LG2XX_SCMD32_OP_WB32				0x10
#define LG2XX_SCMD32_OP_WB64				0x11 /* writeback operation */
#define LG2XX_SCMD32_OP_ENVT				0x12
#define LG2XX_SCMD32_OP_INTR				0x14
#define LG2XX_SCMD32_OP_DBAR				0x16
#define LG2XX_SCMD32_OP_IB				0x80
#define LG2XX_SCMD32_OP_WREG				0x81
#define LG2XX_SCMD32_OP_POLL				0x83
#define LG2XX_SCMD32_OP_VM_BIND				0x84
#define LG2XX_SCMD32_OP_MAP_PROCESS			0xe0 /* map process */
#define LG2XX_SCMD32_OP_MAP_QUEUE			0xe2 /* map user queue, pass mqd (memory queue descriptor) to firmware */
#define LG2XX_SCMD32_OP_UNMAP_QUEUE			0xe3 /* unmap specified (by queue id) user queue */
#define LG2XX_SCMD32_OP_DOORBELL			0xe4 /* press doorbell for specified (by queue id) user queue */

/* INT CFG */
#define LG2XX_INT_CLEAR	0x80000000
#define LG2XX_INT_AFULL	0x20000000
#define LG2XX_INT_ACK1  0x40000000

#define LG2XX_INT_CFG0_SIZE_256		(0x8 << 4)
#define LG2XX_INT_CFG0_SIZE_1024	(0xa << 4)
#define LG2XX_INT_CFG0_SIZE_2048	(0xb << 4)
#define LG210_INT_CFG0_SIZE_1024	(0x9 << 4)
#define LG210_INT_CFG0_SIZE_2048	(0xa << 4)
#define LG2XX_INT_CFG0_ENABLE		(0x1 << 0)
#define LG2XX_INT_CFG0_DISABLE		(0x0 << 0)
#define LG2XX_INT_CFG0_UMAP		(0x1 << 8)
#define LG2XX_INT_CFG0_MAP		(0x0 << 8)
#define LG2XX_INT_CFG0_VMID		(0x0 << 9)

/* SC/EC/132_REG */
#define LG2XX_EC132_OFFSET		(0x100000)
#define LG2XX_SC132_DOORBELL_REG	(0x8)

#define LG2XX_DPM_STATE_READY		(0x80000000)
/*
 * IRQS.
 */

struct loonggpu_flip_work {
	struct delayed_work		flip_work;
	struct work_struct		unpin_work;
	struct loonggpu_device		*adev;
	int				crtc_id;
	u32				target_vblank;
	uint64_t			base;
	struct drm_pending_vblank_event *event;
	struct loonggpu_bo		*old_abo;
	struct dma_fence		*excl;
	unsigned			shared_count;
	struct dma_fence		**shared;
	struct dma_fence_cb		cb;
	bool				async;
};


/*
 * CP & rings.
 */

struct loonggpu_ib {
	struct loonggpu_sa_bo		*sa_bo;
	uint32_t			length_dw;
	uint64_t			gpu_addr;
	uint32_t			*ptr;
	uint32_t			flags;
};

extern const struct drm_sched_backend_ops loonggpu_sched_ops;

/*
 * Queue manager
 */
struct loonggpu_queue_mapper {
	int 		hw_ip;
	struct mutex	lock;
	/* protected by lock */
	struct loonggpu_ring *queue_map[LOONGGPU_MAX_RINGS];
};

struct loonggpu_queue_mgr {
	struct loonggpu_queue_mapper mapper[LOONGGPU_MAX_IP_NUM];
};

int loonggpu_queue_mgr_init(struct loonggpu_device *adev,
			  struct loonggpu_queue_mgr *mgr);
int loonggpu_queue_mgr_fini(struct loonggpu_device *adev,
			  struct loonggpu_queue_mgr *mgr);
int loonggpu_queue_mgr_map(struct loonggpu_device *adev,
			 struct loonggpu_queue_mgr *mgr,
			 u32 hw_ip, u32 instance, u32 ring,
			 struct loonggpu_ring **out_ring);

/*
 * context related structures
 */

struct loonggpu_ctx_ring {
	uint64_t		sequence;
	struct dma_fence	**fences;
	struct drm_sched_entity	entity;
};

struct loonggpu_ctx {
	struct kref		refcount;
	struct loonggpu_device    *adev;
	struct loonggpu_queue_mgr queue_mgr;
	unsigned		reset_counter;
	unsigned        reset_counter_query;
	uint32_t		vram_lost_counter;
	spinlock_t		ring_lock;
	struct dma_fence	**fences;
	struct loonggpu_ctx_ring	rings[LOONGGPU_MAX_RINGS];
	bool			preamble_presented;
	enum drm_sched_priority init_priority;
	enum drm_sched_priority override_priority;
	struct mutex            lock;
	atomic_t	guilty;
};

struct loonggpu_ctx_mgr {
	struct loonggpu_device	*adev;
	struct mutex		lock;
	/* protected by lock */
	struct idr		ctx_handles;
};

struct loonggpu_ctx *loonggpu_ctx_get(struct loonggpu_fpriv *fpriv, uint32_t id);
int loonggpu_ctx_put(struct loonggpu_ctx *ctx);

int loonggpu_ctx_add_fence(struct loonggpu_ctx *ctx, struct loonggpu_ring *ring,
			      struct dma_fence *fence, uint64_t *seq);
struct dma_fence *loonggpu_ctx_get_fence(struct loonggpu_ctx *ctx,
				   struct loonggpu_ring *ring, uint64_t seq);
void loonggpu_ctx_priority_override(struct loonggpu_ctx *ctx,
				  enum drm_sched_priority priority);

int loonggpu_ctx_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *filp);

int loonggpu_ctx_wait_prev_fence(struct loonggpu_ctx *ctx, unsigned ring_id);

void loonggpu_ctx_mgr_init(struct loonggpu_ctx_mgr *mgr);
void loonggpu_ctx_mgr_entity_fini(struct loonggpu_ctx_mgr *mgr);
void loonggpu_ctx_mgr_entity_flush(struct loonggpu_ctx_mgr *mgr);
void loonggpu_ctx_mgr_fini(struct loonggpu_ctx_mgr *mgr);


/*
 * file private structure
 */

struct loonggpu_fpriv {
	struct loonggpu_vm	vm;
	struct loonggpu_bo_va	*prt_va;
	struct loonggpu_bo_va	*csa_va;
	struct mutex		bo_list_lock;
	struct idr		bo_list_handles;
	struct loonggpu_ctx_mgr	ctx_mgr;
};

struct loonggpu_rlc_funcs {
	void (*enter_safe_mode)(struct loonggpu_device *adev);
	void (*exit_safe_mode)(struct loonggpu_device *adev);
};

struct loonggpu_rlc {
	/* for power gating */
	struct loonggpu_bo	*save_restore_obj;
	uint64_t		save_restore_gpu_addr;
	volatile uint32_t	*sr_ptr;
	const u32               *reg_list;
	u32                     reg_list_size;
	/* for clear state */
	struct loonggpu_bo	*clear_state_obj;
	uint64_t		clear_state_gpu_addr;
	volatile uint32_t	*cs_ptr;
	const struct cs_section_def   *cs_data;
	u32                     clear_state_size;
	/* for cp tables */
	struct loonggpu_bo	*cp_table_obj;
	uint64_t		cp_table_gpu_addr;
	volatile uint32_t	*cp_table_ptr;
	u32                     cp_table_size;

	/* safe mode for updating CG/PG state */
	bool in_safe_mode;
	const struct loonggpu_rlc_funcs *funcs;

	/* for firmware data */
	u32 save_and_restore_offset;
	u32 clear_state_descriptor_offset;
	u32 avail_scratch_ram_locations;
	u32 reg_restore_list_size;
	u32 reg_list_format_start;
	u32 reg_list_format_separate_start;
	u32 starting_offsets_start;
	u32 reg_list_format_size_bytes;
	u32 reg_list_size_bytes;
	u32 reg_list_format_direct_reg_list_length;
	u32 save_restore_list_cntl_size_bytes;
	u32 save_restore_list_gpm_size_bytes;
	u32 save_restore_list_srm_size_bytes;

	u32 *register_list_format;
	u32 *register_restore;
	u8 *save_restore_list_cntl;
	u8 *save_restore_list_gpm;
	u8 *save_restore_list_srm;

	bool is_rlc_v2_1;
};

#define LOONGGPU_MAX_COMPUTE_QUEUES KGD_MAX_QUEUES

struct loonggpu_mec {
	struct loonggpu_bo		*hpd_eop_obj;
	u64			hpd_eop_gpu_addr;
	struct loonggpu_bo		*mec_fw_obj;
	u64			mec_fw_gpu_addr;
	u32 			num_mec;
	u32 			num_pipe_per_mec;
	u32 			num_queue_per_pipe;
	void			*mqd_backup[LOONGGPU_MAX_COMPUTE_RINGS + 1];

	/* These are the resources for which loonggpu takes ownership */
	DECLARE_BITMAP(queue_bitmap, LOONGGPU_MAX_COMPUTE_QUEUES);
};

/*
 * GFX configurations
 */
#define LOONGGPU_GFX_MAX_SE 4
#define LOONGGPU_GFX_MAX_SH_PER_SE 2

struct loonggpu_rb_config {
	uint32_t rb_backend_disable;
	uint32_t user_rb_backend_disable;
	uint32_t raster_config;
	uint32_t raster_config_1;
};

struct gb_addr_config {
	uint16_t pipe_interleave_size;
	uint8_t num_pipes;
	uint8_t max_compress_frags;
	uint8_t num_banks;
	uint8_t num_se;
	uint8_t num_rb_per_se;
};

struct loonggpu_gfx_config {
	unsigned max_shader_engines;
	unsigned max_tile_pipes;
	unsigned max_cu_per_sh;
	unsigned max_sh_per_se;
	unsigned max_backends_per_se;
	unsigned max_texture_channel_caches;
	unsigned max_gprs;
	unsigned max_gs_threads;
	unsigned max_hw_contexts;
	unsigned sc_prim_fifo_size_frontend;
	unsigned sc_prim_fifo_size_backend;
	unsigned sc_hiz_tile_fifo_size;
	unsigned sc_earlyz_tile_fifo_size;

	unsigned num_tile_pipes;
	unsigned backend_enable_mask;
	unsigned mem_max_burst_length_bytes;
	unsigned mem_row_size_in_kb;
	unsigned shader_engine_tile_size;
	unsigned num_gpus;
	unsigned multi_gpu_tile_size;
	unsigned mc_arb_ramcfg;
	unsigned gb_addr_config;
	unsigned num_rbs;
	unsigned gs_vgt_table_depth;
	unsigned gs_prim_buffer_depth;

	uint32_t tile_mode_array[32];
	uint32_t macrotile_mode_array[16];

	struct gb_addr_config gb_addr_config_fields;
	struct loonggpu_rb_config rb_config[LOONGGPU_GFX_MAX_SE][LOONGGPU_GFX_MAX_SH_PER_SE];

	/* gfx configure feature */
	uint32_t double_offchip_lds_buf;
	/* cached value of DB_DEBUG2 */
	uint32_t db_debug2;
};

struct loonggpu_cu_info {
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t wave_front_size;
	uint32_t max_scratch_slots_per_cu;
	uint32_t lds_size;

	/* total active CU number */
	uint32_t number;
	uint32_t ao_cu_mask;
	uint32_t ao_cu_bitmap[4][4];
	uint32_t bitmap[4][4];
};

struct loonggpu_gfx_funcs {
	/* get the gpu clock counter */
	uint64_t (*get_gpu_clock_counter)(struct loonggpu_device *adev);
	void (*read_wave_data)(struct loonggpu_device *adev, uint32_t simd, uint32_t wave, uint32_t *dst, int *no_fields);
	void (*read_wave_vgprs)(struct loonggpu_device *adev, uint32_t simd, uint32_t wave, uint32_t thread, uint32_t start, uint32_t size, uint32_t *dst);
	void (*read_wave_sgprs)(struct loonggpu_device *adev, uint32_t simd, uint32_t wave, uint32_t start, uint32_t size, uint32_t *dst);
	void (*select_me_pipe_q)(struct loonggpu_device *adev, u32 me, u32 pipe, u32 queue);
};

struct sq_work {
	struct work_struct	work;
	unsigned ih_data;
};

struct loonggpu_bpipe {
	struct loonggpu_ring ring;
	struct loonggpu_irq_src eop_irq;
	struct drm_sched_entity entity;
};

struct loonggpu_dpipe {
	struct loonggpu_ring ring;
	struct loonggpu_irq_src eop_irq;
};

struct loonggpu_epipe {
	struct loonggpu_ring ring;
	struct loonggpu_irq_src eop_irq;
};

struct loonggpu_gfx {
	struct mutex			gpu_clock_mutex;
	struct loonggpu_gfx_config		config;
	struct loonggpu_rlc		rlc;
	struct loonggpu_mec		mec;
	const struct firmware		*cp_fw;	/* CP firmware */
	uint32_t			cp_fw_version;
	uint32_t			cp_feature_version;
	struct loonggpu_ring		gfx_ring[LOONGGPU_MAX_GFX_RINGS];
	unsigned			num_gfx_rings;
	struct loonggpu_irq_src		eop_irq;

	/* gfx status */
	uint32_t			gfx_current_status;
	/* ce ram size*/
	unsigned			ce_ram_size;
	struct loonggpu_cu_info		cu_info;
	const struct loonggpu_gfx_funcs	*funcs;

	/* s3/s4 mask */
	bool                            in_suspend;
};

int loonggpu_ib_get(struct loonggpu_device *adev, struct loonggpu_vm *vm,
		  unsigned size, struct loonggpu_ib *ib);
void loonggpu_ib_free(struct loonggpu_device *adev, struct loonggpu_ib *ib,
		    struct dma_fence *f);
int loonggpu_ib_schedule(struct loonggpu_ring *ring, unsigned num_ibs,
		       struct loonggpu_ib *ibs, struct loonggpu_job *job,
		       struct dma_fence **f);
int loonggpu_ib_pool_init(struct loonggpu_device *adev);
void loonggpu_ib_pool_fini(struct loonggpu_device *adev);
int loonggpu_ib_ring_tests(struct loonggpu_device *adev);

/*
 * CS.
 */
struct loonggpu_cs_chunk {
	uint32_t		chunk_id;
	uint32_t		length_dw;
	void			*kdata;
};

struct loonggpu_cs_parser {
	struct loonggpu_device	*adev;
	struct drm_file		*filp;
	struct loonggpu_ctx	*ctx;

	/* chunks */
	unsigned		nchunks;
	struct loonggpu_cs_chunk	*chunks;

	/* scheduler job object */
	struct loonggpu_job	*job;
	struct loonggpu_ring	*ring;

	/* buffer objects */
	struct ww_acquire_ctx		ticket;
	struct loonggpu_bo_list		*bo_list;
	struct loonggpu_mn		*mn;
	struct loonggpu_bo_list_entry	vm_pd;
	struct list_head		validated;
	struct dma_fence		*fence;
	uint64_t			bytes_moved_threshold;
	uint64_t			bytes_moved_vis_threshold;
	uint64_t			bytes_moved;
	uint64_t			bytes_moved_vis;
	struct loonggpu_bo_list_entry	*evictable;

	/* user fence */
	struct loonggpu_bo_list_entry	uf_entry;

	unsigned			num_post_deps;
	struct loonggpu_cs_post_dep	*post_deps;
};

static inline u32 loonggpu_get_ib_value(struct loonggpu_cs_parser *p,
				      uint32_t ib_idx, int idx)
{
	return p->job->ibs[ib_idx].ptr[idx];
}

static inline void loonggpu_set_ib_value(struct loonggpu_cs_parser *p,
				       uint32_t ib_idx, int idx,
				       uint32_t value)
{
	p->job->ibs[ib_idx].ptr[idx] = value;
}

/*
 * Writeback
 */
#define LOONGGPU_MAX_WB 128	/* Reserve at most 128 WB slots for loonggpu-owned rings. */

struct loonggpu_wb {
	struct loonggpu_bo	*wb_obj;
	volatile uint32_t	*wb;
	uint64_t		gpu_addr;
	u32			num_wb;	/* Number of wb slots actually reserved for loonggpu. */
	unsigned long		used[DIV_ROUND_UP(LOONGGPU_MAX_WB, BITS_PER_LONG)];
};

int loonggpu_device_wb_get(struct loonggpu_device *adev, u32 *wb);
void loonggpu_device_wb_free(struct loonggpu_device *adev, u32 wb);

/*
 * XDMA
 */
struct loonggpu_xdma_instance {
	/* SDMA firmware */
	const struct firmware	*fw;
	uint32_t		fw_version;
	uint32_t		feature_version;

	struct loonggpu_ring	ring;
	bool			burst_nop;
};

struct loonggpu_xdma {
	struct loonggpu_xdma_instance instance[LOONGGPU_MAX_XDMA_INSTANCES];
	struct loonggpu_irq_src	trap_irq;
	struct loonggpu_irq_src	illegal_inst_irq;
	int			num_instances;
};

enum loong_dpm_sensors {
	LOONGGPU_DPM_SENSOR_GFX_SCLK = 0,
	LOONGGPU_DPM_SENSOR_GFX_COUNTER,
	LOONGGPU_DPM_SENSOR_GPU_LOAD,
	LOONGGPU_DPM_SENSOR_GFX_MCLK,
	LOONGGPU_DPM_SENSOR_GPU_TEMP,
	LOONGGPU_DPM_SENSOR_GFX_PCNT,
	LOONGGPU_DPM_SENSOR_GFX_EVNT,
	LOONGGPU_DPM_SENSOR_GPU_POWER_LEVEL,
};

enum loonggpu_dpm_clock_type {
	DPM_SCLK,
	DPM_MCLK,
};

struct loonggpu_dpm_funcs {
	int (*print_clock_levels)(void *handle, enum loonggpu_dpm_clock_type type, char *buf);
	int (*force_clock_level)(void *handle, enum loonggpu_dpm_clock_type type, uint32_t mask);
	int (*read_sensor)(void *handle, int idx, void *value, int *size);
	int (*update_state)(void *handle);
	int (*set_capture_flags)(void *handle, uint32_t value);
	int (*get_capture_flags)(void *handle, char *buf);
};

enum loonggpu_dpm_forced_level {
	LOONGGPU_DPM_FORCED_LEVEL_AUTO = 0x1,
	LOONGGPU_DPM_FORCED_LEVEL_MANUAL = 0x2,
	LOONGGPU_DPM_FORCED_LEVEL_LOW = 0x4,
	LOONGGPU_DPM_FORCED_LEVEL_HIGH = 0x8,
};

enum loonggpu_pm_state_type {
	POWER_STATE_TYPE_ONDEMAND = 0,
};

struct loonggpu_ps {

};

#define DVFS_MAX_REGULAR_DPM_NUMBER 0x8
#define DVFS_MAX_DPM_AVG_TIMES 2
#define DVFS_MAX_EVNT_REG_NUMS 17
#define DVFS_MAX_PCNT_REG_NUMS 32

#define LOONGGPU_DVFS_CAPTURE_EVNT (0x1ULL << 0)
#define LOONGGPU_DVFS_CAPTURE_PCNT (0x1ULL << 1)

struct loonggpu_dvfs_level {
	bool enabled;
	u32 value;
	u32 param1;
	u32 level;
};

struct loonggpu_dvfs_single_table {
	u32 count;
	struct loonggpu_dvfs_level dvfs_levels[DVFS_MAX_REGULAR_DPM_NUMBER];
};

struct loonggpu_dvfs {
	int num_ps;
	struct loonggpu_ps *current_ps;
	struct loonggpu_ps *requested_ps;
	struct loonggpu_ps *boot_ps;

	struct loonggpu_dvfs_single_table sclk_table;
	uint32_t			  max_sclk;
	struct loonggpu_dvfs_single_table mclk_table;
	uint32_t			  max_mclk;

	uint32_t 			capture_flags;
	struct hrtimer 			capture_htimer;
	bool 				capture_enable;

	uint32_t 			capture_period;
	uint32_t			*capture_data;
	uint32_t			capture_counter;
	spinlock_t			capture_lock;

	uint32_t			capture_gpu_load; // 0.01%
	uint32_t			capture_gpu_load_avg[DVFS_MAX_DPM_AVG_TIMES];

	uint32_t			cur_power_level;
	uint32_t			max_power_level;

	uint32_t			cur_mclk;

	struct loonggpu_bo		*evnt_bo; // evnt capture buffer
	uint64_t			evnt_gpu_addr;
	void *				evnt_ptr;
};

struct loonggpu_dpm {
	struct loonggpu_dvfs dvfs;
	bool bios_support;
	void *handle;
	enum loonggpu_pm_state_type state;
	enum loonggpu_dpm_forced_level forced_level;
};

struct loonggpu_pm {
	struct loonggpu_dpm dpm;

	const struct loonggpu_dpm_funcs *dpm_funcs;
	bool dpm_enabled;
	struct mutex		mutex;
};

/*
 * Firmware
 */
struct loonggpu_firmware {
	struct loonggpu_bo *fw_buf;
	unsigned int fw_size;
	unsigned int max_ucodes;
	struct loonggpu_bo *rbuf;
	struct mutex mutex;

	/* gpu info firmware data pointer */
	const struct firmware *gpu_info_fw;

	void *fw_buf_ptr;
	uint64_t fw_buf_mc;
};

/*
 * Benchmarking
 */
void loonggpu_benchmark(struct loonggpu_device *adev, int test_number);


/*
 * Testing
 */
void loonggpu_test_moves(struct loonggpu_device *adev);

/*
 * ASIC specific functions.
 */
struct loonggpu_asic_funcs {
	bool (*read_bios_from_rom)(struct loonggpu_device *adev,
				   u8 *bios, u32 length_bytes);
	int (*read_register)(struct loonggpu_device *adev, u32 se_num,
			     u32 sh_num, u32 reg_offset, u32 *value);
	void (*set_vga_state)(struct loonggpu_device *adev, bool state);
	int (*reset)(struct loonggpu_device *adev);
	/* get the reference clock */
	u32 (*get_clk)(struct loonggpu_device *adev);
	/* static power management */
	int (*get_pcie_lanes)(struct loonggpu_device *adev);
	void (*set_pcie_lanes)(struct loonggpu_device *adev, int lanes);
	/* check if the asic needs a full reset of if soft reset will work */
	bool (*need_full_reset)(struct loonggpu_device *adev);
	/* initialize doorbell layout for specific asic*/
	void (*init_doorbell_index)(struct loonggpu_device *adev);
};

/*
 * IOCTL.
 */
int loonggpu_gem_create_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *filp);
int loonggpu_bo_list_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp);

int loonggpu_gem_info_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp);
int loonggpu_gem_userptr_ioctl(struct drm_device *dev, void *data,
			struct drm_file *filp);
int loonggpu_gem_mmap_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp);
int loonggpu_gem_wait_idle_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *filp);
int loonggpu_gem_va_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *filp);
int loonggpu_gem_op_ioctl(struct drm_device *dev, void *data,
			struct drm_file *filp);
int loonggpu_cs_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int loonggpu_cs_fence_to_handle_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *filp);
int loonggpu_cs_wait_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);
int loonggpu_cs_wait_fences_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp);

int loonggpu_gem_metadata_ioctl(struct drm_device *dev, void *data,
				struct drm_file *filp);

int loonggpu_hw_sema_op_ioctl(struct drm_device *dev, void *data, struct drm_file *filp);

int loonggpu_get_meta_bo_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *filp);
int loonggpu_get_meta_info_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *filp);

/* VRAM scratch page for HDP bug, default vram page */
struct loonggpu_vram_scratch {
	struct loonggpu_bo		*robj;
	volatile uint32_t		*ptr;
	u64				gpu_addr;
};

/*
 * Firmware VRAM reservation
 */
struct loonggpu_fw_vram_usage {
	u64 start_offset;
	u64 size;
	struct loonggpu_bo *reserved_bo;
	void *va;
};

/*
 * Core structure, functions and helpers.
 */
typedef uint32_t (*loonggpu_rreg_t)(struct loonggpu_device*, uint32_t);
typedef void (*loonggpu_wreg_t)(struct loonggpu_device*, uint32_t, uint32_t);

typedef uint32_t (*loonggpu_block_rreg_t)(struct loonggpu_device*, uint32_t, uint32_t);
typedef void (*loonggpu_block_wreg_t)(struct loonggpu_device*, uint32_t, uint32_t, uint32_t);

#define LOONGGPU_RESET_MAGIC_NUM 64
struct loonggpu_device {
	struct device			*dev;
	struct drm_device		*ddev;
	struct pci_dev			*pdev;
	struct pci_dev			*loongson_dc;
	u8				dc_revision;
	u8				chip;

	/* ASIC */
	enum loonggpu_family_type	family_type;
	unsigned long			flags;
	int				usec_timeout;
	const struct loonggpu_asic_funcs	*asic_funcs;
	bool				shutdown;
	bool				need_dma32;
	bool				need_swiotlb;
	bool				accel_working;
	struct work_struct		reset_work;
	struct notifier_block		acpi_nb;
	struct loonggpu_debugfs		debugfs[LOONGGPU_DEBUGFS_MAX_COMPONENTS];
	unsigned			debugfs_count;
#if defined(CONFIG_DEBUG_FS)
	struct dentry			*debugfs_regs[LOONGGPU_DEBUGFS_MAX_COMPONENTS];
#endif
	struct mutex			srbm_mutex;
	/* GRBM index mutex. Protects concurrent access to GRBM index */
	struct mutex                    grbm_idx_mutex;
	struct dev_pm_domain		vga_pm_domain;
	bool				have_disp_power_ref;

	/* BIOS */
	bool				is_atom_fw;
	uint8_t				*bios;
	uint32_t			bios_size;
	struct loonggpu_bo		*stolen_vga_memory;
	uint32_t			bios_scratch_reg_offset;
	uint32_t			bios_scratch[LOONGGPU_BIOS_NUM_SCRATCH];

	/* Register mmio */
	resource_size_t			rmmio_base;
	resource_size_t			rmmio_size;
	void __iomem			*rmmio;
	void __iomem			*rmmio_sc;

	/* loongson dc mmio */
	resource_size_t			loongson_dc_rmmio_base;
	resource_size_t			loongson_dc_rmmio_size;
	void __iomem			*loongson_dc_rmmio;
	void __iomem			*io_base;

	/* loongson hdmi hda mmio */
	void __iomem			*loongson_hdmi_hda_rmmio;
	/* protects concurrent MM_INDEX/DATA based register access */
	spinlock_t mmio_idx_lock;
	spinlock_t dc_mmio_lock;
	/* protects concurrent PCIE register access */
	spinlock_t pcie_idx_lock;

	/* protects concurrent se_cac register access */
	spinlock_t se_cac_idx_lock;
	loonggpu_rreg_t			se_cac_rreg;
	loonggpu_wreg_t			se_cac_wreg;

	/* clock/pll info */
	struct loonggpu_clock            clock;
	unsigned int 			dp_status[2];

	/* MC */
	struct loonggpu_gmc		gmc;
	struct loonggpu_gart		gart;
	struct loonggpu_zip_meta	zip_meta;
	dma_addr_t			dummy_page_addr;
	struct loonggpu_vm_manager	vm_manager;

	/* memory management */
	struct loonggpu_mman		mman;
	struct loonggpu_vram_scratch	vram_scratch;
	struct loonggpu_wb		wb;
	atomic64_t			num_bytes_moved;
	atomic64_t			num_evictions;
	atomic64_t			num_vram_cpu_page_faults;
	atomic_t			gpu_reset_counter;
	atomic_t			vram_lost_counter;

	struct loonggpu_hw_sema_mgr hw_sema_mgr;

	/* data for buffer migration throttling */
	struct {
		spinlock_t		lock;
		s64			last_update_us;
		s64			accum_us; /* accumulated microseconds */
		s64			accum_us_vis; /* for visible VRAM */
		u32			log2_max_MBps;
	} mm_stats;

	struct loonggpu_dc			*dc;
	struct loonggpu_mode_info		mode_info;
	struct loonggpu_dc_i2c		*i2c[2];
	struct delayed_work		hotplug_work;
	struct loonggpu_irq_src		vsync_irq;
	struct loonggpu_irq_src		i2c_irq;
	struct loonggpu_irq_src		hpd_irq;
	struct loonggpu_irq_src		dc_irq_msg;

	/* rings */
	u64				fence_context;
	unsigned			num_rings;
	struct loonggpu_ring		*rings[LOONGGPU_MAX_RINGS];
	bool				ib_pool_ready;
	struct loonggpu_sa_manager	ring_tmp_bo;

	/* interrupts */
	struct loonggpu_irq		irq;

	/* HPD */
	int				vga_hpd_status;

	u32				cg_flags;
	u32				pg_flags;

	/* gfx */
	struct loonggpu_gfx		gfx;

	/* bpipe */
	struct loonggpu_bpipe 		bpipe;

	/* dpipe */
	struct loonggpu_dpipe 		dpipe;

	/* epipe */
	struct loonggpu_epipe 		epipe;

	/* xdma */
	struct loonggpu_xdma		xdma;

	/* pm */
	struct loonggpu_pm		pm;

	/* KCD */
	struct loonggpu_kcd_dev		kcd;

	/* firmwares */
	struct loonggpu_firmware		firmware;

	struct loonggpu_ip_block          ip_blocks[LOONGGPU_MAX_IP_NUM];
	int				num_ip_blocks;
	struct mutex	mn_lock;
	DECLARE_HASHTABLE(mn_hash, 7);

	/* tracking pinned memory */
	atomic64_t vram_pin_size;
	atomic64_t visible_pin_size;
	atomic64_t gart_pin_size;

	/* delayed work_func for deferring clockgating during resume */
	struct delayed_work     late_init_work;

	/* zl prior virt */
	uint32_t			reg_val_offs;
	/* firmware VRAM reservation */
	struct loonggpu_fw_vram_usage fw_vram_usage;

	/* link all shadow bo */
	struct list_head                shadow_list;
	struct mutex                    shadow_list_lock;
	/* keep an lru list of rings by HW IP */
	struct list_head		ring_lru_list;
	spinlock_t			ring_lru_list_lock;

	/* record hw reset is performed */
	bool has_hw_reset;
	u8				reset_magic[LOONGGPU_RESET_MAGIC_NUM];

	/* record last mm index being written through WREG32*/
	unsigned long last_mm_index;
	bool                            in_gpu_reset;
	struct mutex  lock_reset;

	struct loongson_vbios *vbios;
	bool cursor_showed;
	bool clone_mode;
	int cursor_crtc_id;
	bool inited;

	uint64_t	unique_id;
	struct loonggpu_doorbell		doorbell;
	struct loonggpu_doorbell_index doorbell_index;
	struct dc_reg *dc_reg;
	uint32_t xdma_cb_wptr_offset;
	uint32_t xdma_cb_rptr_offset;
	uint32_t gfx_cb_wptr_offset;
	uint32_t gfx_cb_rptr_offset;
	bool wc_support;
};

#define gdc_reg adev->dc_reg

struct dc_reg *loonggpu_get_dc_reg(struct pci_dev *pdev);

static inline struct loonggpu_device *drm_to_adev(struct drm_device *ddev)
{
	return ddev->dev_private;
}

static inline struct drm_device *adev_to_drm(struct loonggpu_device *adev)
{
	return adev->ddev;
}

static inline struct loonggpu_device *loonggpu_ttm_adev(
			struct ttm_device *bdev)
{
	return container_of(bdev, struct loonggpu_device, mman.bdev);
}

int loonggpu_device_init(struct loonggpu_device *adev,
		       struct drm_device *ddev,
		       struct pci_dev *pdev,
		       uint32_t flags);
void loonggpu_device_fini(struct loonggpu_device *adev);
int loonggpu_gpu_wait_for_idle(struct loonggpu_device *adev);

uint64_t loonggpu_cmd_exec(struct loonggpu_device *adev, uint32_t cmd,
			uint32_t arg0, uint32_t arg1);

uint32_t loonggpu_mm_rreg(struct loonggpu_device *adev, uint32_t reg,
			uint32_t acc_flags);
void loonggpu_mm_wreg(struct loonggpu_device *adev, uint32_t reg, uint32_t v);
void loonggpu_mm_wreg8(struct loonggpu_device *adev, uint32_t offset, uint8_t value);
uint8_t loonggpu_mm_rreg8(struct loonggpu_device *adev, uint32_t offset);

/*
 * Registers read & write functions.
 */

#define LOONGGPU_REGS_IDX       (1<<0)
#define LOONGGPU_REGS_NO_KIQ    (1<<1)

#define RREG32_NO_KIQ(reg) loonggpu_mm_rreg(adev, (reg), LOONGGPU_REGS_NO_KIQ)
#define WREG32_NO_KIQ(reg, v) loonggpu_mm_wreg(adev, (reg), (v))

#define RREG8(reg) loonggpu_mm_rreg8(adev, (reg))
#define WREG8(reg, v) loonggpu_mm_wreg8(adev, (reg), (v))

#define RREG32(reg) loonggpu_mm_rreg(adev, (reg), 0)
#define RREG32_IDX(reg) loonggpu_mm_rreg(adev, (reg), LOONGGPU_REGS_IDX)
#define DREG32(reg) printk(KERN_INFO "REGISTER: " #reg " : 0x%08X\n", loonggpu_mm_rreg(adev, (reg), 0))
#define WREG32(reg, v) loonggpu_mm_wreg(adev, (reg), (v))
#define WREG32_IDX(reg, v) loonggpu_mm_wreg(adev, (reg), (v))
#define REG_SET(FIELD, v) (((v) << FIELD##_SHIFT) & FIELD##_MASK)
#define REG_GET(FIELD, v) (((v) << FIELD##_SHIFT) & FIELD##_MASK)
#define RREG32_SE_CAC(reg) adev->se_cac_rreg(adev, (reg))
#define WREG32_SE_CAC(reg, v) adev->se_cac_wreg(adev, (reg), (v))
#define WREG32_P(reg, val, mask)				\
	do {							\
		uint32_t tmp_ = RREG32(reg);			\
		tmp_ &= (mask);					\
		tmp_ |= ((val) & ~(mask));			\
		WREG32(reg, tmp_);				\
	} while (0)
#define WREG32_AND(reg, and) WREG32_P(reg, 0, and)
#define WREG32_OR(reg, or) WREG32_P(reg, or, ~(or))
#define WREG32_PLL_P(reg, val, mask)				\
	do {							\
		uint32_t tmp_ = RREG32_PLL(reg);		\
		tmp_ &= (mask);					\
		tmp_ |= ((val) & ~(mask));			\
		WREG32_PLL(reg, tmp_);				\
	} while (0)
#define DREG32_SYS(sqf, adev, reg) seq_printf((sqf), #reg " : 0x%08X\n", loonggpu_mm_rreg((adev), (reg), false))

#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field) reg##__##field##_MASK

#define REG_SET_FIELD(orig_val, reg, field, field_val)			\
	(((orig_val) & ~REG_FIELD_MASK(reg, field)) |			\
	 (REG_FIELD_MASK(reg, field) & ((field_val) << REG_FIELD_SHIFT(reg, field))))

#define REG_GET_FIELD(value, reg, field)				\
	(((value) & REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))

#define WREG32_FIELD(reg, field, val)	\
	WREG32(mm##reg, (RREG32(mm##reg) & ~REG_FIELD_MASK(reg, field)) | (val) << REG_FIELD_SHIFT(reg, field))

#define WREG32_FIELD_OFFSET(reg, offset, field, val)	\
	WREG32(mm##reg + offset, (RREG32(mm##reg + offset) & ~REG_FIELD_MASK(reg, field)) | (val) << REG_FIELD_SHIFT(reg, field))

/*
 * BIOS helpers.
 */
#define RBIOS8(i) (adev->bios[i])
#define RBIOS16(i) (RBIOS8(i) | (RBIOS8((i)+1) << 8))
#define RBIOS32(i) ((RBIOS16(i)) | (RBIOS16((i)+2) << 16))

static inline struct loonggpu_xdma_instance *
loonggpu_get_xdma_instance(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;
	int i;

	for (i = 0; i < adev->xdma.num_instances; i++)
		if (&adev->xdma.instance[i].ring == ring)
			break;

	if (i < LOONGGPU_MAX_XDMA_INSTANCES)
		return &adev->xdma.instance[i];
	else
		return NULL;
}

/*
 * ASICs macro.
 */
#define loonggpu_asic_set_vga_state(adev, state) ((adev)->asic_funcs->set_vga_state((adev), (state)))
#define loonggpu_asic_reset(adev) ((adev)->asic_funcs->reset((adev)))
#define loonggpu_asic_get_clk(adev) ((adev)->asic_funcs->get_clk((adev)))
#define loonggpu_get_pcie_lanes(adev) ((adev)->asic_funcs->get_pcie_lanes((adev)))
#define loonggpu_set_pcie_lanes(adev, l) ((adev)->asic_funcs->set_pcie_lanes((adev), (l)))
#define loonggpu_asic_read_bios_from_rom(adev, b, l) ((adev)->asic_funcs->read_bios_from_rom((adev), (b), (l)))
#define loonggpu_asic_read_register(adev, se, sh, offset, v) ((adev)->asic_funcs->read_register((adev), (se), (sh), (offset), (v)))
#define loonggpu_asic_need_full_reset(adev) ((adev)->asic_funcs->need_full_reset((adev)))
#define loonggpu_asic_init_doorbell_index(adev) (adev)->asic_funcs->init_doorbell_index((adev))
#define loonggpu_gmc_flush_gpu_tlb(adev, vmid) ((adev)->gmc.gmc_funcs->flush_gpu_tlb((adev), (vmid)))
#define loonggpu_gmc_flush_gpu_retry(adev, vmid, port) ((adev)->gmc.gmc_funcs->flush_gpu_retry((adev), (vmid), (port)))
#define loonggpu_gmc_emit_flush_gpu_tlb(r, vmid, addr) ((r)->adev->gmc.gmc_funcs->emit_flush_gpu_tlb((r), (vmid), (addr)))
#define loonggpu_gmc_emit_pasid_mapping(r, vmid, pasid) ((r)->adev->gmc.gmc_funcs->emit_pasid_mapping((r), (vmid), (pasid)))
#define loonggpu_gmc_set_pte_pde(adev, pt, idx, addr, flags) ((adev)->gmc.gmc_funcs->set_pte_pde((adev), (pt), (idx), (addr), (flags)))
#define loonggpu_gmc_get_vm_pde(adev, level, dst, flags) ((adev)->gmc.gmc_funcs->get_vm_pde((adev), (level), (dst), (flags)))
#define loonggpu_gmc_get_pte_flags(adev, flags) ((adev)->gmc.gmc_funcs->get_vm_pte_flags((adev), (flags)))
#define loonggpu_vm_copy_pte(adev, ib, pe, src, count) ((adev)->vm_manager.vm_pte_funcs->copy_pte((ib), (pe), (src), (count)))
#define loonggpu_vm_write_pte(adev, ib, pe, value, count, incr) ((adev)->vm_manager.vm_pte_funcs->write_pte((ib), (pe), (value), (count), (incr)))
#define loonggpu_vm_set_pte_pde(adev, ib, pe, addr, count, incr, flags) ((adev)->vm_manager.vm_pte_funcs->set_pte_pde((ib), (pe), (addr), (count), (incr), (flags)))
#define loonggpu_ring_parse_cs(r, p, ib) ((r)->funcs->parse_cs((p), (ib)))
#define loonggpu_ring_patch_cs_in_place(r, p, ib) ((r)->funcs->patch_cs_in_place((p), (ib)))
#define loonggpu_ring_test_ring(r) ((r)->funcs->test_ring((r)))
#define loonggpu_ring_test_cs(r, t) ((r)->funcs->test_cs((r), (t)))
#define loonggpu_ring_test_tl_ib(r, t) ((r)->funcs->test_tl_ib((r), (t)))
#define loonggpu_ring_test_ib(r, t) ((r)->funcs->test_ib((r), (t)))
#define loonggpu_ring_test_xdma(r, t) ((r)->funcs->test_xdma((r), (t)))
#define loonggpu_ring_test_bpipe(r, t) ((r)->funcs->test_bpipe((r), (t)))
#define loonggpu_ring_get_rptr(r) ((r)->funcs->get_rptr((r)))
#define loonggpu_ring_get_wptr(r) ((r)->funcs->get_wptr((r)))
#define loonggpu_ring_set_wptr(r) ((r)->funcs->set_wptr((r)))
#define loonggpu_ring_emit_ib(r, ib, vmid, c) ((r)->funcs->emit_ib((r), (ib), (vmid), (c)))
#define loonggpu_ring_emit_vm_flush(r, vmid, addr) ((r)->funcs->emit_vm_flush((r), (vmid), (addr)))
#define loonggpu_ring_emit_fence(r, addr, seq, flags) ((r)->funcs->emit_fence((r), (addr), (seq), (flags)))
#define loonggpu_ring_emit_cntxcntl(r, d) ((r)->funcs->emit_cntxcntl((r), (d)))
#define loonggpu_ring_emit_rreg(r, d) ((r)->funcs->emit_rreg((r), (d)))
#define loonggpu_ring_emit_wreg(r, d, v) ((r)->funcs->emit_wreg((r), (d), (v)))
#define loonggpu_ring_emit_reg_wait(r, d, v, m) ((r)->funcs->emit_reg_wait((r), (d), (v), (m)))
#define loonggpu_ring_emit_reg_write_reg_wait(r, d0, d1, v, m) ((r)->funcs->emit_reg_write_reg_wait((r), (d0), (d1), (v), (m)))
#define loonggpu_ring_emit_tmz(r, b) ((r)->funcs->emit_tmz((r), (b)))
#define loonggpu_ring_pad_ib(r, ib) ((r)->funcs->pad_ib((r), (ib)))
#define loonggpu_ring_init_cond_exec(r) ((r)->funcs->init_cond_exec((r)))
#define loonggpu_ring_patch_cond_exec(r, o) ((r)->funcs->patch_cond_exec((r), (o)))
#define loonggpu_ih_get_wptr(adev) ((adev)->irq.ih_funcs->get_wptr((adev)))
#define loonggpu_ih_prescreen_iv(adev) ((adev)->irq.ih_funcs->prescreen_iv((adev)))
#define loonggpu_ih_decode_iv(adev, iv) ((adev)->irq.ih_funcs->decode_iv((adev), (iv)))
#define loonggpu_ih_set_rptr(adev) ((adev)->irq.ih_funcs->set_rptr((adev)))
#define loonggpu_display_vblank_get_counter(adev, crtc) ((adev)->mode_info.funcs->vblank_get_counter((adev), (crtc)))
#define loonggpu_display_backlight_set_level(adev, e, l) ((adev)->mode_info.funcs->backlight_set_level((e), (l)))
#define loonggpu_display_backlight_get_level(adev, e) ((adev)->mode_info.funcs->backlight_get_level((e)))
#define loonggpu_display_hpd_sense(adev, h) ((adev)->mode_info.funcs->hpd_sense((adev), (h)))
#define loonggpu_display_hpd_set_polarity(adev, h) ((adev)->mode_info.funcs->hpd_set_polarity((adev), (h)))
#define loonggpu_display_page_flip(adev, crtc, base, async) ((adev)->mode_info.funcs->page_flip((adev), (crtc), (base), (async)))
#define loonggpu_display_page_flip_get_scanoutpos(adev, crtc, vbl, pos) ((adev)->mode_info.funcs->page_flip_get_scanoutpos((adev), (crtc), (vbl), (pos)))
#define loonggpu_emit_copy_buffer(adev, ib, s, d, b) ((adev)->mman.buffer_funcs->emit_copy_buffer((ib),  (s), (d), (b)))
#define loonggpu_emit_fill_buffer(adev, ib, s, d, b) ((adev)->mman.buffer_funcs->emit_fill_buffer((ib), (s), (d), (b)))
#define loonggpu_gfx_get_gpu_clock_counter(adev) ((adev)->gfx.funcs->get_gpu_clock_counter((adev)))
#define loonggpu_psp_check_fw_loading_status(adev, i) ((adev)->firmware.funcs->check_fw_loading_status((adev), (i)))
#define loonggpu_gfx_select_me_pipe_q(adev, me, pipe, q) ((adev)->gfx.funcs->select_me_pipe_q((adev), (me), (pipe), (q)))

/* Common functions */
int loonggpu_device_gpu_recover(struct loonggpu_device *adev,
			      struct loonggpu_job *job, bool force);
void loonggpu_device_pci_config_reset(struct loonggpu_device *adev);
bool loonggpu_device_need_post(struct loonggpu_device *adev);
void loonggpu_display_update_priority(struct loonggpu_device *adev);

void loonggpu_cs_report_moved_bytes(struct loonggpu_device *adev, u64 num_bytes,
				  u64 num_vis_bytes);
void loonggpu_device_vram_location(struct loonggpu_device *adev,
				 struct loonggpu_gmc *mc, u64 base);
void loonggpu_device_gart_location(struct loonggpu_device *adev,
				 struct loonggpu_gmc *mc);
int loonggpu_device_resize_fb_bar(struct loonggpu_device *adev);
void loonggpu_device_program_register_sequence(struct loonggpu_device *adev,
					     const u32 *registers,
					     const u32 array_size);
bool loonggpu_device_skip_hw_access(struct loonggpu_device *adev);
void *loonggpu_get_vram_info(struct device *dev, unsigned long *size);

/*
 * KMS
 */
extern const struct drm_ioctl_desc loonggpu_ioctls_kms[];
extern const int loonggpu_max_kms_ioctl;

int loonggpu_driver_load_kms(struct drm_device *dev, unsigned long flags);
void loonggpu_driver_unload_kms(struct drm_device *dev);
void loonggpu_driver_lastclose_kms(struct drm_device *dev);
int loonggpu_driver_open_kms(struct drm_device *dev, struct drm_file *file_priv);
void loonggpu_driver_postclose_kms(struct drm_device *dev,
				 struct drm_file *file_priv);
int loonggpu_device_ip_suspend(struct loonggpu_device *adev);
int loonggpu_device_suspend(struct drm_device *dev, bool suspend, bool fbcon);
int loonggpu_device_resume(struct drm_device *dev, bool resume, bool fbcon);
u32 loonggpu_get_vblank_counter_kms(struct drm_device *dev, unsigned int pipe);
long loonggpu_kms_compat_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg);

/*
 * functions used by loonggpu_encoder.c
 */
struct loonggpu_afmt_acr {
	u32 clock;

	int n_32khz;
	int cts_32khz;

	int n_44_1khz;
	int cts_44_1khz;

	int n_48khz;
	int cts_48khz;

};

struct loonggpu_afmt_acr loonggpu_afmt_acr(uint32_t clock);

/* loonggpu_acpi.c */
#if defined(CONFIG_ACPI)
int loonggpu_acpi_init(struct loonggpu_device *adev);
void loonggpu_acpi_fini(struct loonggpu_device *adev);
bool loonggpu_acpi_is_pcie_performance_request_supported(struct loonggpu_device *adev);
int loonggpu_acpi_pcie_performance_request(struct loonggpu_device *adev,
						u8 perf_req, bool advertise);
int loonggpu_acpi_pcie_notify_device_ready(struct loonggpu_device *adev);
#else
static inline int loonggpu_acpi_init(struct loonggpu_device *adev) { return 0; }
static inline void loonggpu_acpi_fini(struct loonggpu_device *adev) { }
#endif

int loonggpu_cs_find_mapping(struct loonggpu_cs_parser *parser,
			   uint64_t addr, struct loonggpu_bo **bo,
			   struct loonggpu_bo_va_mapping **mapping);

enum loonggpu_ib_pool_type {
	/* Normal submissions to the top of the pipeline. */
	LOONGGPU_IB_POOL_DELAYED,
	/* Immediate submissions to the bottom of the pipeline. */
	LOONGGPU_IB_POOL_IMMEDIATE,
	/* Direct submission to the ring buffer during init and reset. */
	LOONGGPU_IB_POOL_DIRECT,
	LOONGGPU_IB_POOL_MAX
};

#include "loonggpu_object.h"
#endif
