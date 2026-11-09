#ifndef __LOONGGPU_HW_SEMA_H__
#define __LOONGGPU_HW_SEMA_H__

#define LOONGGPU_NUM_SEMA 32

struct loonggpu_vm;

struct loonggpu_hw_sema {
	struct list_head	list;
	struct loonggpu_vm		*vm;
    bool		own;
	unsigned	pasid;
	unsigned	ctx;
};

struct loonggpu_hw_sema_mgr {
	struct mutex		lock;
	unsigned		num_ids;
	struct list_head	sema_list;
	struct loonggpu_hw_sema	sema[LOONGGPU_NUM_SEMA];
};

void loonggpu_sema_free(struct loonggpu_device *adev, struct loonggpu_vm *vm);

int loonggpu_hw_sema_mgr_init(struct loonggpu_device *adev);
void loonggpu_hw_sema_mgr_fini(struct loonggpu_device *adev);

#endif /* __LOONGGPU_HW_SEMA_H__ */
