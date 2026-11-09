#ifndef __LOONGGPU_BPIPE_H__
#define __LOONGGPU_BPIPE_H__

struct bpipe_box {
    int x1;
    int y1;
    int x2;
    int y2;
};

struct bpipe_buffer {
    u64 addr;	/* va */
    int width;
    int height;
    int pitch;
	int32_t bpp;
	uint32_t tiling;
};

#define BPIPE_TILING_ARRAY_MODE_LINEAR	1
#define BPIPE_TILING_ARRAY_MODE_TILED4	2
#define BPIPE_TILING_ARRAY_MODE_TILED8	3

int bpipe_map_vram_buffer(struct loonggpu_bo *bo,
					uint64_t offset, uint64_t size,
					unsigned window,
					struct loonggpu_ring *ring,
					uint64_t *addr);

int bpipe_draw_cs_copy(struct loonggpu_ring *ring,
						struct bpipe_box *sbox,
						struct bpipe_box *dbox,
						struct bpipe_buffer *sbo,
						struct bpipe_buffer *dbo);

extern const struct loonggpu_ip_block_version bpipe_ip_block;

struct loonggpu_device;

#endif /*__BPIPE_H__*/
