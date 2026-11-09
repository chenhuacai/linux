#ifndef __DC_PLANE_H__
#define __DC_PLANE_H__

enum dc_plane_type{
	DC_PLANE_PRIMARY,
	DC_PLANE_OVERLAY,
	DC_PLANE_CURSOR
};

struct dc_primary_plane {
	union plane_address address;
	int fb_width;
	int fb_height;
	uint64_t fb_address;
	u32 format;
	int crtc_width;
	int crtc_height;
	int crtc_x;
	int crtc_y;
	int stride;
	int unzip_mode;
};

struct dc_cursor_info {
	bool enable;
	int x;
	int y;
	union plane_address address;
	u32 format;
};

struct dc_plane_update {
	enum dc_plane_type type;
	union {
		struct dc_cursor_info cursor;
		struct dc_primary_plane primary;
	};
};

int loonggpu_dc_plane_init(struct loonggpu_device *adev,
			struct loonggpu_plane *aplane,
			unsigned long possible_crtcs);

int initialize_plane(struct loonggpu_device *adev,
		     struct loonggpu_mode_info *mode_info,
		     int plane_id);

bool dc_submit_plane_update(struct loonggpu_dc *dc, u32 link,
			    struct dc_plane_update *update);

bool dc_crtc_plane_update(struct loonggpu_dc_crtc *crtc,
			  struct dc_plane_update *update);

#endif /* __DC_PLANE_H__ */