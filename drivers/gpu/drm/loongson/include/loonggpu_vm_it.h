
#ifndef __LOONGGPU_VM_IT_H__
#define __LOONGGPU_VM_IT_H__

#include <linux/rbtree.h>
#include <linux/types.h>
#include "loonggpu_object.h"

/* bo virtual addresses in a vm */
struct loonggpu_bo_va_mapping {
	struct rb_node rb;
	uint64_t start;
	uint64_t last;
	uint64_t __subtree_last;

	struct loonggpu_bo_va *bo_va;
	struct list_head list;
	uint64_t offset;
	uint64_t flags;
};

extern void loonggpu_vm_it_insert(struct loonggpu_bo_va_mapping *node,
					struct rb_root_cached *root);

extern void loonggpu_vm_it_remove(struct loonggpu_bo_va_mapping *node,
					struct rb_root_cached *root);

extern struct loonggpu_bo_va_mapping *
loonggpu_vm_it_iter_first(struct rb_root_cached *root,
					uint64_t start, uint64_t last);

extern struct loonggpu_bo_va_mapping *
loonggpu_vm_it_iter_next(struct loonggpu_bo_va_mapping *node,
					uint64_t start, uint64_t last);

extern struct loonggpu_bo_va_mapping *
loonggpu_vm_it_subtree_search(struct loonggpu_bo_va_mapping *node,
					uint64_t start, uint64_t last);

#endif  /* __LOONGGPU_VM_IT_H__ */
