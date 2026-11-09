#include <drm/drm_atomic_helper.h>
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_encoder.h"
#include "loonggpu_dc_resource.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "bridge_phy.h"
#include "loonggpu_backlight.h"
#include "loonggpu_helper.h"
#include "loonggpu_dc_hdmi.h"

#define DEBUG_FILTERED_MODES 0
extern bool hotplug_event_flag;

static bool
is_special_hdmi_connector(const struct loonggpu_connector *aconnector)
{
	int connector_type;
	size_t i;

	static const int special_connectors[] = {
		DRM_MODE_CONNECTOR_HDMIA,
		DRM_MODE_CONNECTOR_HDMIB,
		DRM_MODE_CONNECTOR_Composite,
	};

	if (!aconnector)
		return false;

	connector_type = aconnector->base.connector_type;
	for (i = 0; i < ARRAY_SIZE(special_connectors); i++) {
		if (connector_type == special_connectors[i])
			return true;
	}

	return false;
}

bool is_audio_supported_connector(int connector_type)
{
	size_t i;

	static const int audio_connectors[] = {
		DRM_MODE_CONNECTOR_HDMIA,
		DRM_MODE_CONNECTOR_HDMIB,
		DRM_MODE_CONNECTOR_eDP,
		DRM_MODE_CONNECTOR_Composite,
	};

	for (i = 0; i < ARRAY_SIZE(audio_connectors); i++) {
		if (connector_type == audio_connectors[i])
			return true;
	}

	return false;
}

/*
 * EDID manufacturer ID is three 5-bit letters; @mfg must have room for the
 * three characters and the terminator.
 */
void get_edid_manufacturer(const struct edid *edid, char mfg[4])
{
	if (!edid) {
		DRM_INFO("edid is null\n");
		mfg[0] = '\0';
		return;
	}

	mfg[0] = ((edid->mfg_id[0] >> 2) & 0x1f) + 'A' - 1;
	mfg[1] = (((edid->mfg_id[0] & 0x03) << 3) |
			((edid->mfg_id[1] >> 5) & 0x07)) + 'A' - 1;
	mfg[2] = (edid->mfg_id[1] & 0x1f) + 'A' - 1;
	mfg[3] = '\0';
}

static bool is_archermind_1080p_100hz(const struct dc_timing_info *timing)
{
	return timing &&
	       timing->hdisplay == 1920 &&
	       timing->vdisplay == 1080 &&
	       timing->vrefresh == 100;
}

static void hdmi_ctrl_rmw(struct loonggpu_device *adev, u32 link, bool clear)
{
	u32 reg, val;

	if (adev->chip == dev_2k3000) {
		reg = gdc_reg->hdmi_reg_v2[0].ctrl;
	} else if (adev->chip == dev_7a2000 ||
		   adev->chip == dev_2k2000) {
		reg = gdc_reg->hdmi_reg_v1[link].ctrl;
	} else {
		return;
	}

	val = dc_readl(adev, reg);
	if (clear) {
		val |= HDMI_PACKET_ENABLE_CTRL;
		val &= ~HDMI_VIDEO_LEADING_GB_DISABLE;
	} else {
		val &= ~HDMI_PACKET_ENABLE_CTRL;
		val |= HDMI_VIDEO_LEADING_GB_DISABLE;
	}

	dc_writel(adev, reg, val);
}

static void fix_vsc_missing_display(struct loonggpu_device *adev, u32 link, struct dc_timing_info *timing)
{
	/* The VSC workaround applies to all modes */
	hdmi_ctrl_rmw(adev, link, false);
}

static void fix_archermind_missing_display(struct loonggpu_device *adev, u32 link, struct dc_timing_info *timing)
{
	if (!timing || !timing->vrefresh)
		return;

	/* The ArcherMind AD241 workaround only applies to 1920x1080p@100Hz */
	if (is_archermind_1080p_100hz(timing))
		hdmi_ctrl_rmw(adev, link, false);
	else
		hdmi_ctrl_rmw(adev, link, true);
}

enum special_display_id {
	SPECIAL_DISPLAY_VSC,
	SPECIAL_DISPLAY_ARCHERMIND_100HZ,
	SPECIAL_DISPLAY_MAX,
};

static const struct special_display special_displays[SPECIAL_DISPLAY_MAX] = {
	[SPECIAL_DISPLAY_VSC] = {
		.manufacturer = "VSC",
		.product_code = 14882,
		.serial = 16843009,
		.sd_func = fix_vsc_missing_display,
	},
	[SPECIAL_DISPLAY_ARCHERMIND_100HZ] = {
		.manufacturer = "XXX",
		.product_code = 0,
		.serial = 0,
		.sd_func = fix_archermind_missing_display,
	},
};

static const struct special_display *
find_special_display(struct loonggpu_connector *aconnector)
{
	const struct special_display *archermind_sd;
	char manufacturer[4];
	struct edid *edid;
	u16 product_code;
	int i;

	if (!aconnector || !aconnector->base.edid_blob_ptr ||
	    !aconnector->base.edid_blob_ptr->data)
		return NULL;

	edid = (struct edid *)aconnector->base.edid_blob_ptr->data;
	get_edid_manufacturer(edid, manufacturer);

	/*
	 * The ArcherMind monitor reports its product code as 0, so it can
	 * only be matched by manufacturer.
	 */
	product_code = EDID_PRODUCT_ID(edid);
	for (i = 0; i < ARRAY_SIZE(special_displays); i++) {
		const struct special_display *sd = &special_displays[i];

		if (strncmp(manufacturer, sd->manufacturer, 3) != 0)
			continue;
		if (product_code != sd->product_code)
			continue;
		if (edid->serial != sd->serial)
			continue;

		return sd;
	}

	archermind_sd = &special_displays[SPECIAL_DISPLAY_ARCHERMIND_100HZ];
	if (!strncmp(manufacturer, archermind_sd->manufacturer, 3))
		return archermind_sd;

	return NULL;
}

void is_special_display(struct loonggpu_device *adev, struct loonggpu_connector *aconnector,
			u32 link, struct dc_timing_info *timing)
{
	const struct special_display *sd;

	if (!is_special_hdmi_connector(aconnector))
		return;

	sd = find_special_display(aconnector);
	if (sd && sd->sd_func && timing)
		sd->sd_func(adev, link, timing);
}

bool is_special_hdmi_display(struct loonggpu_connector *aconnector,
			     struct dc_timing_info *timing)
{
	const struct special_display *sd = NULL;
	if (!timing || !timing->vrefresh)
		return false;

	sd  = find_special_display(aconnector);
	if (!sd)
		return false;

	switch (sd - special_displays) {
	case SPECIAL_DISPLAY_VSC:
		/* VSC workaround applies to all modes */
		return true;
	case SPECIAL_DISPLAY_ARCHERMIND_100HZ:
		/* ArcherMind AD241 workaround only applies to 1080p@100Hz */
		return is_archermind_1080p_100hz(timing);
	default:
		return false;
	}
}

static void destroy_virtual_connector(struct drm_connector *connector)
{
	struct loonggpu_connector *lconnector = to_loonggpu_connector(connector);
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(lconnector);
}

static enum drm_connector_status
loonggpu_virtual_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_disconnected;
}

static int loonggpu_virtual_connector_get_modes(struct drm_connector *connector)
{
	return 0;
}

static struct drm_connector_helper_funcs virtual_connector_helper_funcs = {
	.get_modes = loonggpu_virtual_connector_get_modes,
};

static const struct drm_connector_funcs virtual_connector_funcs = {
	.destroy = destroy_virtual_connector,
	.detect = loonggpu_virtual_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/**
 * create_virtual_connector - Create and initialize a virtual DRM connector
 * @dev: DRM device object
 *
 * This function is responsible for allocating and initializing a virtual connector.
 * It encapsulates the complete lifecycle of memory allocation and initialization,
 * ensuring the connector is either fully initialized successfully or completely
 * cleaned up on failure.
 *
 * Return: Pointer to drm_connector on success, ERR_PTR() error code on error.
 */
struct drm_connector *create_virtual_connector(struct drm_device *dev)
{
	struct loonggpu_device *adev = dev->dev_private;
	struct loonggpu_connector *lconnector;
	int ret;

	if (adev->mode_info.dp_connector)
		return &adev->mode_info.dp_connector->base;

	/* Allocate connector memory */
	lconnector = kzalloc(sizeof(*lconnector), GFP_KERNEL);
	if (!lconnector)
		return ERR_PTR(-ENOMEM);

	mutex_init(&lconnector->hpd_lock);

	/* Initialize DRM connector */
	ret = drm_connector_init(dev, &lconnector->base, &virtual_connector_funcs,
                           DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		DRM_ERROR("Failed to initialize virtual connector: %d\n", ret);
		kfree(lconnector);
		return ERR_PTR(ret);
	}

	adev->mode_info.dp_connector = lconnector;

	drm_connector_helper_add(&lconnector->base, &virtual_connector_helper_funcs);

	return &lconnector->base;
}

/**
 * filter_unique_progressive_modes - Filter the probed_modes list to keep only unique progressive modes.
 * @connector: Pointer to the DRM connector
 *
 * This function directly modifies the connector->probed_modes linked list.
 * It removes all interlaced modes and duplicate progressive modes (based on
 * hdisplay, vdisplay, vrefresh, and clock).
 * After execution, the probed_modes list will contain only unique progressive modes.
 * Note: This function frees the memory of removed drm_display_mode objects.
 */
void filter_unique_progressive_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *tmp;
	struct drm_display_mode *inner;
	bool unique;

	if (!connector || list_empty(&connector->probed_modes))
		return;

	/* Use the _safe variant for traversal since we may remove nodes during iteration */
	list_for_each_entry_safe(mode, tmp, &connector->probed_modes, head) {

		/* First, unconditionally remove all interlaced modes */
		if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
			DRM_DEBUG("Removing interlaced mode: %dx%d@%d\n",
				mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode));
			list_del(&mode->head);
			drm_mode_destroy(connector->dev, mode); /* Free the mode memory */
			continue;
		}

		unique = true;

		/* Check if the current progressive mode duplicates any mode that appears before it in the list */
		list_for_each_entry(inner, &connector->probed_modes, head) {
		/* Stop checking when reaching the current mode (only compare with preceding modes) */
		if (inner == mode)
			break;

		/* Skip interlaced modes in 'inner' (though they should have been removed in the outer loop) */
		if (inner->flags & DRM_MODE_FLAG_INTERLACE)
			continue;

		/* Compare key parameters: resolution, refresh rate, and clock */
		if (inner->hdisplay == mode->hdisplay && \
			inner->vdisplay == mode->vdisplay &&  \
			drm_mode_vrefresh(inner) == drm_mode_vrefresh(mode) && \
			inner->clock == mode->clock) {

			unique = false;
			DRM_DEBUG("Found duplicate progressive mode: %dx%d@%d, clock=%d\n",
			mode->hdisplay, mode->vdisplay,
			drm_mode_vrefresh(mode), mode->clock);
			break;
			}
		}

		/* Remove the mode from the list and destroy it if it is not unique */
		if (!unique) {
			list_del(&mode->head);
			/* Free the mode memory */
			drm_mode_destroy(connector->dev, mode);
		} else {
			DRM_DEBUG("Keeping unique progressive mode: %dx%d@%d, clock=%d\n",
				mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode), mode->clock);
		}
	}
}

/**
 * print_filtered_modes - Traverse and print the contents of the filtered
 *                       connector->probed_modes linked list
 * @connector: Pointer to the DRM connector
 *
 * This function is used after calling filter_unique_progressive_modes to
 * traverse and print all display mode information (hdisplay, vdisplay,
 * vrefresh, clock, flags) in the linked list.
 * It also prints a summary showing the total number of modes currently
 * in the linked list.
 * This function is primarily used for debugging and logging purposes.
 */
void show_filtered_modes(struct drm_connector *connector)
{
#if DEBUG_FILTERED_MODES
	struct drm_display_mode *mode;
	int count = 0;

	if (!connector || list_empty(&connector->probed_modes)) {
		DRM_INFO("Filtered modes list is empty.\n");
		return;
	}


	DRM_INFO("Listing all modes in connector->probed_modes connetor_type-%d after filtering:\n", connector->connector_type);
	DRM_INFO("Format: hdisplay x vdisplay @ vrefresh Hz, clock kHz [flags]\n");
	DRM_INFO("------------------------------------------------------------\n");

	/* Safely traverse the linked list using list_for_each_entry */
	list_for_each_entry(mode, &connector->probed_modes, head) {
		count++;
		DRM_INFO("[%d] %dx%d@%d Hz, clock: %d kHz, flags: 0x%x\n",
					count,
					mode->hdisplay,
					mode->vdisplay,
					drm_mode_vrefresh(mode),
					mode->clock,
					mode->flags);
	}

	DRM_INFO("------------------------------------------------------------\n");
	DRM_INFO("Total unique progressive modes in list: %d\n", count);
#endif
}

/**
 * drm_connector_edid_intersection - Perform intersection or copy of probed_modes
 * between actual_connector and virtual_connector, returning the valid mode count
 *
 * @actual_connector: The connector where intersection results or copies will be
 *                    stored in its probed_modes list
 * @virtual_connector: The connector providing the mode list for comparison or as copy source
 *
 * This function first checks if the actual_connector's probed_modes is empty and
 * virtual_connector's probed_modes is not empty. If true, it copies the virtual
 * connector's probed_modes list to the actual connector's probed_modes and returns
 * the count of copied modes.
 *
 * Otherwise, it compares the probed_modes of both connectors, finding modes that
 * have identical resolution (hdisplay, vdisplay), vertical refresh rate (vrefresh,
 * calculated by drm_mode_vrefresh), and pixel clock (clock).
 * Only these common modes will be retained in actual_connector->probed_modes,
 * while non-matching modes will be removed.
 * The function returns the number of valid display modes remaining in the
 * actual connector after processing.
 *
 * Return: Number of valid display modes (positive value) on success,
 *         or a negative error code on failure.
 */
int drm_connector_edid_intersection(struct drm_connector *actual_connector,
                                  struct drm_connector *virtual_connector)
{
	struct drm_display_mode *virtual_connector_mode, *new_mode;
	struct drm_display_mode *actual_connector_mode, *temp;
	uint32_t actual_connector_vrefresh, virtual_connector_vrefresh;
	 /* Counter for valid modes */
	int valid_mode_count = 0;
	bool found_match;

	/* Parameter validation */
	if (!actual_connector || !virtual_connector) {
		DRM_ERROR("Invalid connector pointer(s)\n");
		return -EINVAL;
	}

	/*
	 * Check if actual_connector's probed_modes is empty and virtual_connector's
	 * probed_modes is not empty. If condition met, copy virtual_connector's
	 * probed_modes to actual_connector's probed_modes and return the copy count.
	 */
	if (list_empty(&actual_connector->probed_modes) &&
		!list_empty(&virtual_connector->probed_modes)) {
		DRM_INFO("[EDID Intersection] actual connector probed_modes is empty, copying all modes from virtual connector.\n");

		/* Traverse virtual_connector's probed_modes list and perform copying */
		list_for_each_entry(virtual_connector_mode, &virtual_connector->probed_modes, head) {
			new_mode = drm_mode_duplicate(actual_connector->dev, virtual_connector_mode);
			if (!new_mode) {
				DRM_ERROR("Failed to duplicate mode from DP connector\n");
				/* Skip failed copy and continue with next mode */
				continue;
			}
			list_add_tail(&new_mode->head, &actual_connector->probed_modes);
			/* Increment count for each successfully copied mode */
			valid_mode_count++;
		}

		DRM_INFO("[EDID Intersection] Copied %d modes from virtual connector to actual connector.\n", valid_mode_count);
		 /* Return count of successfully copied modes */
		return valid_mode_count;
	}

	/* Reset counter for counting remaining modes after intersection */
	valid_mode_count = 0;

	/*
	 * Intersection operation: Traverse actual_connector's probed_modes list and
	 * compare with virtual_connector's list.
	 * Initialize valid mode counter starting with total modes in actual_connector
	 * (will be decreased during traversal).
	 * Note: Using list_for_each_entry_safe allows node deletion during traversal.
	 */
	list_for_each_entry_safe(actual_connector_mode, temp,
				&actual_connector->probed_modes, head) {
		found_match = false;
		actual_connector_vrefresh = drm_mode_vrefresh(actual_connector_mode);

		/* Traverse virtual_connector's probed_modes list to find matches */
		list_for_each_entry(virtual_connector_mode, &virtual_connector->probed_modes, head) {
			virtual_connector_vrefresh = drm_mode_vrefresh(virtual_connector_mode);

			if (actual_connector_mode->hdisplay == virtual_connector_mode->hdisplay &&
				actual_connector_mode->vdisplay == virtual_connector_mode->vdisplay &&
				actual_connector_vrefresh == virtual_connector_vrefresh &&
				actual_connector_mode->clock == virtual_connector_mode->clock) {
				found_match = true;
				break;
			}
		}

		if (found_match) {
			/* Mode matched, keep it and increment count */
			valid_mode_count++;
		} else {
			/* Remove non-matching mode from actual_connector's probed_list and destroy it */
			list_del(&actual_connector_mode->head);
			drm_mode_destroy(actual_connector->dev, actual_connector_mode);
			/* No match, don't count as valid */
		}
	}

	DRM_INFO("[EDID Intersection] actual and virtual common modes filtered. %d modes remaining.\n", valid_mode_count);

	 /* Return count of valid modes remaining after intersection */
	return valid_mode_count;
}

static struct drm_display_mode *
loonggpu_dc_create_common_mode(struct drm_connector *connector,
			     char *name,
			     int hdisplay, int vdisplay)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct drm_device *dev = adev->ddev;
	struct drm_display_mode *mode = NULL;
	struct drm_display_mode *native_mode = &adev->dc->native_mode;

	mode = drm_mode_duplicate(dev, native_mode);

	if (mode == NULL)
		return NULL;

	mode->hdisplay = hdisplay;
	mode->vdisplay = vdisplay;
	mode->type &= ~DRM_MODE_TYPE_PREFERRED;
	lg_strncpy(mode->name, name, DRM_DISPLAY_MODE_LEN);

	return mode;

}

static int loonggpu_dc_connector_add_common_modes(struct drm_connector *connector)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct drm_display_mode *mode = NULL;
	struct drm_display_mode *native_mode = &adev->dc->native_mode;
	int num_modes = 0;

	int i;
	int n;
	struct mode_size {
		char name[DRM_DISPLAY_MODE_LEN];
		int w;
		int h;
	} common_modes[] = {
		{  "640x480",  640,  480},
		{  "800x600",  800,  600},
		{ "1024x768", 1024,  768},
		{ "1280x720", 1280,  720},
		{ "1280x800", 1280,  800},
		{"1280x1024", 1280, 1024},
		{ "1440x900", 1440,  900},
		{"1680x1050", 1680, 1050},
		{"1600x1200", 1600, 1200},
		{"1920x1080", 1920, 1080},
		{"1920x1200", 1920, 1200}
	};

	n = ARRAY_SIZE(common_modes);

	for (i = 0; i < n; i++) {
		struct drm_display_mode *curmode = NULL;
		bool mode_existed = false;

		if (common_modes[i].w > native_mode->hdisplay ||
		    common_modes[i].h > native_mode->vdisplay ||
		   (common_modes[i].w == native_mode->hdisplay &&
		    common_modes[i].h == native_mode->vdisplay))
			continue;

		list_for_each_entry(curmode, &connector->probed_modes, head) {
			if (common_modes[i].w == curmode->hdisplay &&
			    common_modes[i].h == curmode->vdisplay) {
				mode_existed = true;
				break;
			}
		}

		if (mode_existed)
			continue;

		mode = loonggpu_dc_create_common_mode(connector,
				common_modes[i].name, common_modes[i].w,
				common_modes[i].h);
		drm_mode_probed_add(connector, mode);
		num_modes++;
	}

	return num_modes;
}

int loonggpu_dc_get_modes(struct loonggpu_bridge_phy *phy, int used_method,
					struct drm_connector *connector, struct edid *edid)
{
	struct loonggpu_bridge_phy *internal_bp = phy->res->internal_bp;
	unsigned int count = 0;

	switch (used_method) {
	case via_i2c:
		if (phy->li2c)
			edid = drm_get_edid(connector, &phy->li2c->adapter);

		if (edid) {
			drm_connector_update_edid_property(connector, edid);
			count = drm_add_edid_modes(connector, edid);
			kfree(edid);
		}
		break;
	case via_encoder:
		if (phy->ddc_funcs && phy->ddc_funcs->get_modes)
			count = phy->ddc_funcs->get_modes(phy, connector);
		else if (internal_bp && internal_bp->ddc_funcs && internal_bp->ddc_funcs->get_modes)
			count = internal_bp->ddc_funcs->get_modes(internal_bp, connector);
		break;
	}

	return count;
}

static int bridge_phy_connector_get_modes(struct drm_connector *connector)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct loonggpu_bridge_phy *phy =
		adev->mode_info.encoders[connector->index]->bridge;
	struct loonggpu_bridge_phy *internal_bp = phy->res->internal_bp;
	unsigned short used_method = phy->res->edid_method;
	struct connector_resource *connector_resource;
	struct loonggpu_dc *dc = adev->dc;
	int size = sizeof(u8) * EDID_LENGTH * 2;
	struct edid *edid = NULL;
	unsigned int count = 0;
	struct loonggpu_crtc *lcrtc =
		adev->mode_info.crtcs[connector->index];
	struct loonggpu_connector *lconnector;

	lconnector = to_loonggpu_connector(connector);

	if (lconnector &&
	    lconnector->base.edid_blob_ptr &&
	    lconnector->base.edid_blob_ptr->data) {
		edid = (struct edid *)lconnector->base.edid_blob_ptr->data;
		return drm_add_edid_modes(connector, edid);
	}

	if (used_method == via_vbios) {
		connector_resource = dc_get_vbios_resource(internal_bp->adev->dc->vbios, connector->index, LOONGGPU_RESOURCE_CONNECTOR);

		edid = kmalloc(size, GFP_KERNEL);
		if (edid && connector_resource) {
			memcpy(edid, connector_resource->internal_edid, size);
			drm_connector_update_edid_property(connector, edid);
			count = drm_add_edid_modes(connector, edid);
			kfree(edid);
		}
	} else {
		if (dc->hw_ops->dc_get_modes)
			count = dc->hw_ops->dc_get_modes(phy, used_method, connector, edid);
	}

	if (lcrtc->disp_scale) {
		/* add scaled modes */
		count += loonggpu_dc_connector_add_common_modes(connector);
		return count;
	}

	if (!count) {
		count = drm_add_modes_noedid(connector, 1920, 1080);
		drm_set_preferred_mode(connector, 1024, 768);
		DRM_DEBUG_DRIVER("[Bridge_phy] Setting %s edid.\n",
				 phy->res->chip_name);
	}

	return count;
}

static bool is_resolution_valid(struct panel_resource *panel_resource,
			const struct drm_display_mode *mode)
{
	u32 vrefresh = 0;
	u32 index;

	if (!panel_resource)
		return false;

	vrefresh = drm_mode_vrefresh(mode);

	if (!panel_resource->feature) {
		if (panel_resource->max_hdisplay && panel_resource->max_vrefresh) {
			if (mode->hdisplay > panel_resource->max_hdisplay &&    \
				mode->vdisplay > panel_resource->max_vdisplay)
				return true;
			else if (mode->hdisplay == panel_resource->max_hdisplay &&      \
				mode->vdisplay == panel_resource->max_vdisplay) {
				if (vrefresh > panel_resource->max_vrefresh)
					return true;
			}
		}
	} else {
		if (mode->clock > panel_resource->max_pixclk &&  \
			panel_resource->max_pixclk)
			return true;
	}

	for (index = 0; index < panel_resource->count; index++) {
		if (panel_resource->timing[index].vrefresh == vrefresh &&   \
			panel_resource->timing[index].hdisplay == mode->hdisplay &&   \
			panel_resource->timing[index].vdisplay == mode->vdisplay)
			return true;
	}

	return false;
}

static enum drm_mode_status
bridge_phy_connector_mode_valid(struct drm_connector *connector,
				const struct drm_display_mode *mode)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct loonggpu_bridge_phy *phy =
		adev->mode_info.encoders[connector->index]->bridge;
	struct loonggpu_bridge_phy *internal_bp = phy->res->internal_bp;
	struct panel_resource *panel_resource;
	struct loonggpu_dc *dc = adev->dc;

	panel_resource = dc_get_vbios_resource(dc->vbios,
				connector->index, LOONGGPU_RESOURCE_PANEL);

	if (is_resolution_valid(panel_resource, mode))
		return MODE_BAD;

	if (phy->cfg_funcs && phy->cfg_funcs->mode_valid)
		return phy->cfg_funcs->mode_valid(connector, (struct drm_display_mode *)mode);
	else
		return internal_bp->cfg_funcs->mode_valid(connector, (struct drm_display_mode *)mode);
	return MODE_OK;
}

static struct drm_encoder *
bridge_phy_connector_best_encoder(struct drm_connector *connector)
{
	struct drm_encoder *encoder;
	int __maybe_unused i;

	/* pick the first one */
	lg_drm_connector_for_each_possible_encoder(connector, encoder, i)
		return encoder;

	return NULL;
}

static struct drm_connector_helper_funcs bridge_phy_connector_helper_funcs = {
	.get_modes = bridge_phy_connector_get_modes,
	.mode_valid = bridge_phy_connector_mode_valid,
	.best_encoder = bridge_phy_connector_best_encoder,
};

bool is_connected(struct drm_connector *connector)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct loonggpu_dc_i2c *i2c = adev->i2c[connector->index];
	unsigned char start = 0x0;
	struct i2c_adapter *adapter;
	struct i2c_msg msgs = {
		.addr = DDC_ADDR,
		.flags = I2C_M_RD,
		.len = 1,
		.buf = &start,
	};

	if (!i2c)
		return false;

	adapter = &i2c->adapter;
	if (i2c_transfer(adapter, &msgs, 1) != 1) {
		DRM_DEBUG_KMS("display-%d not connect\n", connector->index);
		return false;
	}

	return true;
}

static enum drm_connector_status
bridge_phy_connector_detect(struct drm_connector *connector, bool force)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	enum drm_connector_status status = connector_status_connected;
	struct loonggpu_bridge_phy *phy = adev->mode_info.encoders[connector->index]->bridge;
	struct loonggpu_bridge_phy *internal_bp = phy->res->internal_bp;

	if (hotplug_event_flag)
		return connector_status_disconnected;

	if (internal_bp->hpd_funcs && internal_bp->hpd_funcs->get_connect_status)
		status = internal_bp->hpd_funcs->get_connect_status(phy);

	return status;
}

static void loonggpu_dc_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(connector);
}

static int loonggpu_drm_helper_probe_single_connector_modes(
				struct drm_connector *connector,
				uint32_t maxX, uint32_t maxY)
{
	int count;
	struct drm_display_mode *mode;

	count = drm_helper_probe_single_connector_modes(connector, maxX, maxY);

	/*
	 * if there is no preferred mode in 'connector->modes',
	 * chose the first one as preferred mode.
	 */
	if (count > 0) {
		mode = list_first_entry(&connector->modes, struct drm_display_mode, head);
		if (mode && !(mode->type & DRM_MODE_TYPE_PREFERRED))
			mode->type |= DRM_MODE_TYPE_PREFERRED;
	}

	return count;
}

static const struct drm_connector_funcs bridge_phy_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = bridge_phy_connector_detect,
	.fill_modes = loonggpu_drm_helper_probe_single_connector_modes,
	.destroy = loonggpu_dc_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/**
 * @section Bridge-phy core functions
 */
static void bridge_phy_atomic_enable(struct drm_bridge *bridge,
				     struct drm_atomic_commit *state)
{
	struct loonggpu_bridge_phy *phy = to_bridge_phy(bridge);

	DRM_DEBUG("[Bridge_phy] [%s] enable\n", phy->res->chip_name);
	if (phy->cfg_funcs && phy->cfg_funcs->afe_high)
		phy->cfg_funcs->afe_high(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->afe_set_tx)
		phy->cfg_funcs->afe_set_tx(phy, TRUE);
	if (phy->cfg_funcs && phy->cfg_funcs->hdmi_audio)
		phy->cfg_funcs->hdmi_audio(phy);
}

static void bridge_phy_atomic_disable(struct drm_bridge *bridge,
				      struct drm_atomic_commit *state)
{
	struct loonggpu_bridge_phy *phy = to_bridge_phy(bridge);

	DRM_DEBUG("[Bridge_phy] [%s] disable\n", phy->res->chip_name);
	if (phy->cfg_funcs && phy->cfg_funcs->afe_low)
		phy->cfg_funcs->afe_low(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->afe_set_tx)
		phy->cfg_funcs->afe_set_tx(phy, FALSE);
}

static int __bridge_phy_mode_set(struct loonggpu_bridge_phy *phy,
				 const struct drm_display_mode *mode,
				 const struct drm_display_mode *adj_mode)
{
	if (phy->mode_config.input_mode.gen_sync)
		DRM_DEBUG("[Bridge_phy] [%s] bridge_phy gen_sync\n",
				phy->res->chip_name);
	if (phy->cfg_funcs && phy->cfg_funcs->mode_set_pre)
		phy->cfg_funcs->mode_set_pre(&phy->bridge, mode, adj_mode);
	if (phy->cfg_funcs && phy->cfg_funcs->mode_set)
		phy->cfg_funcs->mode_set(phy, mode, adj_mode);
	if (phy->cfg_funcs && phy->cfg_funcs->mode_set_post)
		phy->cfg_funcs->mode_set_post(&phy->bridge, mode, adj_mode);

	return 0;
}

void bridge_phy_mode_set(struct loonggpu_bridge_phy *phy,
				struct drm_display_mode *mode,
				struct drm_display_mode *adj_mode)
{
	if (!phy)
		return;

	DRM_DEBUG("[Bridge_phy] [%s] mode set\n", phy->res->chip_name);
	drm_mode_debug_printmodeline(mode);

	__bridge_phy_mode_set(phy, mode, adj_mode);
}

static int bridge_phy_attach (lg_bridge_phy_attach_args)
{
	struct loonggpu_bridge_phy *phy = to_bridge_phy(bridge);
	struct loonggpu_connector *lconnector;
	int link_index = phy->display_pipe_index;
	struct loonggpu_dc_bridge *dc_bridge = phy->res;
	struct loonggpu_bridge_phy *internal_bp = dc_bridge->internal_bp;
	int ret;

	DRM_DEBUG("[Bridge_phy] %s attach\n", phy->res->chip_name);
	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found\n");
		return -ENODEV;
	}

	lconnector = kzalloc(sizeof(*lconnector), GFP_KERNEL);
	if (!lconnector)
		return -ENOMEM;

	mutex_init(&lconnector->hpd_lock);

	ret = drm_connector_init(bridge->dev, &lconnector->base,
				 &bridge_phy_connector_funcs,
				 phy->connector_type);
	if (ret) {
		DRM_ERROR("[Bridge_phy] %s Failed to initialize connector\n",
				phy->res->chip_name);
		return ret;
	}

	lconnector->connector_id = link_index;
	phy->connector = &lconnector->base;
	phy->adev->mode_info.connectors[link_index] = lconnector;
	phy->connector->dpms = DRM_MODE_DPMS_OFF;

	drm_connector_helper_add(&lconnector->base,
				 &bridge_phy_connector_helper_funcs);

	if (loonggpu_backlight_register(&lconnector->base))
		DRM_DEBUG_DRIVER("[Bridge_phy] %s backlight register failed\n",
				 phy->res->chip_name);

	if (phy->hpd_funcs && phy->hpd_funcs->hpd_init)
		phy->hpd_funcs->hpd_init(phy, lconnector, phy->is_ext_encoder);
	else if (internal_bp->hpd_funcs && internal_bp->hpd_funcs->hpd_init)
		internal_bp->hpd_funcs->hpd_init(phy, lconnector, phy->is_ext_encoder);

	DRM_DEBUG_DRIVER("[Bridge_phy] %s Set connector poll=0x%x.\n",
			 phy->res->chip_name, phy->connector->polled);

	return ret;
}

static const struct drm_bridge_funcs bridge_funcs = {
	.attach			= bridge_phy_attach,
	.atomic_enable		= bridge_phy_atomic_enable,
	.atomic_disable		= bridge_phy_atomic_disable,
	.atomic_create_state	= drm_atomic_helper_bridge_create_state,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
};

static int bridge_phy_bind(struct loonggpu_bridge_phy *phy)
{
	int ret;

	phy->bridge.funcs = &bridge_funcs;
	drm_bridge_add(&phy->bridge);
	ret = lg_drm_bridge_attach(phy->encoder, &phy->bridge, NULL, 0);
	if (ret) {
		DRM_ERROR("[Bridge_phy] %s Failed to attach phy ret %d\n",
			  phy->res->chip_name, ret);
		return ret;
	}

	DRM_INFO("[Bridge_phy] %s encoder-%d be attach to this bridge.\n",
		 phy->res->chip_name, phy->encoder->index);

	return 0;
}

/**
 * @section Bridge-phy helper functions
 */
void bridge_phy_reg_mask_seq(struct loonggpu_bridge_phy *phy,
			     const struct reg_mask_seq *seq, size_t seq_size)
{
	unsigned int i;
	struct regmap *regmap;

	regmap = phy->phy_regmap;
	for (i = 0; i < seq_size; i++)
		regmap_update_bits(regmap, seq[i].reg, seq[i].mask, seq[i].val);
}

void bridge_phy_reg_update_bits(struct loonggpu_bridge_phy *phy, unsigned int reg,
				unsigned int mask, unsigned int val)
{
	unsigned int reg_val;

	regmap_read(phy->phy_regmap, reg, &reg_val);
	val ? (reg_val |= (mask)) : (reg_val &= ~(mask));
	regmap_write(phy->phy_regmap, reg, reg_val);
}

int bridge_phy_reg_dump(struct loonggpu_bridge_phy *phy, size_t start,
			size_t count)
{
	u8 *buf;
	int ret;
	unsigned int i;

	buf = kzalloc(count, GFP_KERNEL);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		return -ENOMEM;
	}
	ret = regmap_raw_read(phy->phy_regmap, start, buf, count);
	for (i = 0; i < count; i++)
		pr_info("[%lx]=%02x", start + i, buf[i]);

	kfree(buf);
	return ret;
}

static char *get_encoder_chip_name(int encoder_obj)
{
	switch (encoder_obj) {
	case ENCODER_CHIP_ID_NONE:
		return "none";
	case ENCODER_CHIP_ID_EDP_NCS8803:
		return "ncs8803";
	case ENCODER_CHIP_ID_EDP_NCS8805:
		return "ncs8805";
	case ENCODER_CHIP_ID_EDP_LT9721:
		return "lt9721";
	case ENCODER_CHIP_ID_EDP_LT6711:
		return "lt6711";
	case ENCODER_CHIP_ID_LVDS_LT8619:
		return "lt8619";
	case ENCODER_CHIP_ID_DP_LT8718:
		return "lt8718";
	case ENCODER_CHIP_ID_VGA_TRANSPARENT:
	case ENCODER_CHIP_ID_INTERNAL_DVO:
	case ENCODER_CHIP_ID_VGA_ADV7125:
		return "vga";
	case ENCODER_CHIP_ID_DVI_TRANSPARENT:
	case ENCODER_CHIP_ID_DVI_TFP410:
		return "dvi";
	case ENCODER_CHIP_ID_HDMI_TRANSPARENT:
	case ENCODER_CHIP_ID_INTERNAL_HDMI:
		return "hdmi";
	case ENCODER_CHIP_ID_EDP_TRANSPARENT:
	case ENCODER_CHIP_ID_INTERNAL_EDP:
	case ENCODER_CHIP_ID_EDP_CS5611AQ_S:
	case ENCODER_CHIP_ID_EDP_ICNM7601:
		return "edp";
	case ENCODER_CHIP_ID_INTERNAL_DP:
		return "dp";
	case ENCODER_CHIP_ID_HDMI_LT8618:
		return "lt8618";
	case ENCODER_CHIP_ID_HDMI_IT66121:
		return "it66121";
	case ENCODER_CHIP_ID_HDMI_MS7210:
		return "ms7210";
	case ENCODER_CHIP_ID_INTERNAL_COMPOSITE:
		return "composite";
	default:
		DRM_WARN("No ext encoder chip 0x%x.\n", encoder_obj);
		return "Unknown";
	}
}

static int bridge_phy_register_irq_num(struct loonggpu_bridge_phy *phy)
{
	int ret = -1;
	char irq_name[NAME_SIZE_MAX];
	struct loonggpu_dc_bridge *res;

	res = phy->res;
	phy->irq_num = -1;

	if(!phy->adev->dc->link_info[phy->display_pipe_index].encoder->has_ext_encoder)
		return 0;

	if (phy->hpd_funcs || phy->hpd_funcs->isr_thread || phy->hpd_funcs->irq_handler)
		return ret;

	if (phy->res->hotplug != IRQ)
		return ret;

	if (res->gpio_placement)
		res->irq_gpio += LS7A_GPIO_OFFSET;

	ret = gpio_is_valid(res->irq_gpio);
	if (!ret)
		goto error_gpio_valid;
	sprintf(irq_name, "%s-irq", res->chip_name);

	ret = gpio_request(res->irq_gpio, irq_name);
	if (ret)
		goto error_gpio_req;
	ret = gpio_direction_input(res->irq_gpio);
	if (ret)
		goto error_gpio_cfg;

	phy->irq_num = gpio_to_irq(res->irq_gpio);
	if (phy->irq_num < 0) {
		ret = phy->irq_num;
		DRM_ERROR("GPIO %d has no interrupt\n", res->irq_gpio);
		return ret;
	}

	DRM_DEBUG("[Bridge_phy] %s register irq num %d.\n", res->chip_name,
		  phy->irq_num);
	return 0;

error_gpio_cfg:
	DRM_ERROR("Failed to config gpio %d free it %d\n", res->irq_gpio, ret);
	gpio_free(res->irq_gpio);
error_gpio_req:
	DRM_ERROR("Failed to request gpio %d, %d\n", res->irq_gpio, ret);
error_gpio_valid:
	DRM_ERROR("Invalid gpio %d, %d\n", res->irq_gpio, ret);
	return ret;
}

static int bridge_phy_register_irq_handle(struct loonggpu_bridge_phy *phy)
{
	int ret = -1;
	irqreturn_t (*irq_handler)(int irq, void *dev);
	irqreturn_t (*isr_thread)(int irq, void *dev);

	if(!phy->adev->dc->link_info[phy->display_pipe_index].encoder->has_ext_encoder)
		return 0;

	if (phy->hpd_funcs || phy->hpd_funcs->isr_thread || phy->hpd_funcs->irq_handler)
		return ret;

	if (phy->res->hotplug != IRQ)
		return ret;

	if (phy->irq_num <= 0) {
		phy->connector->polled = DRM_CONNECTOR_POLL_CONNECT |
					 DRM_CONNECTOR_POLL_DISCONNECT;
		return ret;
	}

	if (phy->hpd_funcs && phy->hpd_funcs->isr_thread) {
		irq_handler = phy->hpd_funcs->irq_handler;
		isr_thread = phy->hpd_funcs->isr_thread;
	}

	ret = devm_request_threaded_irq(
		&phy->i2c_phy->dev, phy->irq_num, irq_handler, isr_thread,
		IRQF_ONESHOT | IRQF_SHARED | IRQF_TRIGGER_HIGH,
		phy->res->chip_name, phy);
	if (ret)
		goto error_irq;

	DRM_DEBUG_DRIVER("[Bridge_phy] %s register irq handler succeed %d.\n",
			 phy->res->chip_name, phy->irq_num);
	return 0;

error_irq:
	gpio_free(phy->res->irq_gpio);
	DRM_ERROR("Failed to request irq handler for irq %d.\n", phy->irq_num);
	return ret;
}

static int bridge_phy_hw_reset(struct loonggpu_bridge_phy *phy)
{
	if (phy->cfg_funcs && phy->cfg_funcs->hw_reset)
		phy->cfg_funcs->hw_reset(phy);

	return 0;
}

static int bridge_phy_misc_init(struct loonggpu_bridge_phy *phy)
{
	if (phy->misc_funcs && phy->misc_funcs->debugfs_init)
		phy->misc_funcs->debugfs_init(phy);

	return 0;
}

static int bridge_phy_regmap_init(struct loonggpu_bridge_phy *phy)
{
	int ret;
	struct regmap *regmap;

	mutex_init(&phy->ddc_status.ddc_bus_mutex);
	atomic_set(&phy->irq_status, 0);

	if (!phy->regmap_cfg)
		return 0;

	regmap = devm_regmap_init_i2c(phy->li2c->ddc_client,
				      phy->regmap_cfg);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		return -ret;
	}
	phy->phy_regmap = regmap;

	return 0;
}

static int bridge_phy_chip_id_verify(struct loonggpu_bridge_phy *phy)
{
	int ret;
	char str[NAME_SIZE_MAX] = "";

	if (phy->misc_funcs && phy->misc_funcs->chip_id_verify) {
		ret = phy->misc_funcs->chip_id_verify(phy, str);
		if (!ret)
			DRM_ERROR("Failed to verify chip %s, return [%s]\n",
				  phy->res->chip_name, str);
		lg_strncpy(phy->res->vendor_str, str, NAME_SIZE_MAX - 1);
		return ret;
	}

	return -ENODEV;
}

static int bridge_phy_video_config(struct loonggpu_bridge_phy *phy)
{
	if (phy->cfg_funcs && phy->cfg_funcs->video_input_cfg)
		phy->cfg_funcs->video_input_cfg(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->video_output_cfg)
		phy->cfg_funcs->video_output_cfg(phy);

	return 0;
}

static int bridge_phy_hdmi_config(struct loonggpu_bridge_phy *phy)
{
	if (phy->cfg_funcs && phy->cfg_funcs->hdmi_audio)
		phy->cfg_funcs->hdmi_audio(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->hdmi_csc)
		phy->cfg_funcs->hdmi_csc(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->hdmi_hdcp_init)
		phy->cfg_funcs->hdmi_hdcp_init(phy);

	return 0;
}

static int bridge_phy_sw_init(struct loonggpu_bridge_phy *phy)
{
	bridge_phy_chip_id_verify(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->sw_reset)
		phy->cfg_funcs->sw_reset(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->reg_init)
		phy->cfg_funcs->reg_init(phy);
	if (phy->cfg_funcs && phy->cfg_funcs->sw_enable)
		phy->cfg_funcs->sw_enable(phy);
	bridge_phy_video_config(phy);
	bridge_phy_hdmi_config(phy);

	DRM_DEBUG_DRIVER("[Bridge_phy] %s sw init completed\n",
			 phy->res->chip_name);

	return 0;
}

static int bridge_phy_internal_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_device *adev = dc_bridge->adev;

	switch(adev->chip) {
	case dev_7a2000:
		return bridge_phy_ls7a2000_init(dc_bridge);
	case dev_2k2000:
		return bridge_phy_ls2k2000_init(dc_bridge);
	case dev_2k3000:
		return bridge_phy_ls2k3000_init(dc_bridge);
	case dev_7a1000:
		return bridge_phy_ls7a1000_init(dc_bridge);
	case dev_9a1000:
		return bridge_phy_ls9a1000_init(dc_bridge);
	default:
		DRM_DEBUG_DRIVER("No matching chip! Skip internal bridge phy init\n");
		break;
	}

	return 0;
}

static int bridge_phy_encoder_obj_select(struct loonggpu_dc_bridge *dc_bridge)
{
	int ret = 0;

	switch (dc_bridge->encoder_obj) {
	case ENCODER_CHIP_ID_EDP_LT6711:
		ret = bridge_phy_lt6711_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_EDP_LT9721:
		ret = bridge_phy_lt9721_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_LVDS_LT8619:
		ret = bridge_phy_lt8619_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_EDP_NCS8805:
		ret = bridge_phy_ncs8805_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_DP_LT8718:
		ret = bridge_phy_lt8718_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_HDMI_LT8618:
		ret = bridge_phy_lt8618_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_HDMI_IT66121:
		ret = bridge_phy_it66121_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_HDMI_MS7210:
		ret = bridge_phy_ms7210_init(dc_bridge);
		break;
	case ENCODER_CHIP_ID_INTERNAL_DVO:
	case ENCODER_CHIP_ID_INTERNAL_HDMI:
	case ENCODER_CHIP_ID_INTERNAL_EDP:
	case ENCODER_CHIP_ID_EDP_CS5611AQ_S:
	case ENCODER_CHIP_ID_INTERNAL_DP:
	case ENCODER_CHIP_ID_NONE:
	case ENCODER_CHIP_ID_VGA_TRANSPARENT:
	case ENCODER_CHIP_ID_HDMI_TRANSPARENT:
	case ENCODER_CHIP_ID_EDP_TRANSPARENT:
	case ENCODER_CHIP_ID_INTERNAL_COMPOSITE:
	case ENCODER_CHIP_ID_EDP_ICNM7601:
	case ENCODER_CHIP_ID_VGA_ADV7125:
	case ENCODER_CHIP_ID_DVI_TFP410:
		ret = bridge_phy_internal_init(dc_bridge);
		break;
	default:
		DRM_DEBUG_DRIVER("No matching chip! Skip bridge phy init\n");
		break;
	}

	return ret;
}

int bridge_phy_init(struct loonggpu_bridge_phy *phy)
{
	bridge_phy_hw_reset(phy);
	bridge_phy_regmap_init(phy);
	bridge_phy_register_irq_num(phy);

	bridge_phy_misc_init(phy);
	bridge_phy_bind(phy);
	bridge_phy_sw_init(phy);
	bridge_phy_register_irq_handle(phy);
	DRM_INFO("[Bridge_phy] %s init finish.\n", phy->res->chip_name);

	return 0;
}

/**
 * @section Bridge-phy interface
 */
struct loonggpu_bridge_phy *bridge_phy_alloc(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *bridge_phy;
	int index = dc_bridge->display_pipe_index;
	struct connector_resource *connector_res =
			dc_bridge->adev->dc->link_info[index].connector;

	bridge_phy = lg_bridge_phy_alloc(dc_bridge->adev, &bridge_funcs);
	if (IS_ERR(bridge_phy)) {
		DRM_ERROR("Failed to alloc loonggpu bridge phy!\n");
		return NULL;
	}

	bridge_phy->display_pipe_index = dc_bridge->display_pipe_index;
	bridge_phy->bridge.driver_private = bridge_phy;
	bridge_phy->adev = dc_bridge->adev;
	bridge_phy->res = dc_bridge;
	bridge_phy->encoder = &dc_bridge->adev->mode_info.encoders[index]->base;
	bridge_phy->li2c = dc_bridge->adev->i2c[index];
	bridge_phy->connector_type = connector_res->type;
	dc_bridge->adev->mode_info.encoders[index]->bridge = bridge_phy;

	return bridge_phy;
}

struct loonggpu_dc_bridge
*dc_bridge_construct(struct loonggpu_dc *dc,
		     struct encoder_resource *encoder_res,
		     struct connector_resource *connector_res)
{
	struct loonggpu_dc_bridge *dc_bridge;
	const char *chip_name;
	u32 link;

	if (IS_ERR_OR_NULL(dc) ||
	    IS_ERR_OR_NULL(encoder_res) ||
	    IS_ERR_OR_NULL(connector_res))
		return NULL;

	dc_bridge = kzalloc(sizeof(*dc_bridge), GFP_KERNEL);
	if (IS_ERR_OR_NULL(dc_bridge))
		return NULL;

	link = encoder_res->base.link;
	if (link >= DC_DVO_MAXLINK)
		return false;

	dc_bridge->adev = dc->adev;
	dc_bridge->dc = dc;
	dc_bridge->display_pipe_index = link;

	dc_bridge->encoder_obj = encoder_res->chip;
	chip_name = get_encoder_chip_name(dc_bridge->encoder_obj);
	snprintf(dc_bridge->chip_name, NAME_SIZE_MAX, "%s", chip_name);
	dc_bridge->i2c_bus_num = encoder_res->i2c_id;
	dc_bridge->i2c_dev_addr = encoder_res->chip_addr;

	dc_bridge->hotplug = connector_res->hotplug;
	dc_bridge->edid_method = connector_res->edid_method;
	dc_bridge->gpio_placement = connector_res->gpio_placement;
	dc_bridge->irq_gpio = connector_res->irq_gpio;

	switch (dc_bridge->encoder_obj) {
		case ENCODER_CHIP_ID_EDP_NCS8805:
		case ENCODER_CHIP_ID_EDP_LT9721:
		case ENCODER_CHIP_ID_EDP_LT6711:
		case ENCODER_CHIP_ID_LVDS_LT8619:
		case ENCODER_CHIP_ID_DP_LT8718:
		case ENCODER_CHIP_ID_HDMI_IT66121:
		case ENCODER_CHIP_ID_HDMI_LT8618:
		case ENCODER_CHIP_ID_HDMI_MS7210:
			dc->link_info[link].encoder->has_ext_encoder = true;
			break;
		default:
			dc->link_info[link].encoder->has_ext_encoder = false;
			break;
	}

	DRM_INFO("Encoder Parse: #0x%02x-%s feature:%d type:%s hotplug:%s.\n",
		 dc_bridge->encoder_obj, dc_bridge->chip_name, encoder_res->feature,
		 encoder_type_to_str(encoder_res->type),
		 hotplug_to_str(dc_bridge->hotplug));
	DRM_INFO("Encoder Parse: config_type:%s edid_method:%s reset_gpio:%d.\n",
		 encoder_config_to_str(encoder_res->config_type),
		 edid_method_to_str(dc_bridge->edid_method),
		 (encoder_res->feature >= 1) ? encoder_res->reset_gpio : 0);

	return dc_bridge;
}

static int bridge_phy_internal_register(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_device *adev = dc_bridge->adev;

	switch(adev->chip) {
	case dev_7a2000:
		return internal_bridge_ls7a2000_register(dc_bridge);
	case dev_2k2000:
		return internal_bridge_ls2k2000_register(dc_bridge);
	case dev_2k3000:
		return internal_bridge_ls2k3000_register(dc_bridge);
	case dev_7a1000:
		return internal_bridge_ls7a1000_register(dc_bridge);
	case dev_9a1000:
		return internal_bridge_ls9a1000_register(dc_bridge);
	default:
		DRM_DEBUG_DRIVER("No matching chip! Skip internal bridge phy register\n");
		break;
	}

	return 0;
}

int loonggpu_dc_bridge_init(struct loonggpu_device *adev, int link_index)
{
	struct loonggpu_dc_bridge *dc_bridge =
					adev->dc->link_info[link_index].bridge;
	int ret;

	if (link_index >= 4)
		return -1;

	bridge_phy_internal_register(dc_bridge);
	ret = bridge_phy_encoder_obj_select(dc_bridge);
	if (ret)
		return ret;

	return ret;
}

void loonggpu_bridge_suspend(struct loonggpu_device *adev)
{
	struct loonggpu_bridge_phy *phy;
	int i;

	for (i = 0; i < adev->dc->links; i++) {
		phy = adev->mode_info.encoders[i]->bridge;
		if (phy && phy->cfg_funcs && phy->cfg_funcs->suspend) {
			phy->cfg_funcs->suspend(phy);
			DRM_INFO("[Bridge_phy] %s suspend completed.\n",
					phy->res->chip_name);
		}
	}
}

void loonggpu_bridge_resume(struct loonggpu_device *adev)
{
	struct loonggpu_bridge_phy *phy = NULL;
	int i;

	for (i = 0; i < adev->dc->links; i++) {
		phy = adev->mode_info.encoders[i]->bridge;
		if (phy) {
			if (phy->cfg_funcs &&  phy->cfg_funcs->resume)
				phy->cfg_funcs->resume(phy);
			else {
				bridge_phy_hw_reset(phy);
				bridge_phy_sw_init(phy);
			}
			DRM_INFO("[Bridge_phy] %s resume completed.\n",
					phy->res->chip_name);
		}
	}
}
