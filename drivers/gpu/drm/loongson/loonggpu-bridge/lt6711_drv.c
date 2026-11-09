
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_vbios.h"
#include "bridge_phy.h"

static enum drm_mode_status lt6711_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
	if (mode->hdisplay < 1920)
		return MODE_BAD;
	if (mode->vdisplay < 1080)
		return MODE_BAD;

	return MODE_OK;
}

static int lt6711_get_modes(struct loonggpu_bridge_phy *phy,
			    struct drm_connector *connector)
{
	struct loonggpu_dc_i2c *i2c = phy->li2c;
	struct edid *edid;
	unsigned int count = 0;

	edid = drm_get_edid(connector, &i2c->adapter);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		kfree(edid);
	} else {
		DRM_ERROR("LT6711 edid is invalid.\n");
	}

	return count;
}

static enum hpd_status lt6711_get_hpd_status(struct loonggpu_bridge_phy *phy)
{
	return hpd_status_plug_on;
}

static const struct bridge_phy_cfg_funcs lt6711_cfg_funcs = {
	.mode_valid = lt6711_mode_valid,
};

static struct bridge_phy_ddc_funcs lt6711_ddc_funcs = {
	.get_modes = lt6711_get_modes,
};

static struct bridge_phy_hpd_funcs lt6711_hpd_funcs = {
	.get_hpd_status = lt6711_get_hpd_status,
};

int bridge_phy_lt6711_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *lt6711_phy;

	lt6711_phy = bridge_phy_alloc(dc_bridge);
	lt6711_phy->cfg_funcs = &lt6711_cfg_funcs;
	lt6711_phy->ddc_funcs = &lt6711_ddc_funcs;
	lt6711_phy->hpd_funcs = &lt6711_hpd_funcs;
	lt6711_phy->is_ext_encoder = true;
	lt6711_phy->regmap_cfg = NULL;

	return bridge_phy_init(lt6711_phy);
}

int bridge_phy_lt6711_remove(struct loonggpu_dc_bridge *phy)
{
	return 0;
}
