#if !defined(LG_SYNC_HELPER_H)
#define LG_SYNC_HELPER_H

void *loonggpu_sync_get_owner(struct dma_fence *f);
bool loonggpu_sync_same_dev(struct loonggpu_device *adev, struct dma_fence *f);

static bool loonggpu_sync_test_fence(struct loonggpu_device *adev,
                                  enum loonggpu_sync_mode mode,
                                  void *owner, struct dma_fence *f)
{
        void *fence_owner = loonggpu_sync_get_owner(f);

        /* Always sync to moves, no matter what */
        if (fence_owner == LOONGGPU_FENCE_OWNER_UNDEFINED)
                return true;

        /* We only want to trigger KCD eviction fences on
         * evict or move jobs. Skip KCD fences otherwise.
         */
        if (fence_owner == LOONGGPU_FENCE_OWNER_KCD &&
            owner != LOONGGPU_FENCE_OWNER_UNDEFINED)
                return false;

        /* Never sync to VM updates either. */
        if (fence_owner == LOONGGPU_FENCE_OWNER_VM &&
            owner != LOONGGPU_FENCE_OWNER_UNDEFINED)
                return false;

        /* Ignore fences depending on the sync mode */
        switch (mode) {
        case LOONGGPU_SYNC_ALWAYS:
                return true;

        case LOONGGPU_SYNC_NE_OWNER:
                if (loonggpu_sync_same_dev(adev, f) &&
                    fence_owner == owner)
                        return false;
                break;

        case LOONGGPU_SYNC_EQ_OWNER:
                if (loonggpu_sync_same_dev(adev, f) &&
                    fence_owner != owner)
                        return false;
                break;

        case LOONGGPU_SYNC_EXPLICIT:
                return false;
        }

        return true;
}

static inline int lg_loonggpu_sync_resv(struct loonggpu_device *adev,
                     struct loonggpu_sync *sync,
                     lg_dma_resv_t *resv,
                     enum loonggpu_sync_mode mode,
                     void *owner, bool explicit_sync)
{
#if defined(dma_resv_for_each_fence)
        struct dma_resv_iter cursor;
        struct dma_fence *f;
        int r;

        if (resv == NULL)
                return -EINVAL;

        dma_resv_for_each_fence(&cursor, resv, DMA_RESV_USAGE_BOOKKEEP, f) {
                dma_fence_chain_for_each(f, f) {
                        struct dma_fence *tmp = dma_fence_chain_contained(f);
                        if (loonggpu_sync_test_fence(adev, mode, owner, tmp)) {
                                r = loonggpu_sync_fence(adev, sync, f, explicit_sync);
                                dma_fence_put(f);
                                if (r)
                                        return r;

                                break;
                        }
                }
        }
        return 0;
#else
        lg_dma_resv_list_t *flist;
        struct dma_fence *f;
        void *fence_owner;
        unsigned i;
        int r = 0;

        /* always sync to the exclusive fence */
        f = lg_dma_resv_get_excl(resv);
        r = loonggpu_sync_fence(adev, sync, f, false);

        flist = lg_dma_resv_get_list(resv);
        if (!flist || r)
                return r;

        for (i = 0; i < flist->shared_count; ++i) {
                f = rcu_dereference_protected(flist->shared[i],
                                              lg_dma_resv_held(resv));
                /* We only want to trigger KCD eviction fences on
                 * evict or move jobs. Skip KCD fences otherwise.
                 */
                fence_owner = loonggpu_sync_get_owner(f);
                if (fence_owner == LOONGGPU_FENCE_OWNER_KCD &&
                    owner != LOONGGPU_FENCE_OWNER_UNDEFINED)
                        continue;

                if (loonggpu_sync_same_dev(adev, f)) {
                        /* VM updates are only interesting
                         * for other VM updates and moves.
                         */
                        if ((owner != LOONGGPU_FENCE_OWNER_UNDEFINED) &&
                            (fence_owner != LOONGGPU_FENCE_OWNER_UNDEFINED) &&
                            ((owner == LOONGGPU_FENCE_OWNER_VM) !=
                             (fence_owner == LOONGGPU_FENCE_OWNER_VM)))
                                continue;

                        /* Ignore fence from the same owner and explicit one as
                         * long as it isn't undefined.
                         */
                        if (owner != LOONGGPU_FENCE_OWNER_UNDEFINED &&
                            (fence_owner == owner || explicit_sync))
                                continue;
                }

                r = loonggpu_sync_fence(adev, sync, f, false);
                if (r)
                        break;
        }
        return r;
#endif
}

#endif /* LG_SYNC_HELPER_H */
