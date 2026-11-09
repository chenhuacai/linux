#ifndef __LOONGGPU_BO_LIST_H__
#define __LOONGGPU_BO_LIST_H__

#include <drm/ttm/ttm_execbuf_util.h>
#include "loonggpu_drm.h"

struct loonggpu_device;
struct loonggpu_bo;
struct loonggpu_bo_va;
struct loonggpu_fpriv;

struct loonggpu_bo_list_entry {
	struct loonggpu_bo		*robj;
	struct ttm_validate_buffer	tv;
	struct loonggpu_bo_va		*bo_va;
	uint32_t			priority;
	struct page			**user_pages;
	int				user_invalidated;
};

struct loonggpu_bo_list {
	struct rcu_head rhead;
	struct kref refcount;
	unsigned first_userptr;
	unsigned num_entries;
};

int loonggpu_bo_list_get(struct loonggpu_fpriv *fpriv, int id,
		       struct loonggpu_bo_list **result);
void loonggpu_bo_list_get_list(struct loonggpu_bo_list *list,
			     struct list_head *validated);
void loonggpu_bo_list_put(struct loonggpu_bo_list *list);
int loonggpu_bo_create_list_entry_array(struct drm_loonggpu_bo_list_in *in,
				      struct drm_loonggpu_bo_list_entry **info_param);

int loonggpu_bo_list_create(struct loonggpu_device *adev,
				 struct drm_file *filp,
				 struct drm_loonggpu_bo_list_entry *info,
				 unsigned num_entries,
				 struct loonggpu_bo_list **list);

static inline struct loonggpu_bo_list_entry *
loonggpu_bo_list_array_entry(struct loonggpu_bo_list *list, unsigned index)
{
	struct loonggpu_bo_list_entry *array = (void *)&list[1];

	return &array[index];
}

#define loonggpu_bo_list_for_each_entry(e, list) \
	for (e = loonggpu_bo_list_array_entry(list, 0); \
	     e != loonggpu_bo_list_array_entry(list, (list)->num_entries); \
	     ++e)

#define loonggpu_bo_list_for_each_userptr_entry(e, list) \
	for (e = loonggpu_bo_list_array_entry(list, (list)->first_userptr); \
	     e != loonggpu_bo_list_array_entry(list, (list)->num_entries); \
	     ++e)

#endif /* __LOONGGPU_BO_LIST_H__ */
