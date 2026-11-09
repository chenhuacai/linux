#ifndef __LOONGGPU_XDMA_H__
#define __LOONGGPU_XDMA_H__

#define LOONGGPU_XDMA_FLAG_UMAP 0x20000

extern const struct loonggpu_ip_block_version xdma_ip_block;

void xdma_ring_test_xdma_loop(struct loonggpu_ring *ring, long timeout);

#endif /*__LOONGGPU_XDMA_H__*/
