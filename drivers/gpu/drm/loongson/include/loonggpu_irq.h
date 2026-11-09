#ifndef __LOONGGPU_IRQ_H__
#define __LOONGGPU_IRQ_H__

#include <linux/irqdomain.h>
#include "loonggpu_ih.h"
#include <linux/irqreturn.h>

#define LOONGGPU_MAX_IRQ_SRC_ID	0x100
#define LOONGGPU_MAX_IRQ_CLIENT_ID	0x100

/* TODO irq srcid need rewrite*/
#define LOONGGPU_SRCID_GFX_PAGE_INV_FAULT                 0x00000092  /* 146 */
#define LOONGGPU_SRCID_GFX_MEM_PROT_FAULT                 0x00000093  /* 147 */
#define LOONGGPU_SRCID_CP_END_OF_PIPE                     0x000000b5  /* 181 */
#define LOONGGPU_SRCID_XDMA_TRAP          	               0x000000e0  /* 224 */

/* LG200 srcid */
#define LOONGGPU_LG200_SRCID_MMU_PAGE_FAULT 0x20
#define LOONGGPU_LG200_SRCID_MMU_PORT_FAULT 0x21
#define LOONGGPU_LG200_SRCID_CPIPE 0x22
#define LOONGGPU_LG200_SRCID_XDMA 0x23
#define LOONGGPU_LG200_SRCID_SQ_INTERRUPTMSG 0x24
#define LOONGGPU_LG200_SRCID_CP_END_OF_GPIPE 0x01
#define LOONGGPU_LG200_SRCID_CP_END_OF_BPIPE 0x02
#define LOONGGPU_LG210_SRCID_CP_END_OF_DPIPE 0x03
#define LOONGGPU_LG210_SRCID_CP_END_OF_EPIPE 0x04

struct loonggpu_device;
struct loonggpu_iv_entry;

enum loonggpu_interrupt_state {
	LOONGGPU_IRQ_STATE_DISABLE,
	LOONGGPU_IRQ_STATE_ENABLE,
};

struct loonggpu_irq_src {
	unsigned				num_types;
	atomic_t				*enabled_types;
	const struct loonggpu_irq_src_funcs	*funcs;
	void *data;
};

struct loonggpu_irq_client {
	struct loonggpu_irq_src **sources;
};

/* provided by interrupt generating IP blocks */
struct loonggpu_irq_src_funcs {
	int (*set)(struct loonggpu_device *adev, struct loonggpu_irq_src *source,
		   unsigned type, enum loonggpu_interrupt_state state);

	int (*process)(struct loonggpu_device *adev,
		       struct loonggpu_irq_src *source,
		       struct loonggpu_iv_entry *entry);
};

struct loonggpu_irq {
	bool				installed;
	unsigned int			irq;
	spinlock_t			lock;
	/* interrupt sources */
	struct loonggpu_irq_client	client[LOONGGPU_IH_CLIENTID_MAX];

	/* status, etc. */
	bool				msi_enabled; /* msi enabled */

	/* interrupt ring */
	struct loonggpu_ih_ring		ih;
	const struct loonggpu_ih_funcs	*ih_funcs;
};

void loonggpu_irq_disable_all(struct loonggpu_device *adev);
irqreturn_t loonggpu_irq_handler(int irq, void *arg);

int loonggpu_irq_init(struct loonggpu_device *adev);
void loonggpu_irq_fini(struct loonggpu_device *adev);
int loonggpu_irq_add_id(struct loonggpu_device *adev,
		      unsigned client_id, unsigned src_id,
		      struct loonggpu_irq_src *source);
void loonggpu_irq_dispatch(struct loonggpu_device *adev,
			 struct loonggpu_iv_entry *entry);
int loonggpu_irq_update(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
		      unsigned type);
int loonggpu_irq_get(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
		   unsigned type);
int loonggpu_irq_put(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
		   unsigned type);
bool loonggpu_irq_enabled(struct loonggpu_device *adev, struct loonggpu_irq_src *src,
			unsigned type);
void loonggpu_irq_gpu_reset_resume_helper(struct loonggpu_device *adev);

#endif /* __LOONGGPU_IRQ_H__ */
