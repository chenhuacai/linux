#include <linux/kernel.h>
#include "loonggpu.h"
#include "loonggpu_common.h"
#include "loonggpu_cp.h"
#include "loonggpu_pipe.h"
#include "loonggpu_bpipe.h"

static void bpipe_set_ring_funcs(struct loonggpu_device *adev);
static void bpipe_set_irq_funcs(struct loonggpu_device *adev);

static uint32_t bpipe_cb_wptr_offset = 0;
static uint32_t bpipe_cb_rptr_offset = 0;

struct cbuf {
    uint64_t base                :64;

    uint32_t format              :8;
    uint32_t type                :4;
    uint32_t tilem               :4;
    uint32_t swizzle             :12;
    uint32_t swzlrev             :1;
    uint32_t pow2p               :1;
    uint32_t                     :2;

    uint32_t width               :16;
    uint32_t height              :16;

    uint32_t depth               :16;
    uint32_t pitch               :16;

    uint32_t base_layer          :12;
    uint32_t last_layer          :12;
    uint32_t base_level          :4;
    uint32_t last_level          :4;

    uint32_t level               :4;
    uint32_t align               :4;
    uint32_t                     :24;

    uint32_t                     :32;
};

struct btex {
   uint64_t  baseaddr :64;  /* texture base addr, only use 40 bit */
   /* R_028_TEX_SIZE */
   uint32_t  width    :16;
   uint32_t  height   :16;
   /* R_02c_TEX_RSRC */
   uint32_t  wrap     :1;  /* 0:clamp_to_edge 1:repeat */
   uint32_t  filter   :1;  /* 0:nearest  1:linear */
   uint32_t  pattern  :1;  /* true is pattern_fill */
   uint32_t  pow2p    :1;  /* pow2pad */
   uint32_t  mtype    :4;  /* tile24, tile248, tile4, linear */
   uint32_t  dfmt     :8;
   uint32_t  pitch    :16;
};

struct bpipe_drawcall {
    /* R_000_DRAW_TYPE */
    uint32_t  draw_op  :2; /* BE_FILL, BE_CLEAR & BE_BLIT */
    uint32_t  ras_dir  :2; /* 00:left_bottom 01:left_top 10:right_bottom 11:right_top */
    uint32_t  rot_xy   :1;  /* true is rot_90 or rot_270 */
    uint32_t  cst_fill :1; /* fill const_color no need src */
    uint32_t  maskblit :1; /* tex blend enable */
    uint32_t  tex_rot  :1; /* tex blend rot_xy */
    uint32_t  tex_swizzle :12; /* tex_swizzle */
    uint32_t  mask_swizzle:12; /* tex_mask_swizzle */
    /* R_004_DRAW_NULL */
    uint32_t  single_gpc:1;
    uint32_t  camask_en :1;
    uint32_t  wmask_en  :1;
    uint32_t  gpc_block_x : 3;
    uint32_t  gpc_block_y : 3;
    uint32_t           :23;

    /* R_008_POINT_START */
    uint32_t  box_x0   :16; /* 16 bit uint */
    uint32_t  box_y0   :16;

    /* R_00c_POINT_END */
    uint32_t  box_x1   :16;
    uint32_t  box_y1   :16;

    /* R_010_TCOORD_S */
    uint32_t  tex_s    :32; /* S1.16.15 1bit sign, 16bit integer, 15bit factor */
    /* R_014_TCOORD_T */
    uint32_t  tex_t    :32;
    /* R_018_TCOORD_DS */
    uint32_t  tex_ds   :32; /* S1.7.24  1bit sign, 7 bit integer 24bit factor */
    /* R_01c_TCOORD_DT */
    uint32_t  tex_dt   :32;

    /* R_020_TADD R_LO */
    /* R_024_TADD R_HI */
    struct btex bpipe_tex;
    /* BUFCFG */
    struct cbuf bpipe_buf;

    /* BLEND_OP */
    /* R_050_BLEND_STATE_RT */
    uint32_t blend_enable     :1;
    uint32_t rgb_func         :3;
    uint32_t rgb_src_factor   :5;
    uint32_t rgb_dst_factor   :5;
    uint32_t alpha_func       :3;
    uint32_t alpha_src_factor :5;
    uint32_t alpha_dst_factor :5;
    uint32_t colormask        :4;
    uint32_t                  :1;

    /* LOGIC_OP */
    /* R_054_BLEND_LOGIC_CONFIG */
    uint32_t indep_blend_en   :1;
    uint32_t dualsrc_blend_en :1;
    uint32_t                  :1;
    uint32_t logicop_en       :1;
    uint32_t logicop_func     :4;
    uint32_t                  :24;

    /* blend_color: 64*4 */
    /* R_058_BLEND_COLOR_LO */
    /* R_05c_BLEND_COLOR_HI */
    uint64_t blend_color      :64;
    /* R_060_TCOORD_S */
    uint32_t  mask_tex_s    :32; /* S1.16.15 1bit sign, 16bit integer, 15bit factor */
    /* R_064_TCOORD_T */
    uint32_t  mask_tex_t    :32;
    /* R_068_TCOORD_DS */
    uint32_t  mask_tex_ds   :32; /* S1.7.24  1bit sign, 7 bit integer 24bit factor */
    /* R_06c_TCOORD_DT */
    uint32_t  mask_tex_dt   :32;

    /* R_070_TADD R_LO */
    /* R_074_TADD R_HI */
    struct btex bpipe_tex_mask;

    /* R_080_BLEND_STATE_RT */
    uint32_t mask_blend_enable     :1;
    uint32_t mask_rgb_func         :3;
    uint32_t mask_rgb_src_factor   :5;
    uint32_t mask_rgb_dst_factor   :5;
    uint32_t mask_alpha_func       :3;
    uint32_t mask_alpha_src_factor :5;
    uint32_t mask_alpha_dst_factor :5;
    uint32_t mask_colormask        :4;
    uint32_t                       :1;

    uint32_t                       :32;
    /* fill_color */
    /* R_088_FILLPAT_COLOR_DATA -
     * R_25c_FILLPAT_COLOR_DATA
	 */
    uint32_t fillpat_color[128];
};

#define BPIPE_CMD(cmd, op0, op1) (((cmd) & 0xff) | (((op0) & 0xfff) << 8) | (((op1) & 0xfff) << 20))

#define PIPE_CMD_NULL 0x00
#define PIPE_CMD_CFGW 0x01   /* config write word, {null32, data32} */
#define PIPE_CMD_CFGD 0x02   /* config write dword, {data64} */
#define PIPE_CMD_DRAW 0x03   /* DTG: {null64} */
#define PIPE_CMD_CINV 0x04
#define PIPE_CMD_SYNC 0x05   /* flush cache */
#define PIPE_CMD_WAIT 0x06   /* wait at stage[i] for EOP's message */
#define PIPE_CMD_SEND 0x07   /* message send     obsolete */
#define PIPE_CMD_CTXD 0x08   /* context dump {vaddr64} */
#define PIPE_CMD_CTXL 0x09   /* context load {vaddr64}, including perf-cnt */
#define PIPE_CMD_CTXR 0x0a   /* context reset */
#define PIPE_CMD_CTXS 0x0b
#define PIPE_CMD_STSD 0x0c   /* status dump, {vaddr64} */
#define PIPE_CMD_STSR 0x0d   /* status reset */
#define PIPE_CMD_VMID 0x0f   /* update domain ID, shall disable for IBs */
#define PIPE_CMD_WR32 0x10   /* write 32bit data at EOP */
#define PIPE_CMD_WR64 0x11   /* write 64bit data at EOP */
#define PIPE_CMD_EVNT 0x12   /* sample event counter in the pipe */
#define PIPE_CMD_TIME 0x13   /* sample clock counter in the pipe */
#define PIPE_CMD_INTR 0x14   /* raise interrupt at EOP */
#define PIPE_CMD_MSIW 0x15   /* send MSI interrupt at EOP */
#define PIPE_CMD_DBAR 0x16   /* write barrier, wait writes done and read last addr back */
#define PIPE_CMD_SEMI 0x18
#define PIPE_CMD_SEMW 0x19
#define PIPE_CMD_SEMR 0x1a

/* color buffer blend ctrl */
#define BLEND_ONE                        0x01
#define BLEND_SRC_COLOR                  0x02
#define BLEND_SRC_ALPHA                  0x03
#define BLEND_DST_ALPHA                  0x04
#define BLEND_DST_COLOR                  0x05
#define BLEND_SRC_ALPHA_SATURATE         0x06
#define BLEND_CONSTANT_COLOR             0x07
#define BLEND_CONSTANT_ALPHA             0x08
#define BLEND_SRC1_COLOR                 0x09  /* no define */
#define BLEND_SRC1_ALPHA                 0x0a  /* no define */
#define BLEND_ZERO                       0x11
#define BLEND_ONE_MINUS_SRC_COLOR        0x12
#define BLEND_ONE_MINUS_SRC_ALPHA        0x13
#define BLEND_ONE_MINUS_DST_ALPHA        0x14
#define BLEND_ONE_MINUS_DST_COLOR        0x15
#define BLEND_ONE_MINUS_CONSTANT_COLOR   0x17
#define BLEND_ONE_MINUS_CONSTANT_ALPHA   0x18
#define BLEND_INV_SRC1_COLOR             0x19  /* no define */
#define BLEND_INV_SRC1_ALPHA             0x1a  /* no define */

#define FMT_8                           0x01
#define FMT_16                          0x02
#define FMT_16_16                       0x05
#define FMT_10_11_11                    0x06
#define FMT_11_11_10                    0x07
#define FMT_10_10_10_2                  0x08
#define FMT_2_10_10_10                  0x09
#define FMT_8_8_8_8                     0x0a
#define FMT_16_16_16_16                 0x0c
#define FMT_5_6_5                       0x10
#define FMT_1_5_5_5                     0x11
#define FMT_5_5_5_1                     0x12
#define FMT_4_4_4_4                     0x13

#define COLOR_8_8_8_8               0x0
#define COLOR_8                     20
#define COLOR_2_10_10_10            30
#define COLOR_1_5_5_5               33
#define COLOR_5_6_5                 34
#define COLOR_10_10_10_2            44
#define COLOR_5_5_5_1               45

#define SQ_SEL_X                        0x00
#define SQ_SEL_Y                        0x01
#define SQ_SEL_Z                        0x02
#define SQ_SEL_W                        0x03
#define SQ_SEL_0                        0x04
#define SQ_SEL_1                        0x05

#define BPIPE_WRAP_CLAMP_TO_EDGE    0x0
#define BPIPE_WRAP_REPEAT           0x1

#define BPIPE_FILTER_NEAREST        0x0
#define BPIPE_FILTER_LINEAR         0x1

#define BPIPE_BE_FILL               0
#define BPIPE_BE_BLIT               2

#define INT32_MAX	S32_MAX
#define INT64_MAX	S64_MAX

static int bpipe_gart_map_vram(struct loonggpu_device *adev, uint64_t offset,
		    int pages, uint64_t addr, uint64_t flags, void *dst)
{
	uint64_t page_base;
	unsigned i, t;

	if (!adev->gart.ready) {
		WARN(1, "trying to bind memory to uninitialized GART !\n");
		return -EINVAL;
	}

	page_base = addr;
	t = offset / LOONGGPU_GPU_PAGE_SIZE;

	for (i = 0; i < pages; i++, t++) {
		loonggpu_gmc_set_pte_pde(adev, dst, t, page_base, flags);
		page_base += LOONGGPU_GPU_PAGE_SIZE;
	}
	return 0;
}

static int bpipe_gart_bind_vram(struct loonggpu_device *adev, uint64_t offset,
		     int pages, uint64_t addr, uint64_t flags)
{
	int r;

	if (!adev->gart.ready) {
		WARN(1, "trying to bind memory to uninitialized GART !\n");
		return -EINVAL;
	}

	if (!adev->gart.ptr)
		return -EINVAL;

	r = bpipe_gart_map_vram(adev, offset, pages, addr, flags, adev->gart.ptr);
	if (r)
		return r;

	mb();
	/*
	 * FIXME: vram map frequently flush gpu tlb.
	*/
	loonggpu_gmc_flush_gpu_tlb(adev, 0);
	return 0;
}

int bpipe_map_vram_buffer(struct loonggpu_bo *bo,
					uint64_t offset, uint64_t size,
					unsigned window,
					struct loonggpu_ring *ring,
					uint64_t *addr)
{
	struct loonggpu_device *adev = ring->adev;
	lg_ttm_mem_t *mem = lg_tbo_to_mem(&bo->tbo);
	struct drm_mm_node __maybe_unused *mm_node;
	struct loonggpu_res_cursor __maybe_unused src_mm;
	uint64_t node_start, node_size, page_offset;
	unsigned num_pages = 0;
	uint64_t flags = 0;

	if (mem->mem_type != TTM_PL_VRAM)
		return -1;

	if (!(bo->flags & LOONGGPU_GEM_CREATE_VRAM_CONTIGUOUS))
		return -1;
	loonggpu_res_first(mem, 0, loonggpu_bo_size(bo), &src_mm);

	if (src_mm.node == NULL)
		return -1;

	node_size = src_mm.size;
	if (offset >= node_size)
		return -1;

	node_size -= offset;

	if (size > node_size)
		size = node_size;

	node_start = loonggpu_bo_gpu_offset(bo) + offset;
	page_offset = node_start & (PAGE_SIZE - 1);
	num_pages = PFN_UP(size + page_offset);

	if (num_pages > LOONGGPU_VRAM_MAX_TRANSFER_SIZE)
		num_pages = LOONGGPU_VRAM_MAX_TRANSFER_SIZE;

	flags = loonggpu_ttm_tt_pte_flags(adev, bo->tbo.ttm, mem);

	*addr = adev->gmc.gart_start + adev->gmc.gart_size;
	*addr += (u64)window * LOONGGPU_VRAM_MAX_TRANSFER_SIZE *
		LOONGGPU_GPU_PAGE_SIZE;

	return bpipe_gart_bind_vram(adev, *addr, num_pages, node_start, flags);
}

static void bpipe_drawcall_emit_draw_packet(struct loonggpu_ring *ring,
						struct loonggpu_ib *ib, struct bpipe_drawcall *dc)
{
	int i = 0, size = sizeof(*dc) / 4;
	unsigned *ptr = (unsigned *)dc;
	int no_skip = 0;

	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_CTXR, 0, 0);
	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_CFGW, 0x0, size);

	for (i = 0; i < size; i++)
		ib->ptr[ib->length_dw++] = *(ptr + i);

	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_DRAW, no_skip, 0);
}

static void bpipe_emit_gfx_flush(struct loonggpu_ring *ring, struct loonggpu_ib *ib)
{
	int i = 0;
	uint32_t pad_count;
	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_SYNC, ((0 << 8) | 0xf), 0);
	pad_count = (8 - (ib->length_dw & 0x7)) % 8;
	for (i = 0; i < pad_count; i++)
		ib->ptr[ib->length_dw++] = GSPKT(LG2XX_SCMD32_OP_NOP, 0);
}

static int32_t div_to_q8_24(int a, int b)
{
	int64_t a64, b64;
	int64_t scaled, x;
	const int64_t scale_24 = 1LL << 24;
	const int64_t denom = 1000000LL;
	int64_t product, fixed64;
	const int64_t min_q8_24 = -(int64_t)128 << 24;
	const int64_t max_q8_24 = ((int64_t)128 << 24) - 1;
	int sign = 1;

	if (!b)
		return -1;

	if (a < 0) {
		sign = -sign;
		a = -a;
	}

	if (b < 0) {
		sign = -sign;
		b = -b;
	}

	a64 = (int64_t)a;
	b64 = (int64_t)b;

	if (a64 > (INT64_MAX / 10000000LL))
		return -1;

	scaled = (a64 * 10000000LL) / b64;
	x = (scaled + 5) / 10;

	if (!x)
		return 0;

	if (x > (INT64_MAX - denom / 2) / scale_24)
		return -1;

	product = x * scale_24;
	fixed64 = (product + denom / 2) / denom;

	if (sign < 0) {
		if (fixed64 > (int64_t)INT32_MAX + 1)
			return -1;
		fixed64 = -fixed64;
	} else {
		if (fixed64 > INT32_MAX)
			return -1;
	}

	if (fixed64 < min_q8_24 || fixed64 > max_q8_24)
		return -1;

	return (int32_t)fixed64;
}

static void bpipe_drawcall_dump_fillpat_color(struct bpipe_drawcall *dc)
{
	uint32_t i = 0, j = 0;
	for (i = 0; i < ARRAY_SIZE(dc->fillpat_color); i++) {
		DRM_INFO("0x%x \n", dc->fillpat_color[i]);
		if (j == 16) {
			DRM_INFO("\n");
			j = 0;
		} else
			j++;
	}
}

static void bpipe_drawcall_dump(struct bpipe_drawcall *dc)
{
	DRM_INFO("dc->draw_op   =[%d]\n", dc->draw_op);
	DRM_INFO("dc->ras_dir   =[%d]\n", dc->ras_dir);
	DRM_INFO("dc->rot_xy   =[%d]\n", dc->rot_xy);
	DRM_INFO("dc->cst_fill   =[%d]\n", dc->cst_fill);
	DRM_INFO("dc->maskblit   =[%d]\n", dc->maskblit);
	DRM_INFO("dc->tex_rot   =[%d]\n", dc->tex_rot);
	DRM_INFO("dc->tex_swizzle   =[%d]\n", dc->tex_swizzle);
	DRM_INFO("dc->mask_swizzle   =[%d]\n", dc->mask_swizzle);
	DRM_INFO("dc->single_gpc   =[%d]\n", dc->single_gpc);
	DRM_INFO("dc->camask_en   =[%d]\n", dc->camask_en);
	DRM_INFO("dc->wmask_en   =[%d]\n", dc->wmask_en);
	DRM_INFO("dc->gpc_block_x   =[%d]\n", dc->gpc_block_x);
	DRM_INFO("dc->gpc_block_y   =[%d]\n", dc->gpc_block_y);
	DRM_INFO("dc->box_x0   =[%d]\n", dc->box_x0);
	DRM_INFO("dc->box_y0   =[%d]\n", dc->box_y0);
	DRM_INFO("dc->box_x1   =[%d]\n", dc->box_x1);
	DRM_INFO("dc->box_y1   =[%d]\n", dc->box_y1);
	DRM_INFO("dc->tex_s   =[0x%x]\n", dc->tex_s >> 15);
	DRM_INFO("dc->tex_t   =[0x%x]\n", dc->tex_t >> 15);
	DRM_INFO("dc->tex_ds   =[0x%x]\n", dc->tex_ds);
	DRM_INFO("dc->tex_dt   =[0x%x]\n", dc->tex_dt);
	DRM_INFO("dc->bpipe_tex.baseaddr  =[0x%llx]\n", dc->bpipe_tex.baseaddr);
	DRM_INFO("dc->bpipe_tex.width   =[%d]\n", dc->bpipe_tex.width);
	DRM_INFO("dc->bpipe_tex.height   =[%d]\n", dc->bpipe_tex.height);
	DRM_INFO("dc->bpipe_tex.wrap   =[%d]\n", dc->bpipe_tex.wrap);
	DRM_INFO("dc->bpipe_tex.filter   =[%d]\n", dc->bpipe_tex.filter);
	DRM_INFO("dc->bpipe_tex.pattern   =[%d]\n", dc->bpipe_tex.pattern);
	DRM_INFO("dc->bpipe_tex.pow2p   =[%d]\n", dc->bpipe_tex.pow2p);
	DRM_INFO("dc->bpipe_tex.mtype   =[%d]\n", dc->bpipe_tex.mtype);
	DRM_INFO("dc->bpipe_tex.dfmt   =[%d]\n", dc->bpipe_tex.dfmt);
	DRM_INFO("dc->bpipe_tex.pitch   =[%d]\n", dc->bpipe_tex.pitch);
	DRM_INFO("dc->bpipe_buf.base   =[0x%llx]\n", dc->bpipe_buf.base);
	DRM_INFO("dc->bpipe_buf.format   =[%d]\n", dc->bpipe_buf.format);
	DRM_INFO("dc->bpipe_buf.type   =[%d]\n", dc->bpipe_buf.type);
	DRM_INFO("dc->bpipe_buf.tilem   =[%d]\n", dc->bpipe_buf.tilem);
	DRM_INFO("dc->bpipe_buf.swizzle   =[%d]\n", dc->bpipe_buf.swizzle);
	DRM_INFO("dc->bpipe_buf.swzlrev   =[%d]\n", dc->bpipe_buf.swzlrev);
	DRM_INFO("dc->bpipe_buf.pow2p   =[%d]\n", dc->bpipe_buf.pow2p);
	DRM_INFO("dc->bpipe_buf.width   =[%d]\n", dc->bpipe_buf.width);
	DRM_INFO("dc->bpipe_buf.height   =[%d]\n", dc->bpipe_buf.height);
	DRM_INFO("dc->bpipe_buf.depth   =[%d]\n", dc->bpipe_buf.depth);
	DRM_INFO("dc->bpipe_buf.pitch   =[%d]\n", dc->bpipe_buf.pitch);
	DRM_INFO("dc->bpipe_buf.base_layer   =[%d]\n", dc->bpipe_buf.base_layer);
	DRM_INFO("dc->bpipe_buf.last_layer   =[%d]\n", dc->bpipe_buf.last_layer);
	DRM_INFO("dc->bpipe_buf.base_level   =[%d]\n", dc->bpipe_buf.base_level);
	DRM_INFO("dc->bpipe_buf.last_level   =[%d]\n", dc->bpipe_buf.last_level);
	DRM_INFO("dc->bpipe_buf.level   =[%d]\n", dc->bpipe_buf.level);
	DRM_INFO("dc->bpipe_buf.align   =[%d]\n", dc->bpipe_buf.align);
	DRM_INFO("dc->blend_enable   =[%d]\n", dc->blend_enable);
	DRM_INFO("dc->rgb_func   =[%d]\n", dc->rgb_func);
	DRM_INFO("dc->rgb_src_factor   =[%d]\n", dc->rgb_src_factor);
	DRM_INFO("dc->rgb_dst_factor   =[%d]\n", dc->rgb_dst_factor);
	DRM_INFO("dc->alpha_func   =[%d]\n", dc->alpha_func);
	DRM_INFO("dc->alpha_src_factor   =[%d]\n", dc->alpha_src_factor);
	DRM_INFO("dc->alpha_dst_factor   =[%d]\n", dc->alpha_dst_factor);
	DRM_INFO("dc->colormask   =[%d]\n", dc->colormask);
	DRM_INFO("dc->indep_blend_en   =[%d]\n", dc->indep_blend_en);
	DRM_INFO("dc->dualsrc_blend_en   =[%d]\n", dc->dualsrc_blend_en);
	DRM_INFO("dc->logicop_en   =[%d]\n", dc->logicop_en);
	DRM_INFO("dc->logicop_func   =[%d]\n", dc->logicop_func);
	DRM_INFO("dc->blend_color   =[0x%llx]\n", dc->blend_color);
	DRM_INFO("dc->mask_tex_s   =[0x%x]\n", dc->mask_tex_s >> 15);
	DRM_INFO("dc->mask_tex_t   =[0x%x]\n", dc->mask_tex_t >> 15);
	DRM_INFO("dc->mask_tex_ds   =[0x%x]\n", dc->mask_tex_ds);
	DRM_INFO("dc->mask_tex_dt   =[0x%x]\n", dc->mask_tex_dt);
	DRM_INFO("dc->bpipe_tex_mask.baseaddr  =[0x%llx]\n", dc->bpipe_tex_mask.baseaddr);
	DRM_INFO("dc->bpipe_tex_mask.width   =[%d]\n", dc->bpipe_tex_mask.width);
	DRM_INFO("dc->bpipe_tex_mask.height   =[%d]\n", dc->bpipe_tex_mask.height);
	DRM_INFO("dc->bpipe_tex_mask.wrap   =[%d]\n", dc->bpipe_tex_mask.wrap);
	DRM_INFO("dc->bpipe_tex_mask.filter   =[%d]\n", dc->bpipe_tex_mask.filter);
	DRM_INFO("dc->bpipe_tex_mask.pattern   =[%d]\n", dc->bpipe_tex_mask.pattern);
	DRM_INFO("dc->bpipe_tex_mask.pow2p   =[%d]\n", dc->bpipe_tex_mask.pow2p);
	DRM_INFO("dc->bpipe_tex_mask.mtype   =[%d]\n", dc->bpipe_tex_mask.mtype);
	DRM_INFO("dc->bpipe_tex_mask.dfmt   =[%d]\n", dc->bpipe_tex_mask.dfmt);
	DRM_INFO("dc->bpipe_tex_mask.pitch   =[%d]\n", dc->bpipe_tex_mask.pitch);
	DRM_INFO("dc->mask_blend_enable   =[%d]\n", dc->mask_blend_enable);
	DRM_INFO("dc->mask_rgb_func   =[%d]\n", dc->mask_rgb_func);
	DRM_INFO("dc->mask_rgb_src_factor   =[%d]\n", dc->mask_rgb_src_factor);
	DRM_INFO("dc->mask_rgb_dst_factor   =[%d]\n", dc->mask_rgb_dst_factor);
	DRM_INFO("dc->mask_alpha_func   =[%d]\n", dc->mask_alpha_func);
	DRM_INFO("dc->mask_alpha_src_factor   =[%d]\n", dc->mask_alpha_src_factor);
	DRM_INFO("dc->mask_alpha_dst_factor   =[%d]\n", dc->mask_alpha_dst_factor);
	DRM_INFO("dc->mask_colormask   =[%d]\n", dc->mask_colormask);
	bpipe_drawcall_dump_fillpat_color(dc);
}

int bpipe_draw_cs_copy(struct loonggpu_ring *ring,
						struct bpipe_box *sbox,
						struct bpipe_box *dbox,
						struct bpipe_buffer *sbo,
						struct bpipe_buffer *dbo)
{
	int r = 0;
	struct dma_fence *f = NULL;
	struct loonggpu_device *adev;
	struct loonggpu_job *job;
	struct loonggpu_ib *ib;
	int sw, sh, dw, dh;
	struct bpipe_drawcall *dc = NULL;
	int i = 0, size = sizeof(*dc) / 4;
	int no_skip = 0;
	uint32_t pad_count;
	uint32_t pix_r, pix_g, pix_b, pix_a;

	adev = ring->adev;

	if (!ring->ready)
		return -1;

	if (adev->family_type < CHIP_LG200)
		return -1;

	sw = sbox->x2 - sbox->x1;
	sh = sbox->y2 - sbox->y1;
	dw = dbox->x2 - dbox->x1;
	dh = dbox->y2 - dbox->y1;

	if (!sw || !sh || !dw || !dh)
		return -1;

	r = loonggpu_job_alloc_with_ib(adev, sizeof(*dc) + 256, &job);
	if (r)
		return r;

	ib = &job->ibs[0];

	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_CTXR, 0, 0);
	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_CFGW, 0x0, size);

	dc = (struct bpipe_drawcall *)&ib->ptr[ib->length_dw];

	memset(dc, 0, sizeof(*dc));

	dc->draw_op = BPIPE_BE_BLIT;
	dc->single_gpc = 0;
	dc->cst_fill = 0;
	dc->colormask = 0xf;	/* A|B|G|R */

	dc->rgb_src_factor = BLEND_CONSTANT_COLOR;
	dc->rgb_dst_factor = BLEND_ZERO;
	dc->alpha_src_factor = BLEND_CONSTANT_ALPHA;
	dc->alpha_dst_factor = BLEND_ZERO;

	/* color buffer */
	if (dbo->bpp == 8) {
		dc->bpipe_buf.format = COLOR_8; /* A */
		pix_r = SQ_SEL_0; /* R */
		pix_g = SQ_SEL_0; /* G */
		pix_b = SQ_SEL_0; /* B */
		pix_a = SQ_SEL_X; /* A */
	} else if (dbo->bpp == 16) {
		dc->bpipe_buf.format = COLOR_5_6_5; /* RGB */
		pix_r = SQ_SEL_X; /* R */
		pix_g = SQ_SEL_Y; /* G */
		pix_b = SQ_SEL_Z; /* B */
		pix_a = SQ_SEL_1; /* A */
	} else {
		dc->bpipe_buf.format = COLOR_8_8_8_8; /* ARGB */
		pix_r = SQ_SEL_Z; /* R */
		pix_g = SQ_SEL_Y; /* G */
		pix_b = SQ_SEL_X; /* B */
		pix_a = SQ_SEL_W; /* A */
	}

	dc->bpipe_buf.swizzle = (pix_a << 9 | pix_b << 6
							| pix_g << 3 | pix_r << 0);
	dc->bpipe_buf.base = dbo->addr;
	dc->bpipe_buf.width = dbo->width;	/* pixels */
	dc->bpipe_buf.height = dbo->height;	/* pixels */
	dc->bpipe_buf.pitch = dbo->pitch;	/* pixels */
	dc->bpipe_buf.depth = 1;

	if (dbo->tiling == BPIPE_TILING_ARRAY_MODE_TILED4)
		dc->bpipe_buf.tilem = 2;
	else if (dbo->tiling == BPIPE_TILING_ARRAY_MODE_TILED8)
		dc->bpipe_buf.tilem = 3;
	else
		dc->bpipe_buf.tilem = 0;

	dc->ras_dir = 0;	/* left to right & top to bottom */

	/* texture */
	if (sbo->bpp == 8) {
		dc->bpipe_tex.dfmt = FMT_8;
		pix_r = SQ_SEL_1; /* R */
		pix_g = SQ_SEL_1; /* G */
		pix_b = SQ_SEL_1; /* B */
		pix_a = SQ_SEL_X; /* A */
	} else if (sbo->bpp == 16) {
		dc->bpipe_tex.dfmt = FMT_5_6_5;
		pix_r = SQ_SEL_X; /* R */
		pix_g = SQ_SEL_Y; /* G */
		pix_b = SQ_SEL_Z; /* B */
		pix_a = SQ_SEL_1; /* A */
	} else {
		dc->bpipe_tex.dfmt = FMT_8_8_8_8;
		pix_r = SQ_SEL_Z; /* R */
		pix_g = SQ_SEL_Y; /* G */
		pix_b = SQ_SEL_X; /* B */
		pix_a = SQ_SEL_W; /* A */
	}

	dc->tex_swizzle = (pix_a << 9 | pix_b << 6
						| pix_g << 3 | pix_r << 0);
	dc->bpipe_tex.baseaddr = sbo->addr;
	dc->bpipe_tex.width = sbo->width;
	dc->bpipe_tex.height = sbo->height;
	dc->bpipe_tex.pitch = sbo->pitch;
	dc->bpipe_tex.filter = BPIPE_FILTER_LINEAR;
	dc->bpipe_tex.wrap = BPIPE_WRAP_CLAMP_TO_EDGE;

	if (sbo->tiling == BPIPE_TILING_ARRAY_MODE_TILED4)
		dc->bpipe_tex.mtype = 2;
	else if (sbo->tiling == BPIPE_TILING_ARRAY_MODE_TILED8)
		dc->bpipe_tex.mtype = 3;
	else
		dc->bpipe_tex.mtype = 0;

	dc->box_x0 = dbox->x1;
	dc->box_y0 = dbox->y1;
	dc->box_x1 = dbox->x2 - 1;
	dc->box_y1 = dbox->y2 - 1;

	dc->tex_s = ((sbox->x1 & 0xffff) << 15);
	dc->tex_t = ((sbox->y1 & 0xffff) << 15);

	dc->tex_ds = (sw == dw) ?
				(1 << 24) : div_to_q8_24(sw, dw);
	dc->tex_dt = (sh == dh) ?
				(1 << 24) : div_to_q8_24(sh, dh);

	if (0)
		bpipe_drawcall_dump(dc);

	ib->length_dw += size;

	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_DRAW, no_skip, 0);
	ib->ptr[ib->length_dw++] = BPIPE_CMD(PIPE_CMD_SYNC, ((0 << 8) | 0xf), 0);

	pad_count = (8 - (ib->length_dw & 0x7)) % 8;
	for (i = 0; i < pad_count; i++)
		ib->ptr[ib->length_dw++] = GSPKT(LG2XX_SCMD32_OP_NOP, 0);

	r = loonggpu_job_submit(job, &adev->bpipe.entity,
						LOONGGPU_FENCE_OWNER_UNDEFINED, &f);
	if (r)
		goto job_free;

	r = dma_fence_wait_timeout(f, false, msecs_to_jiffies(5000));
	if (r == 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout timed out\r\n", __FUNCTION__);
		r = -ETIMEDOUT;
		goto fence_free;
	} else if (r < 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout failed\r\n", __FUNCTION__);
		goto fence_free;
	}

	dma_fence_put(f);

	return 0;

fence_free:
	dma_fence_put(f);
job_free:
	loonggpu_job_free(job);
	return r;
}

 static int bpipe_draw_cs_test_gtt_copy(struct loonggpu_ring *ring)
{
	int r = 0;
	struct dma_fence *f = NULL;
	struct loonggpu_device *adev;
	struct loonggpu_ib ib;
	u64 dst_gpu_addr, src_gpu_addr;
	uint32_t *dst_cpu_ptr, *src_cpu_ptr;
	struct loonggpu_bo *dst_bo, *src_bo;
	unsigned int size;
	struct bpipe_drawcall drawcall = {0};
	struct bpipe_drawcall *dc = &drawcall;
	int i;
	int zoom = 1;	/* 0 - no zoom(1:1)  1 - zoom in(1:2)  2 - zoom out(1:0.5) */

	adev = ring->adev;

	if (!ring->ready)
		return -1;

	if (adev->family_type < CHIP_LG200)
		return -1;

	src_bo = NULL;
	size = LOONGGPU_GPU_PAGE_SIZE;
	r = loonggpu_bo_create_kernel(adev, size,
						LOONGGPU_GPU_PAGE_SIZE, LOONGGPU_GEM_DOMAIN_GTT,
						&src_bo, &src_gpu_addr, (void **)&src_cpu_ptr);
	if (r) {
		DRM_ERROR("%s : loonggpu_bo_create_kernel src error\r\n", __FUNCTION__);
		return r;
	}

	dst_bo = NULL;
	r = loonggpu_bo_create_kernel(adev, size,
						LOONGGPU_GPU_PAGE_SIZE, LOONGGPU_GEM_DOMAIN_GTT,
						&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
	if (r) {
		DRM_ERROR("%s : loonggpu_bo_create_kernel dst error\r\n", __FUNCTION__);
		goto src_bo_free;
	}

	 /* clear bo */
	for (i = 0; i < size / sizeof(*src_cpu_ptr); i++)
		src_cpu_ptr[i] = (i + 1);

	memset((void *)dst_cpu_ptr, 0, size);

	dc->draw_op = BPIPE_BE_BLIT;
	dc->colormask = 0xf;
	dc->ras_dir = 0;

	dc->cst_fill = 0;
	dc->single_gpc = 0;

	dc->bpipe_buf.width = 8;
	dc->bpipe_buf.height = 8;
	dc->bpipe_buf.depth = 1;
	dc->bpipe_buf.pitch = 8;
	dc->bpipe_buf.swizzle = 0x688;

	dc->rgb_src_factor = BLEND_CONSTANT_COLOR;
	dc->rgb_dst_factor = BLEND_ZERO;
	dc->alpha_src_factor = BLEND_CONSTANT_ALPHA;
	dc->alpha_dst_factor = BLEND_ZERO;

	dc->bpipe_tex.width = 4;
	dc->bpipe_tex.height = 4;
	dc->bpipe_tex.pitch = 8;
	dc->bpipe_tex.wrap = BPIPE_WRAP_CLAMP_TO_EDGE;
	dc->bpipe_tex.filter = BPIPE_FILTER_LINEAR;
	dc->bpipe_tex.mtype = 0;
	dc->bpipe_tex.dfmt = FMT_8_8_8_8;
	dc->tex_swizzle = 0x688;

	dc->tex_s = 0;
	dc->tex_t = 0;

	if (zoom == 1) {
		/* 1:2 zoom in */
		dc->tex_ds = div_to_q8_24(1, 2);
		dc->tex_dt = div_to_q8_24(1, 2);
	} else if (zoom == 2) {
		/* 1:0.5 zoom out */
		dc->tex_ds = div_to_q8_24(2, 1);
		dc->tex_dt = div_to_q8_24(2, 1);
	} else {
		/* 1:1 */
		dc->tex_ds = (1 << 24);
		dc->tex_dt = (1 << 24);
	}

	dc->box_x0 = 0;
	dc->box_y0 = 0;
	dc->box_x1 = 7;
	dc->box_y1 = 7;

	dc->bpipe_buf.base = dst_gpu_addr;
	dc->bpipe_tex.baseaddr = src_gpu_addr;

	memset(&ib, 0, sizeof(ib));
	r = loonggpu_ib_get(adev, NULL, sizeof(*dc) + 256, &ib);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_get error\r\n", __FUNCTION__);
		goto dst_bo_free;
	}

	bpipe_drawcall_emit_draw_packet(ring, &ib, dc);
	bpipe_emit_gfx_flush(ring, &ib);

	r = loonggpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_schedule error\r\n", __FUNCTION__);
		goto ib_free;
	}

	r = dma_fence_wait_timeout(f, false, msecs_to_jiffies(5000));
	if (r == 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout timed out\r\n", __FUNCTION__);
		r = -ETIMEDOUT;
		goto fence_free;
	} else if (r < 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout failed\r\n", __FUNCTION__);
		goto fence_free;
	}

	r = 0;

	printk("%s:\n", __FUNCTION__);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4, src_cpu_ptr, 256, false);
	printk("\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4, dst_cpu_ptr, 256, false);

fence_free:
	dma_fence_put(f);
ib_free:
	loonggpu_ib_free(adev, &ib, NULL);
dst_bo_free:
	loonggpu_bo_free_kernel(&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
src_bo_free:
	loonggpu_bo_free_kernel(&src_bo, &src_gpu_addr, (void **)&src_cpu_ptr);
	return r;
}

 static int bpipe_draw_cs_test_vram_copy(struct loonggpu_ring *ring)
{
	int r = 0;
	struct dma_fence *f = NULL;
	struct loonggpu_device *adev;
	struct loonggpu_ib ib;
	u64 dst_gpu_addr, src_gpu_addr;
	uint32_t *dst_cpu_ptr, *src_cpu_ptr;
	u64 dst_va, src_va;
	struct loonggpu_bo *dst_bo, *src_bo;
	unsigned int size;
	struct bpipe_drawcall drawcall = {0};
	struct bpipe_drawcall *dc = &drawcall;
	int i;

	adev = ring->adev;

	if (!ring->ready)
		return -1;

	if (adev->family_type < CHIP_LG200)
		return -1;

	src_bo = NULL;
	size = LOONGGPU_GPU_PAGE_SIZE;
	r = loonggpu_bo_create_kernel(adev, size,
						LOONGGPU_GPU_PAGE_SIZE, LOONGGPU_GEM_DOMAIN_VRAM,
						&src_bo, &src_gpu_addr, (void **)&src_cpu_ptr);
	if (r) {
		DRM_ERROR("%s : loonggpu_bo_create_kernel src error\r\n", __FUNCTION__);
		return r;
	}

	dst_bo = NULL;
	r = loonggpu_bo_create_kernel(adev, size,
						LOONGGPU_GPU_PAGE_SIZE, LOONGGPU_GEM_DOMAIN_VRAM,
						&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
	if (r) {
		DRM_ERROR("%s : loonggpu_bo_create_kernel dst error\r\n", __FUNCTION__);
		goto src_bo_free;
	}

	 /* clear bo */
	for (i = 0; i < size / sizeof(*src_cpu_ptr); i++)
		src_cpu_ptr[i] = (i + 1);

	memset((void *)dst_cpu_ptr, 0, size);

	r = bpipe_map_vram_buffer(src_bo, 0, size, 0, ring, &src_va);
	if (r) {
		DRM_ERROR("%s : bpipe_map_vram_buffer src error\r\n", __FUNCTION__);
		goto dst_bo_free;
	}

	r = bpipe_map_vram_buffer(dst_bo, 0, size, 1, ring, &dst_va);
	if (r) {
		DRM_ERROR("%s : bpipe_map_vram_buffer dst error\r\n", __FUNCTION__);
		goto dst_bo_free;
	}

	dc->draw_op = BPIPE_BE_BLIT;
	dc->colormask = 0xf;
	dc->ras_dir = 0;
	dc->cst_fill = 0;
	dc->single_gpc = 0;

	dc->bpipe_buf.width = 2;
	dc->bpipe_buf.height = 2;
	dc->bpipe_buf.depth = 1;
	dc->bpipe_buf.pitch = 8;
	dc->bpipe_buf.swizzle = 0x688;

	dc->rgb_src_factor = BLEND_CONSTANT_COLOR;
	dc->rgb_dst_factor = BLEND_ZERO;
	dc->alpha_src_factor = BLEND_CONSTANT_ALPHA;
	dc->alpha_dst_factor = BLEND_ZERO;

	dc->bpipe_tex.width = 2;
	dc->bpipe_tex.height = 2;
	dc->bpipe_tex.pitch = 2;
	dc->bpipe_tex.filter = 0;
	dc->bpipe_tex.mtype = 0;
	dc->bpipe_tex.dfmt = FMT_8_8_8_8;
	dc->tex_swizzle = 0x688;

	dc->tex_s = 0;
	dc->tex_t = 0;
	dc->tex_ds = (1 << 24);
	dc->tex_dt = (1 << 24);

	dc->box_x0 = 0;
	dc->box_y0 = 0;
	dc->box_x1 = 1;
	dc->box_y1 = 1;

	dc->bpipe_buf.base = dst_va;
	dc->bpipe_tex.baseaddr = src_va;

	memset(&ib, 0, sizeof(ib));
	r = loonggpu_ib_get(adev, NULL, sizeof(*dc) + 256, &ib);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_get error\r\n", __FUNCTION__);
		goto dst_bo_free;
	}

	bpipe_drawcall_emit_draw_packet(ring, &ib, dc);
	bpipe_emit_gfx_flush(ring, &ib);

	r = loonggpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_schedule error\r\n", __FUNCTION__);
		goto ib_free;
	}

	r = dma_fence_wait_timeout(f, false, msecs_to_jiffies(5000));
	if (r == 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout timed out\r\n", __FUNCTION__);
		r = -ETIMEDOUT;
		goto fence_free;
	} else if (r < 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout failed\r\n", __FUNCTION__);
		goto fence_free;
	}

	r = 0;

	printk("%s:\n", __FUNCTION__);
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4, src_cpu_ptr, 256, false);
	printk("\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4, dst_cpu_ptr, 256, false);

fence_free:
	dma_fence_put(f);
ib_free:
	loonggpu_ib_free(adev, &ib, NULL);
dst_bo_free:
	loonggpu_bo_free_kernel(&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
src_bo_free:
	loonggpu_bo_free_kernel(&src_bo, &src_gpu_addr, (void **)&src_cpu_ptr);
	return r;
}

 static int bpipe_draw_cs_test_solid(struct loonggpu_ring *ring)
{
    int r = 0;
	struct dma_fence *f = NULL;
	struct loonggpu_device *adev;
	struct loonggpu_ib ib;
	u64 dst_gpu_addr;
	uint32_t *dst_cpu_ptr;
	struct loonggpu_bo *dst_bo;
	unsigned int size;
	struct bpipe_drawcall drawcall = {0};
	struct bpipe_drawcall *dc = &drawcall;

	adev = ring->adev;

	if (!ring->ready)
		return -1;

	if (adev->family_type < CHIP_LG200)
		return -1;

	dst_bo = NULL;
	size = LOONGGPU_GPU_PAGE_SIZE;
	r = loonggpu_bo_create_kernel(adev, size,
						LOONGGPU_GPU_PAGE_SIZE, LOONGGPU_GEM_DOMAIN_GTT,
						&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
	if (r) {
		DRM_ERROR("%s : loonggpu_bo_create_kernel error\r\n", __FUNCTION__);
		return r;
	}

	memset((void *)dst_cpu_ptr, 0x0, size);

	dc->draw_op = BPIPE_BE_FILL;
	dc->cst_fill = 1;
	dc->box_x0 = 0;
	dc->box_y0 = 0;
	dc->box_x1 = 1;
	dc->box_y1 = 1;
	dc->bpipe_buf.width = 2;
	dc->bpipe_buf.height = 2;
	dc->bpipe_buf.depth = 1;
	dc->bpipe_buf.pitch = 8;
	dc->bpipe_buf.format = COLOR_8_8_8_8;
	dc->blend_enable = 1;
	dc->rgb_src_factor = BLEND_CONSTANT_COLOR;
	dc->rgb_dst_factor = BLEND_ZERO;
	dc->alpha_src_factor = BLEND_CONSTANT_ALPHA;
	dc->alpha_dst_factor = BLEND_ZERO;
	dc->colormask = 0xf;
	dc->blend_color = 0xffffffffffffffff;
	dc->bpipe_buf.swizzle = 0x688;
	dc->bpipe_buf.base = dst_gpu_addr;

	memset(&ib, 0, sizeof(ib));
	r = loonggpu_ib_get(adev, NULL, sizeof(*dc) + 256, &ib);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_get error\r\n", __FUNCTION__);
		goto dst_bo_free;
	}

	bpipe_drawcall_emit_draw_packet(ring, &ib, dc);
	bpipe_emit_gfx_flush(ring, &ib);

	r = loonggpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (r) {
		DRM_ERROR("%s : loonggpu_ib_schedule error\r\n", __FUNCTION__);
		goto ib_free;
	}

	r = dma_fence_wait_timeout(f, false, msecs_to_jiffies(5000));
	if (r == 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout timed out\r\n", __FUNCTION__);
		r = -ETIMEDOUT;
		goto fence_free;
	} else if (r < 0) {
		DRM_ERROR("%s : dma_fence_wait_timeout failed\r\n", __FUNCTION__);
		goto fence_free;
	}

	r = 0;

	printk("%s:\n", __FUNCTION__);

	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 32, 4, dst_cpu_ptr, 256, false);

fence_free:
	dma_fence_put(f);
ib_free:
	loonggpu_ib_free(adev, &ib, NULL);
dst_bo_free:
	loonggpu_bo_free_kernel(&dst_bo, &dst_gpu_addr, (void **)&dst_cpu_ptr);
	return r;
}

/**
 * bpipe_ring_test_bpipe - test bpipe on the BPIPE engine
 *
 * @ring: loonggpu_ring structure holding ring information
 *
 * Test a simple bpipe in the BPIPE ring  .
 * Returns 0 on success, error on failure.
 */
static int bpipe_ring_test_bpipe(struct loonggpu_ring *ring, long timeout)
{
	int r;

	r = bpipe_draw_cs_test_solid(ring);
	if (r)
		return r;

	r = bpipe_draw_cs_test_gtt_copy(ring);
	if (r)
		return r;

	r = bpipe_draw_cs_test_vram_copy(ring);
	if (r)
		return r;

	return r;
}

static int bpipe_entity_init(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring;
	struct drm_sched_rq *rq;
	struct drm_gpu_scheduler *sched;
	int r;

	ring = &adev->bpipe.ring;
	rq = sched->sched_rq[DRM_SCHED_PRIORITY_KERNEL];
	sched = &ring->sched;
	r = lg_drm_sched_entity_init(&adev->bpipe.entity, DRM_SCHED_PRIORITY_KERNEL,
						&sched, 1, &rq, 1, NULL);
	if (r != 0) {
		DRM_ERROR("Failed setting up BPIPE run queue.\n");
		return r;
	}

	return 0;
}

static int bpipe_sw_init(void *handle)
{
	int r;
	struct loonggpu_ring *ring;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	/* FENCE Event */
	r = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, LOONGGPU_LG200_SRCID_CP_END_OF_BPIPE, &adev->bpipe.eop_irq);
	if (r)
		return r;

	ring = &adev->bpipe.ring;
	ring->ring_obj = NULL;
	sprintf(ring->name, "bpipe");

	r = loonggpu_ring_init(adev, ring, 256, &adev->bpipe.eop_irq,
			       LOONGGPU_CP_IRQ_BPIPE_EOP);
	if (r)
		return r;

	r = bpipe_entity_init(adev);
	if (r)
		return r;

	return 0;
}

static int bpipe_sw_fini(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	drm_sched_entity_destroy(&adev->bpipe.entity);

	loonggpu_ring_fini(&adev->bpipe.ring);

	return 0;
}

static int bpipe_cp_bpipe_resume(struct loonggpu_device *adev)
{
	struct loonggpu_ring *ring;
	int r = 0;

        bpipe_cb_wptr_offset = LOONGGPU_LG2XX_BPIPE_CB_WPTR_OFFSET;
        bpipe_cb_rptr_offset = LOONGGPU_LG2XX_BPIPE_CB_RPTR_OFFSET;

	/* Set ring buffer size */
	ring = &adev->bpipe.ring;

	/* clear the ring */
	loonggpu_ring_clear_ring(ring);

	/* Initialize the ring buffer's read and write pointers */
	ring->wptr = 0;
	WREG32(bpipe_cb_wptr_offset, lower_32_bits(ring->wptr));

	/* set the RPTR */
	WREG32(bpipe_cb_rptr_offset, 0);

	mdelay(1);

	loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_BPIPE,
			LG2XX_ICMD32_SOP_BPIPE_BBQ, 0),
			lower_32_bits(ring->gpu_addr), upper_32_bits(ring->gpu_addr));
        loonggpu_cmd_exec(adev, LG2XX_ICMD32i(LG2XX_ICMD32_MOP_BPIPE,
			LG2XX_ICMD32_SOP_BPIPE_BQSZ, 0), 
                        ring->ring_size / 4, 0);

	ring->ready = true;

	return r;
}

static int bpipe_cp_resume(struct loonggpu_device *adev)
{
	int r;

	r = bpipe_cp_bpipe_resume(adev);
	if (r)
		return r;

	return 0;
}

static int bpipe_hw_init(void *handle)
{
	int r;
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	r = bpipe_cp_resume(adev);

	return r;
}

static int bpipe_hw_fini(void *handle)
{
	return 0;
}

static int bpipe_suspend(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return bpipe_hw_fini(adev);
}

static int bpipe_resume(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return bpipe_hw_init(adev);
}

static bool bpipe_is_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	return (RREG32(LOONGGPU_STATUS) == GSCMD_STS_DONE);
}

static int bpipe_wait_for_idle(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	if (loonggpu_cp_wait_done(adev) == true)
			return 0;

	return -ETIMEDOUT;
}

static int bpipe_early_init(void *handle)
{
	struct loonggpu_device *adev = (struct loonggpu_device *)handle;

	bpipe_set_ring_funcs(adev);
	bpipe_set_irq_funcs(adev);

	return 0;
}

static int bpipe_late_init(void *handle)
{
	DRM_INFO("%s bpipe drawcall size:%ld\n", __func__, sizeof(struct bpipe_drawcall));
	return 0;
}

static u64 bpipe_ring_get_rptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(bpipe_cb_rptr_offset);
}

static u64 bpipe_ring_get_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	return RREG32(bpipe_cb_wptr_offset);
}

static void bpipe_ring_set_wptr(struct loonggpu_ring *ring)
{
	struct loonggpu_device *adev = ring->adev;

	WREG32(bpipe_cb_wptr_offset, lower_32_bits(ring->wptr));
}

static int bpipe_set_eop_interrupt_state(struct loonggpu_device *adev,
					    struct loonggpu_irq_src *src,
					    unsigned type,
					    enum loonggpu_interrupt_state state)
{
	return 0;
}

static int bpipe_eop_irq(struct loonggpu_device *adev,
			    struct loonggpu_irq_src *source,
			    struct loonggpu_iv_entry *entry)
{
	u8 me_id;

	DRM_DEBUG("IH: BPIPE FENCE\n");
	me_id = (entry->ring_id & 0x0c) >> 2;

	switch (me_id) {
	case 0:
		loonggpu_fence_process(&adev->bpipe.ring);
		break;
	default:
		DRM_ERROR("loonggpu bpipe number fail 0x%x",me_id);
		break;
	}
	return 0;
}

static const struct loonggpu_ip_funcs bpipe_ip_funcs = {
	.name = "bpipe",
	.early_init = bpipe_early_init,
	.late_init = bpipe_late_init,
	.sw_init = bpipe_sw_init,
	.sw_fini = bpipe_sw_fini,
	.hw_init = bpipe_hw_init,
	.hw_fini = bpipe_hw_fini,
	.suspend = bpipe_suspend,
	.resume = bpipe_resume,
	.is_idle = bpipe_is_idle,
	.wait_for_idle = bpipe_wait_for_idle,
};

static struct loonggpu_ring_funcs bpipe_ring_funcs = {
	.type = LOONGGPU_RING_TYPE_BPIPE,
	.align_mask = 0xf,
	.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0),
	.support_64bit_ptrs = false,
	.get_rptr = bpipe_ring_get_rptr,
	.get_wptr = bpipe_ring_get_wptr,
	.set_wptr = bpipe_ring_set_wptr,
	.emit_frame_size = LOONGGPU_PIPE_EMIT_FRAME_SIZE,
	.emit_ib_size =	LOONGGPU_PIPE_EMIT_IB_SIZE, /* bpipe_ring_emit_ib */
	.emit_ib = loonggpu_pipe_ring_emit_ib,
	.emit_fence = loonggpu_pipe_ring_emit_fence,
	.emit_pipeline_sync = loonggpu_pipe_ring_emit_pipeline_sync,
	.emit_vm_flush = loonggpu_pipe_ring_emit_vm_flush,
	.test_ring = loonggpu_pipe_ring_test_ring,
	.test_cs = loonggpu_pipe_ring_test_cs,
	.test_ib = loonggpu_pipe_ring_test_ib,
	.test_bpipe = bpipe_ring_test_bpipe,
	.insert_nop = loonggpu_ring_insert_nop,
	.pad_ib = loonggpu_ring_generic_pad_ib,
	.emit_wreg = loonggpu_pipe_ring_emit_wreg,
};

static void bpipe_set_ring_funcs(struct loonggpu_device *adev)
{
        bpipe_ring_funcs.nop = LG2XX_SCMD32(LG2XX_SCMD32_OP_NOP, 0);
	adev->bpipe.ring.funcs = &bpipe_ring_funcs;
}

static const struct loonggpu_irq_src_funcs bpipe_eop_irq_funcs = {
	.set = bpipe_set_eop_interrupt_state,
	.process = bpipe_eop_irq,
};

static void bpipe_set_irq_funcs(struct loonggpu_device *adev)
{
	adev->bpipe.eop_irq.num_types = LOONGGPU_CP_IRQ_LAST;
	adev->bpipe.eop_irq.funcs = &bpipe_eop_irq_funcs;
}

const struct loonggpu_ip_block_version bpipe_ip_block = {
	.type = LOONGGPU_IP_BLOCK_TYPE_BPIPE,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &bpipe_ip_funcs,
};
