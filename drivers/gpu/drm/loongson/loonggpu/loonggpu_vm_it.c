#include <linux/interval_tree_generic.h>
#include "loonggpu_vm_it.h"

#define START(node) ((node)->start)
#define LAST(node) ((node)->last)

INTERVAL_TREE_DEFINE(struct loonggpu_bo_va_mapping, rb, uint64_t, __subtree_last,
				START, LAST,, loonggpu_vm_it)

