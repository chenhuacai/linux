#ifndef __LOONGGPU_SHARED_H__
#define __LOONGGPU_SHARED_H__

#include "loonggpu_family_type.h"

/*
 * Chip flags
 */
enum loonggpu_chip_flags {
	GAGPU_ASIC_MASK = 0x0000ffffUL,
	LOONGGPU_FLAGS_MASK  = 0xffff0000UL,
	LOONGGPU_IS_MOBILITY = 0x00010000UL,
	LOONGGPU_IS_APU      = 0x00020000UL,
	LOONGGPU_IS_PX       = 0x00040000UL,
	LOONGGPU_EXP_HW_SUPPORT = 0x00080000UL,
};

enum loonggpu_ip_block_type {
	LOONGGPU_IP_BLOCK_TYPE_COMMON,
	LOONGGPU_IP_BLOCK_TYPE_GMC,
	LOONGGPU_IP_BLOCK_TYPE_ZIP,
	LOONGGPU_IP_BLOCK_TYPE_IH,
	LOONGGPU_IP_BLOCK_TYPE_DCE,
	LOONGGPU_IP_BLOCK_TYPE_GFX,
	LOONGGPU_IP_BLOCK_TYPE_BPIPE,
	LOONGGPU_IP_BLOCK_TYPE_DPIPE,
	LOONGGPU_IP_BLOCK_TYPE_EPIPE,
	LOONGGPU_IP_BLOCK_TYPE_XDMA,
	LOONGGPU_IP_BLOCK_TYPE_DVFS,
};

/**
 * struct loonggpu_ip_funcs - general hooks for managing loonggpu IP Blocks
 */
struct loonggpu_ip_funcs {
	/** @name: Name of IP block */
	char *name;
	/**
	 * @early_init:
	 *
	 * sets up early driver state (pre sw_init),
	 * does not configure hw - Optional
	 */
	int (*early_init)(void *handle);
	/** @late_init: sets up late driver/hw state (post hw_init) - Optional */
	int (*late_init)(void *handle);
	/** @sw_init: sets up driver state, does not configure hw */
	int (*sw_init)(void *handle);
	/** @sw_fini: tears down driver state, does not configure hw */
	int (*sw_fini)(void *handle);
	/** @hw_init: sets up the hw state */
	int (*hw_init)(void *handle);
	/** @hw_fini: tears down the hw state */
	int (*hw_fini)(void *handle);
	/** @late_fini: final cleanup */
	void (*late_fini)(void *handle);
	/** @suspend: handles IP specific hw/sw changes for suspend */
	int (*suspend)(void *handle);
	/** @resume: handles IP specific hw/sw changes for resume */
	int (*resume)(void *handle);
	/** @is_idle: returns current IP block idle status */
	bool (*is_idle)(void *handle);
	/** @wait_for_idle: poll for idle */
	int (*wait_for_idle)(void *handle);
	/** @check_soft_reset: check soft reset the IP block */
	bool (*check_soft_reset)(void *handle);
	/** @pre_soft_reset: pre soft reset the IP block */
	int (*pre_soft_reset)(void *handle);
	/** @soft_reset: soft reset the IP block */
	int (*soft_reset)(void *handle);
	/** @post_soft_reset: post soft reset the IP block */
	int (*post_soft_reset)(void *handle);
};

extern const struct dma_buf_ops loonggpu_dmabuf_ops;

#endif /* __LOONGGPU_SHARED_H__ */
