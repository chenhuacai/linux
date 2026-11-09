#ifndef __PIPE_H__
#define __PIPE_H__

#define LOONGGPU_PIPE_EMIT_IB_SIZE 4
#define LOONGGPU_PIPE_EMIT_FRAME_SIZE                                               \
        (                                    /* maximum 215dw if count 16 IBs in */ \
         7 +                                 /* COND_EXEC */                        \
         1 +                                 /* PIPELINE_SYNC */                    \
         VI_FLUSH_GPU_TLB_NUM_WREG * 5 + 9 + /* VM_FLUSH */                         \
         5 +                                 /* FENCE for VM_FLUSH */               \
         3 +                                 /* CNTX_CTRL */                        \
         5 + 5                               /* FENCE x2 */                         \
        )

extern const struct loonggpu_ip_block_version bpipe_ip_block;
extern const struct loonggpu_ip_block_version dpipe_ip_block;
extern const struct loonggpu_ip_block_version epipe_ip_block;

struct loonggpu_device;

void loonggpu_pipe_ring_emit_ib(struct loonggpu_ring *ring,
				struct loonggpu_ib *ib,
				unsigned vmid, bool ctx_switch);
void loonggpu_pipe_ring_emit_fence(struct loonggpu_ring *ring, u64 addr,
				   u64 seq, unsigned flags);
void loonggpu_pipe_ring_emit_pipeline_sync(struct loonggpu_ring *ring);
void loonggpu_pipe_ring_emit_vm_flush(struct loonggpu_ring *ring,
				      unsigned vmid, uint64_t pd_addr);
void loonggpu_pipe_ring_emit_wreg(struct loonggpu_ring *ring, uint32_t reg,
				  uint32_t val);
int loonggpu_pipe_ring_test_ring(struct loonggpu_ring *ring);
int loonggpu_pipe_ring_test_ib(struct loonggpu_ring *ring, long timeout);
int loonggpu_pipe_ring_test_cs(struct loonggpu_ring *ring, long timeout);
#endif /*__PIPE_H__*/
