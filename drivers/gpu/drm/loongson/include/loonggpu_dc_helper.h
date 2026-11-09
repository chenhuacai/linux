/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LOONGGPU_DC_HELPER_H__
#define __LOONGGPU_DC_HELPER_H__

#if defined(LG_DRM_ATOMIC_STATE)
#define lg_drm_atomic_state drm_atomic_state
#else
#define lg_drm_atomic_state drm_atomic_commit
#endif

#if defined(LG_STRNCPY)
#define lg_strncpy strncpy
#else
#define lg_strncpy strscpy_pad
#endif

#endif /* __LOONGGPU_DC_HELPER_H__ */
