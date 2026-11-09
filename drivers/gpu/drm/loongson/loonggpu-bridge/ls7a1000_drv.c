#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "bridge_phy.h"

static enum drm_connector_status ls7a1000_get_connect_status(struct loonggpu_bridge_phy *phy)
{
	struct drm_connector *connector = phy->connector;
	struct loonggpu_device *adev = phy->adev;
	struct loonggpu_dc_crtc *crtc = adev->dc->link_info[phy->connector->index].crtc;
	enum drm_connector_status status = connector_status_disconnected;

	if (connector->polled == 0)
		status = connector_status_connected;
	else if (connector->polled == (DRM_CONNECTOR_POLL_CONNECT
				     | DRM_CONNECTOR_POLL_DISCONNECT)) {
		if (is_connected(connector))
			status = connector_status_connected;
		else
			status = connector_status_disconnected;
	} else if (connector->polled == DRM_CONNECTOR_POLL_HPD) {
		if (phy->hpd_funcs && phy->hpd_funcs->get_connect_status)
			status = phy->hpd_funcs->get_connect_status(phy);
		else {
			switch (phy->connector->index) {
			case 0:
				if (adev->vga_hpd_status == connector_status_unknown)
					status = connector_status_unknown;

				if (status != adev->vga_hpd_status)
					status = connector_status_connected;
				break;
			}
		}
	}

	if (status == connector_status_connected)
		crtc->intf[0].connected = true;
	else
		crtc->intf[0].connected = false;

	return status;
}

static void ls7a1000_hpd_init(struct loonggpu_bridge_phy *phy, struct loonggpu_connector* lconnector, bool has_ext_encoder)
{
	struct loonggpu_device *adev = phy->adev;
	struct connector_resource *connector_res = adev->dc->link_info[lconnector->connector_id].connector;

	if (has_ext_encoder) {
		if (lconnector->connector_id == 0) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C0;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI0_NULL;
		} else if (lconnector->connector_id == 1) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C1;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI1_NULL;
		}
	} else {
		if (lconnector->connector_id == 0) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C0;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI0;
			lconnector->irq_source_vga_hpd = DC_IRQ_SOURCE_HPD_VGA;
		} else if (lconnector->connector_id == 1) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C1;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI1;
		}
	}

	if (connector_res->type == DRM_MODE_CONNECTOR_VGA &&
	    connector_res->hotplug == IRQ)
		connector_res->hotplug = POLLING;

	switch (connector_res->hotplug) {
	case IRQ:
		lconnector->base.polled = DRM_CONNECTOR_POLL_HPD;
		break;
	case POLLING:
	default:
		lconnector->base.polled = DRM_CONNECTOR_POLL_CONNECT |
					  DRM_CONNECTOR_POLL_DISCONNECT;
		break;
	case FORCE_ON:
		lconnector->base.polled = 0;
		break;
	}
}

static struct bridge_phy_hpd_funcs ls7a1000_hpd_funcs = {
	.hpd_init = ls7a1000_hpd_init,
	.get_connect_status = ls7a1000_get_connect_status,
};

static enum drm_mode_status ls7a1000_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
	if (mode->hdisplay > 2048 ||
			mode->hdisplay == 1680 ||
			mode->hdisplay == 1440 ||
			mode->hdisplay % 8 ||
			mode->vdisplay > 2048 ||
			mode->vdisplay < 480)
		return MODE_BAD;

	if (mode->clock > 200000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct bridge_phy_cfg_funcs ls7a1000_cfg_funcs = {
        .mode_valid = ls7a1000_mode_valid,
};

int internal_bridge_ls7a1000_register(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls7a1000_phy;
	int index = dc_bridge->display_pipe_index;
	struct connector_resource *connector_res =
			dc_bridge->adev->dc->link_info[index].connector;

	ls7a1000_phy = kzalloc(sizeof(*ls7a1000_phy), GFP_KERNEL);
	if (IS_ERR(ls7a1000_phy)) {
		DRM_ERROR("Failed to alloc loonggpu bridge phy!\n");
		return -1;
	}

	ls7a1000_phy->display_pipe_index = index;
	ls7a1000_phy->bridge.driver_private = ls7a1000_phy;
	ls7a1000_phy->adev = dc_bridge->adev;
	ls7a1000_phy->res = dc_bridge;
	ls7a1000_phy->li2c = dc_bridge->adev->i2c[index];
	ls7a1000_phy->connector_type = connector_res->type;

	ls7a1000_phy->cfg_funcs = &ls7a1000_cfg_funcs;
	ls7a1000_phy->hpd_funcs = &ls7a1000_hpd_funcs;

	dc_bridge->internal_bp = ls7a1000_phy;
	DRM_DEBUG_DRIVER("internal bridge phy register success!\n");

	return 0;
}

int bridge_phy_ls7a1000_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls7a1000_phy;

	ls7a1000_phy = bridge_phy_alloc(dc_bridge);

	return bridge_phy_init(ls7a1000_phy);
}
