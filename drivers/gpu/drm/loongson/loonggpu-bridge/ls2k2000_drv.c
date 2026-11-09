#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "bridge_phy.h"

static enum drm_connector_status ls2k2000_get_connect_status(struct loonggpu_bridge_phy *phy)
{
	struct drm_connector *connector = phy->connector;
	struct loonggpu_device *adev = phy->res->adev;
	struct loonggpu_dc_crtc *crtc = adev->dc->link_info[phy->connector->index].crtc;
	enum drm_connector_status status = connector_status_disconnected;
	u32 reg_val = dc_readl(adev, gdc_reg->global_reg.hdmi_hp_stat);

	crtc->intf[0].connected = false;
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
				if (reg_val & 0x1)
					status = connector_status_connected;
				break;
			case 1:
				if (reg_val & 0x2)
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

static void ls2k2000_hpd_init(struct loonggpu_bridge_phy *phy, struct loonggpu_connector* lconnector, bool has_ext_encoder)
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
		} else if (lconnector->connector_id == 1) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C1;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI1_NULL;
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

static struct bridge_phy_hpd_funcs ls2k2000_hpd_funcs = {
	.hpd_init = ls2k2000_hpd_init,
        .get_connect_status = ls2k2000_get_connect_status,
};

static enum drm_mode_status ls2k2000_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
	if ((mode->hdisplay > 1920 && connector->index == 1) ||
			mode->hdisplay > 4096 ||
			mode->hdisplay % 8 ||
			(mode->vdisplay > 1200 && connector->index == 1) ||
			mode->vdisplay > 2160 ||
			mode->vdisplay < 480)
		return MODE_BAD;

	if (mode->clock > 340000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct bridge_phy_cfg_funcs ls2k2000_cfg_funcs = {
        .mode_valid = ls2k2000_mode_valid,
};

int internal_bridge_ls2k2000_register(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls2k2000_phy;
	int index = dc_bridge->display_pipe_index;
	struct connector_resource *connector_res =
			dc_bridge->adev->dc->link_info[index].connector;

	ls2k2000_phy = kzalloc(sizeof(*ls2k2000_phy), GFP_KERNEL);
	if (IS_ERR(ls2k2000_phy)) {
		DRM_ERROR("Failed to alloc loonggpu bridge phy!\n");
		return -1;
	}

	ls2k2000_phy->display_pipe_index = index;
	ls2k2000_phy->bridge.driver_private = ls2k2000_phy;
	ls2k2000_phy->adev = dc_bridge->adev;
	ls2k2000_phy->res = dc_bridge;
	ls2k2000_phy->li2c = dc_bridge->adev->i2c[index];
	ls2k2000_phy->connector_type = connector_res->type;

	ls2k2000_phy->cfg_funcs = &ls2k2000_cfg_funcs;
	ls2k2000_phy->hpd_funcs = &ls2k2000_hpd_funcs;

	dc_bridge->internal_bp = ls2k2000_phy;
	DRM_DEBUG_DRIVER("internal bridge phy register success!\n");

	return 0;
}

int bridge_phy_ls2k2000_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls2k2000_phy;

	ls2k2000_phy = bridge_phy_alloc(dc_bridge);

	return bridge_phy_init(ls2k2000_phy);
}

