#include "loonggpu.h"
#include "loonggpu_dc_crtc.h"
#include "loonggpu_dc_plane.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_dc_reg.h"

static int get_cursor_position(struct drm_plane *plane, struct drm_crtc *crtc,
			       struct dc_cursor_position *position)
{
	struct loonggpu_crtc *loonggpu_crtc = to_loonggpu_crtc(crtc);
	int x, y;
	int xorigin = 0, yorigin = 0;

	if (!crtc || !plane->state->fb) {
		position->enable = false;
		position->x = 0;
		position->y = 0;
		return 0;
	}

	if ((plane->state->crtc_w > loonggpu_crtc->max_cursor_width) ||
	    (plane->state->crtc_h > loonggpu_crtc->max_cursor_height)) {
		DRM_ERROR("%s: bad cursor width or height %d x %d\n",
			  __func__,
			  plane->state->crtc_w,
			  plane->state->crtc_h);
		return -EINVAL;
	}

	x = plane->state->crtc_x;
	y = plane->state->crtc_y;

	if (x <= -loonggpu_crtc->max_cursor_width ||
	    y <= -loonggpu_crtc->max_cursor_height)
		return 0;

	if (x < 0) {
		xorigin = min(-x, loonggpu_crtc->max_cursor_width - 1);
		x = 0;
	}

	if (y < 0) {
		yorigin = min(-y, loonggpu_crtc->max_cursor_height - 1);
		y = 0;
	}

	position->enable = true;
	position->x = x;
	position->y = y;
	position->x_hotspot = xorigin;
	position->y_hotspot = yorigin;

	return 0;
}

bool dc_cursor_move(struct loonggpu_dc_crtc *crtc,
				struct dc_cursor_move *move)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_crtc *acrtc = adev->mode_info.crtcs[crtc->resource->base.link];
	u32 cfg_val = 0;
	u32 pos_val = 0;
	u32 link = crtc->resource->base.link;

	if (IS_ERR_OR_NULL(move))
		return false;

	if (link >= DC_DVO_MAXLINK)
		return false;

	DRM_DEBUG_DRIVER("crusor link%d cur move x%d y%d hotx%d hoty%d enable%d\n",
		link, move->x, move->y, move->hot_x, move->hot_y, move->enable);

	mutex_lock(&acrtc->cursor_lock);
	cfg_val = dc_readl(adev, gdc_reg->cursor_reg[link].cfg);

	cfg_val &= ~(DC_CURSOR_POS_HOT_X_MASK << DC_CURSOR_POS_HOT_X_SHIFT);
	cfg_val &= ~(DC_CURSOR_POS_HOT_Y_MASK << DC_CURSOR_POS_HOT_Y_SHIFT);
	if (move->enable == false) {
		cfg_val &= ~DC_CURSOR_FORMAT_MASK;
		pos_val = 0x0;
	} else {
		cfg_val |= ((CUR_FORMAT_ARGB8888 & DC_CURSOR_FORMAT_MASK) << DC_CURSOR_FORMAT_SHIFT);
		cfg_val |= ((link & DC_CURSOR_DISPLAY_MASK) << DC_CURSOR_DISPLAY_SHIFT);
		cfg_val |= ((move->hot_x & DC_CURSOR_POS_HOT_X_MASK) << DC_CURSOR_POS_HOT_X_SHIFT);
		cfg_val |= ((move->hot_y & DC_CURSOR_POS_HOT_Y_MASK) << DC_CURSOR_POS_HOT_Y_SHIFT);
		pos_val = ((move->x & DC_CURSOR_POS_X_MASK) << DC_CURSOR_POS_X_SHIFT);
		pos_val |= ((move->y & DC_CURSOR_POS_Y_MASK) << DC_CURSOR_POS_Y_SHIFT);
	}

	cfg_val &= DC_CURSOR_MODE_CLEAN;
	cfg_val |= DC_CURSOR_MODE_64x64;
	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1) /* 2K3000CA */
		cfg_val |= 0x9000000;

	dc_writel(adev, gdc_reg->cursor_reg[link].cfg, cfg_val);
	dc_writel(adev, gdc_reg->cursor_reg[link].position, pos_val);
	mutex_unlock(&acrtc->cursor_lock);

	return true;
}

bool dc_cursor_set(struct loonggpu_dc_crtc *crtc, struct dc_cursor_info *cursor)
{
	struct loonggpu_device *adev = crtc->dc->adev;
	struct loonggpu_crtc *acrtc;
	int value = 0;
	u32 addr_l, addr_h;
	u32 link;

	if (IS_ERR_OR_NULL(crtc) || IS_ERR_OR_NULL(cursor))
		return false;

	link = crtc->resource->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	acrtc = adev->mode_info.crtcs[link];

	mutex_lock(&acrtc->cursor_lock);
	value = dc_readl(adev, gdc_reg->cursor_reg[link].cfg);

	value |= (link & DC_CURSOR_DISPLAY_MASK) << DC_CURSOR_DISPLAY_SHIFT;
	value |= (CUR_FORMAT_ARGB8888 & DC_CURSOR_FORMAT_MASK) << DC_CURSOR_FORMAT_SHIFT;

	value &= DC_CURSOR_MODE_CLEAN;
	value |= DC_CURSOR_MODE_64x64;
	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1) /* 2K3000CA */
		value |= 0x9000000;

	addr_l = cursor->address.low_part;
	addr_h = cursor->address.high_part;

	dc_writel(adev, gdc_reg->cursor_reg[link].cfg, value);
	dc_writel(adev, gdc_reg->cursor_reg[link].laddr, addr_l);
	dc_writel(adev, gdc_reg->cursor_reg[link].haddr, addr_h);
	mutex_unlock(&acrtc->cursor_lock);

	return true;
}

void handle_cursor_update(struct drm_plane *plane,
			  struct drm_plane_state *old_plane_state)
{
	struct loonggpu_device *adev = plane->dev->dev_private;
	struct loonggpu_dc *dc = adev->dc;
	struct drm_display_mode *native_mode = &dc->native_mode;
	struct loonggpu_framebuffer *afb;
	struct drm_crtc *crtc;
	struct loonggpu_crtc *loonggpu_crtc;
	struct loonggpu_dc_crtc *dc_crtc;
	uint64_t address;
	struct dc_cursor_position position;
	struct dc_plane_update dc_plane;
	struct dc_cursor_move move = {0};
	int ret;

	if (!plane->state->fb && !old_plane_state->fb)
		return;

	afb = to_loonggpu_framebuffer(plane->state->fb);
	crtc = afb ? plane->state->crtc : old_plane_state->crtc;
	if (!crtc)
		return;

	loonggpu_crtc = to_loonggpu_crtc(crtc);
	dc_crtc = dc->link_info[loonggpu_crtc->crtc_id].crtc;
	address = afb ? afb->address : 0;

	DRM_DEBUG_DRIVER("%s: crtc_id=%d with size %d to %d\n",
			 __func__,
			 loonggpu_crtc->crtc_id,
			 plane->state->crtc_w,
			 plane->state->crtc_h);

	ret = get_cursor_position(plane, crtc, &position);
	if (ret)
		return;

	if (!position.enable) {
		/* turn off cursor */
		dc->hw_ops->cursor_move(dc_crtc, &move);
		return;
	}

	dc_plane.type = DC_PLANE_CURSOR;
	dc_plane.cursor.x = plane->state->crtc_w;
	dc_plane.cursor.y = plane->state->crtc_h;
	dc_plane.cursor.address.low_part = lower_32_bits(address);
	dc_plane.cursor.address.high_part = upper_32_bits(address);
	dc_plane.cursor.enable = position.enable;
	dc_plane.cursor.format = afb->base.format->format;

	move.enable = position.enable;
	move.hot_x = position.x_hotspot;
	move.hot_y = position.y_hotspot;
	move.x = position.x;
	move.y = position.y;

	if (loonggpu_crtc->disp_scale
		&& dc->disp_bo[0].handle && dc->disp_bo[1].handle
		&& ((crtc->mode.hdisplay * crtc->mode.vdisplay)
		< (native_mode->hdisplay * native_mode->vdisplay))) {

		move.x = (u32)(((((int64_t)move.x * (int64_t)native_mode->hdisplay
				* 1000000LL) / (int64_t)crtc->mode.hdisplay) + 500000LL) / 1000000LL);
		move.y = (u32)(((((int64_t)move.y * (int64_t)native_mode->vdisplay
				* 1000000LL) / (int64_t)crtc->mode.vdisplay) + 500000LL) / 1000000LL);
	}

	if (!dc_submit_plane_update(adev->dc, loonggpu_crtc->crtc_id, &dc_plane))
		DRM_ERROR("DC failed to set cursor attributes\n");
	if (!dc->hw_ops->cursor_move(dc_crtc, &move))
		DRM_ERROR("DC failed to set cursor position\n");

	return;
}
