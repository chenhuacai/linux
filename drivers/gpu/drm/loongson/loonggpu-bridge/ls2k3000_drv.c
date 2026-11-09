#include <drm/drm_atomic_helper.h>
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_dp.h"
#include "bridge_phy.h"
#include "loonggpu_dc_encoder.h"

static enum drm_connector_status ls2k3000_get_connect_status(struct loonggpu_bridge_phy *phy)
{
	struct drm_connector *connector = phy->connector;
	struct loonggpu_device *adev = phy->adev;
	struct loonggpu_dc_crtc *crtc = adev->dc->link_info[phy->connector->index].crtc;
	enum drm_connector_status status = connector_status_disconnected;
	struct connector_resource *connector_res;
	struct encoder_resource *encoder_res;
	u32 reg_val;
	u32 index;
	int i;

	index = phy->connector->index;
	connector_res = adev->dc->link_info[index].connector;
	encoder_res = adev->dc->link_info[index].encoder->resource;

	if (connector->polled == 0) {
		status = connector_status_connected;
		crtc->intf[phy->connector->index].connected = true;
		/* Fixme: actually, we should not use index of contector to index intf, and should change later......*/
	} else {
		if (phy->hpd_funcs && phy->hpd_funcs->get_connect_status)
			status = phy->hpd_funcs->get_connect_status(phy);

		for (i = 0; i < crtc->interfaces; i++) {
			crtc->intf[i].connected = false;
			switch (crtc->intf[i].type) {
			case INTERFACE_HDMI:
				reg_val = dc_readl(adev, gdc_reg->global_reg.hdmi_hp_stat);
				if (reg_val & 0x1) {
					crtc->intf[i].connected = true;
					if (crtc->dc->adev->loongson_hdmi_hda_rmmio)
						writeb(0x1, crtc->dc->adev->loongson_hdmi_hda_rmmio + 0x2254);
				} else {
					if (crtc->dc->adev->loongson_hdmi_hda_rmmio)
						writeb(0x0, crtc->dc->adev->loongson_hdmi_hda_rmmio + 0x2254);
				}
				break;
			case INTERFACE_DP:
				/*
				* Don't clear dp_status, which is set by interrupt asynchronously.
				* */
				if ((connector_res->multi_interface && connector_res) ||
					(encoder_res->chip == ENCODER_CHIP_ID_INTERNAL_DP && encoder_res)) {
					if (adev->dp_status[1] & 0x2) {
						crtc->intf[i].connected = true;
					}
				}
				break;
			case INTERFACE_EDP:
				/*
				* Don't clear dp_status, which is set by interrupt asynchronously.
				* */
				if (adev->dp_status[0] & 0x1) {
					crtc->intf[i].connected = true;
				}
				break;
			}
		}
	}

	for (i = 0; i < crtc->interfaces; i++) {
		if (crtc->intf[i].connected == true)
			status = connector_status_connected;
	}

	return status;
}

static void ls2k3000_hpd_init(struct loonggpu_bridge_phy *phy, struct loonggpu_connector* lconnector, bool has_ext_encoder)
{
	struct loonggpu_device *adev = phy->adev;
	struct connector_resource *connector_res = adev->dc->link_info[lconnector->connector_id].connector;

	if (has_ext_encoder) {
		if (lconnector->connector_id == 0) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C0;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_EDP_NULL;
		} else if (lconnector->connector_id == 1) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C1;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI0_NULL;
			lconnector->irq_source_hpd[1] = DC_IRQ_SOURCE_HPD_DP_NULL;
		}
	} else {
		if (lconnector->connector_id == 0) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C0;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_EDP;
		} else if (lconnector->connector_id == 1) {
			lconnector->irq_source_i2c = DC_IRQ_SOURCE_I2C1;
			lconnector->irq_source_hpd[0] = DC_IRQ_SOURCE_HPD_HDMI0;
			lconnector->irq_source_hpd[1] = DC_IRQ_SOURCE_HPD_DP;
		}
	}

	if (connector_res->hotplug != FORCE_ON)
		connector_res->hotplug = IRQ;

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

static struct bridge_phy_hpd_funcs ls2k3000_hpd_funcs = {
	.hpd_init = ls2k3000_hpd_init,
	.get_connect_status = ls2k3000_get_connect_status,
};

static enum drm_mode_status ls2k3000_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
	if (mode->hdisplay > 4096 ||
	    mode->vdisplay > 2160 ||
	    mode->vdisplay < 480 ||
	    mode->hdisplay % 8)
		return MODE_BAD;

	if (mode->clock > 600000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const u8 edid_header[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

static unsigned int aux_transfer(struct loonggpu_device *adev,
				int intf,
				unsigned int pkt_size,
				unsigned char *data)
{
	aux_msg_t aux_data;
	unsigned int j = 0;
	int ret = 0;

	aux_data.addr = 0x50;
	aux_data.size = pkt_size;
	aux_data.data_low = 0;
	aux_data.data_high = 0;
	ret = aux_config(adev, READ, aux_data, intf);
	if (ret == 0) {
		for (j = 0; j < pkt_size; j++) {
			DRM_DEBUG_DRIVER("aux data read: [aux base: 0x%x]: 0x%x\n", gdc_reg->dp_reg[intf].aux_monitor1 + j,
						readb(adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_monitor1 + j));
			data[j] = readb(adev->loongson_dc_rmmio + gdc_reg->dp_reg[intf].aux_monitor1 + j);
		}
	} else {
		DRM_ERROR("f:%s error ret:%d addr: 0x50 size:%d\r\n", __func__, ret, pkt_size);
	}

	return ret;
}

static bool drm_edid_block_checksum(const unsigned char *edid)
{
	unsigned char checksum = 0, i;

	for (i = 0; i < 128; i++) {
		checksum += edid[i];
	}

	return !checksum;
}

static bool read_one_packet(struct loonggpu_device *adev, int intf, int pkt_size, unsigned char *data)
{
	int retry_cnt = 10;
	int ret;
	int i;

	for (i = 0; i < retry_cnt; i++) {
		ret = aux_transfer(adev, intf, pkt_size, data);

		if (!ret) {
			return true;
		} else if (ret == 1) {
			DRM_DEBUG_DRIVER("No Dp device detected! \n");
			return false;
		}
	}

	return false;
}

static bool header_is_ok(struct loonggpu_device *adev, int intf, unsigned int pkt_size)
{
	unsigned char data[256] = {0};
	int index;
	unsigned pkt_cnt = 256/pkt_size;

	for (index = 0; index < (7/pkt_size); index++) {
		if (false == read_one_packet(adev, intf, pkt_size, &data[index * pkt_size])) {
			return false;
		}
	}

	for (; index < pkt_cnt; index++) {
		if (false == read_one_packet(adev, intf, pkt_size, &data[index * pkt_size])) {
			return false;
		}
		if (!memcmp(&data[(index - (7/pkt_size)) * pkt_size], edid_header, 8)) {
			return true;
		}
	}

	return false;
}

static void edid_correct_extensions(unsigned char *block0)
{
	unsigned int i, sum = 0;

	if (block0[126] <= 1)
		return;

	block0[126] = 0x1;
	for (i = 0; i < 127; i++)
		sum += block0[i];

	block0[127] = 0x100 - sum;
}

static bool read_edid_form_aux(struct loonggpu_device *adev, int intf, unsigned char *edid)
{
	unsigned char internal_edid[2*EDID_LENGTH] = {0};
	unsigned char block_num;
	unsigned int value;
	int index;
	unsigned int pkt_size = 0x8; /* can be 1, 2, 4, 8 */
	unsigned pkt_cnt = (EDID_LENGTH * 2)/pkt_size;
	aux_msg_t aux_data;

	if (adev->chip == dev_2k3000 && adev->dc_revision == 0x1)
		aux_preinit_for_edid_2k3000ca(adev, intf);

	aux_data.addr = DPCD_SET_POWER_ADDR;
	aux_data.size = 1;
	aux_data.data_low = DP_POWER_STATE_D3;
	aux_data.data_high = 0;
	aux_config(adev,  WRITE, aux_data, intf);
	usleep_range(1000, 2000);

	aux_data.addr = DPCD_SET_POWER_ADDR;
	aux_data.size = 1;
	aux_data.data_low = DP_POWER_STATE_D0;
	aux_data.data_high = 0;
	aux_config(adev,  WRITE, aux_data, intf);
	usleep_range(1000, 2000);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel8);
	value |= 0x3 | (0xff << 16);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel8, value);

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel0);
	value &= ~(0x1 << 3);
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel0, value);

	/* find out header */
	if (!header_is_ok(adev, intf, pkt_size)) {
		return false;
	}

	memcpy(internal_edid, edid_header, 8);

	/* read block0 */
	pkt_cnt = 128/pkt_size;
	for (index = 8/pkt_size; index < pkt_cnt; index++) {
		if (false == read_one_packet(adev, intf, pkt_size, &internal_edid[index * pkt_size])) {
			return false;
		}
	}

	if (!drm_edid_block_checksum(internal_edid)) {
		return false;
	}

	/* Limit to a maximum of 256 bytes in EDID.
	 * Since 2k3000 does not support it.
	 */
	edid_correct_extensions(internal_edid);

	/* read block1 */
	block_num = internal_edid[126];
	if (block_num) {
		for (index = pkt_cnt; index < (2 * pkt_cnt) ; index++) {
			if (false == read_one_packet(adev, intf, pkt_size, &internal_edid[index * pkt_size])) {
				return false;
			}
		}
		if (!drm_edid_block_checksum(&internal_edid[128])) {
			return false;
		}
	}

	value = dc_readl(adev, gdc_reg->dp_reg[intf].aux_channel8);
	value &= ~0x1;
	dc_writel(adev, gdc_reg->dp_reg[intf].aux_channel8, value);

	memcpy(edid, internal_edid, EDID_LENGTH * 2);

	return true;
}

bool ls2k3000_interface_status_changed(struct drm_connector *connector, struct loonggpu_dc_crtc *crtc)
{
	bool old_hdmi_status = false, current_hdmi_status = false;
	bool old_edp_status = false, current_edp_status = false;
	bool old_dp_status = false, current_dp_status = false;
	bool status_changed = false;
	int i;

	/*
	* Retrieve the previous connection status of the display connector
	*/
	for (i = 0; i < crtc->interfaces; i++) {
		switch (crtc->intf[i].type) {
		case INTERFACE_HDMI:
			old_hdmi_status = crtc->intf[i].connected;
			break;
		case INTERFACE_DP:
			old_dp_status = crtc->intf[i].connected;
			break;
		case INTERFACE_EDP:
			old_edp_status = crtc->intf[i].connected;
			break;
		}
	}

	/**
	 * ​​Retrieve the current connection status of the display connector
	 */
	drm_helper_probe_detect(connector, NULL, false);
	for (i = 0; i < crtc->interfaces; i++) {
		switch (crtc->intf[i].type) {
		case INTERFACE_HDMI:
			current_hdmi_status = crtc->intf[i].connected;
			break;
		case INTERFACE_DP:
			current_dp_status = crtc->intf[i].connected;
			break;
		case INTERFACE_EDP:
			current_edp_status = crtc->intf[i].connected;
			break;
		}
	}

    if (connector->index != 0) {
        status_changed = (old_hdmi_status != current_hdmi_status) ||
                        (old_dp_status != current_dp_status);
    } else {
        status_changed = (old_edp_status != current_edp_status);
    }

	return status_changed;
}

/**
 * process_edp_edid - Process EDID data for an Embedded DisplayPort (eDP) connector
 * @connector: Pointer to the DRM connector associated with the eDP display
 * @valid_edid: Pointer to the raw EDID data buffer to be processed
 *
 * This function processes the Extended Display Identification Data (EDID) for an eDP panel.
 * It allocates memory for the EDID structure, copies the provided raw EDID data into it,
 * updates the connector's EDID property, and adds the supported display modes to the
 * connector's mode list. The function is typically called during eDP panel initialization
 * or when handling a hotplug event to process newly read EDID information.
 *
 * The caller is responsible for ensuring that the @valid_edid points to a valid EDID block
 * of at least EDID_LENGTH * 2 bytes. The function handles memory allocation failures gracefully.
 *
 * Return: Number of display modes successfully added to the connector. Returns 0 if
 *         memory allocation for the EDID structure fails.
 */
static unsigned int process_edp_edid(struct drm_connector *connector,
                                        unsigned char *valid_edid)
{
	struct edid *edid = kzalloc(EDID_LENGTH * 2, GFP_KERNEL);
	unsigned long count = 0;

	if (!edid)
		return 0;

	memcpy(edid, valid_edid, EDID_LENGTH * 2);
	drm_connector_update_edid_property(connector, edid);
	count = drm_add_edid_modes(connector, edid);
	kfree(edid);

	return count;
}

/**
 * process_dp_edid - Process and apply EDID data for a DisplayPort connector
 * @dp_connector: The DRM connector associated with the DisplayPort
 * @valid_edid: Pointer to the raw EDID data buffer to be processed
 *
 * This function processes the Extended Display Identification Data (EDID) for a DisplayPort
 * display. It allocates memory for the EDID structure, copies the provided raw EDID data
 * into it, updates the connector's EDID property, adds the supported display modes to the
 * connector's mode list, and finally applies a filter to retain only unique progressive modes.
 *
 * It is typically called during DisplayPort connector initialization or in response to a
 * hotplug event when new EDID data is available.
 *
 * Caller is responsible for freeing the returned EDID structure using kfree().
 *
 * Return: A pointer to the newly allocated and processed &struct edid on success.
 *         Returns %NULL if memory allocation for the EDID structure fails.
 */
static struct edid *process_dp_edid(struct drm_connector *dp_connector,
                                   unsigned char *valid_edid)
{
	struct edid *dp_edid = kzalloc(EDID_LENGTH * 2, GFP_KERNEL);

	if (!dp_edid)
		return NULL;

	memcpy(dp_edid, valid_edid, EDID_LENGTH * 2);
	drm_add_edid_modes(dp_connector, dp_edid);
	filter_unique_progressive_modes(dp_connector);

	return dp_edid;
}

/**
 * combine_edid_results - Combine EDID results from HDMI and DP interfaces
 * @connector: Primary DRM connector object
 * @dp_connector: DisplayPort connector object
 * @hdmi_edid: EDID data from HDMI interface, may be NULL
 * @dp_edid: EDID data from DisplayPort interface, may be NULL
 * @current_count: Current mode count before combination
 *
 * This function merges Extended Display Identification Data (EDID) from both HDMI
 * and DisplayPort interfaces to determine the optimal set of display modes. It
 * handles three main scenarios:
 * 1. Both HDMI and DP EDID are available: shows filtered modes for both connectors
 *    and returns the intersection of supported modes
 * 2. Only DP EDID is available: updates the primary connector with DP EDID and
 *    adds the corresponding modes
 * 3. Other cases: returns the current mode count without modification
 *
 * The intersection logic ensures that only modes supported by both interfaces are
 * exposed when both connections are active, providing compatibility across
 * different display technologies.
 *
 * Return: Number of display modes after combination. Returns the intersection count
 *         when both EDIDs are present, DP-only mode count when only DP is available,
 *         or the original current_count in other cases.
 */
static unsigned int combine_edid_results(struct drm_connector *connector,
                                       struct drm_connector *dp_connector,
                                       struct edid *hdmi_edid, struct edid *dp_edid,
                                       unsigned long current_count)
{
	if (dp_edid && hdmi_edid) {
		show_filtered_modes(connector);
		show_filtered_modes(dp_connector);
		return drm_connector_edid_intersection(connector, dp_connector);
	} else if (dp_edid && !hdmi_edid) {
		drm_connector_update_edid_property(connector, dp_edid);
		return drm_add_edid_modes(connector, dp_edid);
	}

	return current_count;
}

static void probed_modes_clean(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *tmp;

	if (!connector || list_empty(&connector->probed_modes))
		return;

	list_for_each_entry_safe(mode, tmp, &connector->probed_modes, head) {
		list_del(&mode->head);
		drm_mode_destroy(connector->dev, mode); /* Free the mode memory */
	}
}

/**
 * process_display_interfaces - Process EDID data for HDMI and DisplayPort interfaces
 * @adev: Pointer to the Loongson GPU device structure
 * @index: Index identifier for the display controller
 * @connector: Primary DRM connector object (typically for HDMI)
 * @dc_crtc: Display controller CRTC containing interface configuration
 * @phy: Physical bridge layer for I2C/auxiliary communication
 *
 * This function handles the retrieval and processing of Extended Display Identification Data
 * (EDID) for both HDMI and DisplayPort interfaces attached to a display controller. It first
 * attempts to read and process the HDMI EDID if the interface is connected, then attempts to
 * read the DisplayPort EDID via the AUX channel if that interface is connected. Results from
 * both interfaces are combined to determine the final set of supported display modes.
 *
 * For the DisplayPort interface, a virtual connector is created to manage its modes. The function
 * ensures proper cleanup of all allocated resources (EDID blocks, virtual connectors) before returning.
 *
 * Return: Total number of display modes successfully added and combined for the primary connector.
 */
static unsigned int process_display_interfaces(struct loonggpu_device *adev, int index,
                                              struct drm_connector *connector,
                                              struct loonggpu_dc_crtc *dc_crtc,
                                              struct loonggpu_bridge_phy *phy)
{
	struct edid *hdmi_edid = NULL, *dp_edid = NULL;
	struct drm_connector *dp_connector = NULL;
	unsigned char valid_edid[256];
	unsigned long count = 0;


	/* Process HDMI interface */
	if (dc_crtc->intf[0].type == INTERFACE_HDMI && dc_crtc->intf[0].connected) {
		hdmi_edid = drm_get_edid(connector, &phy->li2c->adapter);
		if (hdmi_edid) {
			drm_connector_update_edid_property(connector, hdmi_edid);
			count = drm_add_edid_modes(connector, hdmi_edid);
			filter_unique_progressive_modes(connector);
		}
	}

	/* Process DisplayPort interface */
	if (dc_crtc->intf[1].type == INTERFACE_DP && dc_crtc->intf[1].connected) {
		dp_aux_lock(adev, index, true);
		if (read_edid_form_aux(adev, index, valid_edid)) {
			dp_connector = &adev->mode_info.dp_connector->base;
			probed_modes_clean(dp_connector);
			dp_edid = process_dp_edid(dp_connector, valid_edid);
		}
		dp_aux_lock(adev, index, false);
	}

	/* Combine results from both interfaces */
	count = combine_edid_results(connector, dp_connector, hdmi_edid, dp_edid, count);

	/* Cleanup resources */
	kfree(hdmi_edid);
	kfree(dp_edid);

	if (dp_connector)
		probed_modes_clean(dp_connector);

	return count;
}

/**
 * ls2k3000_dc_read_edid - Read and process EDID data for Loongson display controller
 * @adev: Loongson GPU device instance
 * @index: Display controller index (0 for primary, 1 for secondary)
 * @connector: DRM connector to populate with display modes
 * @edid: EDID data buffer
 *
 * Return: Number of valid display modes found, or 0 on error.
 */
static unsigned int ls2k3000_dc_read_edid(struct loonggpu_device *adev, int index,
                            struct drm_connector *connector, struct edid *edid)
{
	struct loonggpu_dc_crtc *dc_crtc = adev->dc->link_info[index].crtc;
	struct loonggpu_bridge_phy *phy = adev->mode_info.encoders[connector->index]->bridge;
	unsigned char valid_edid[256];
	unsigned long mode_count = 0;

	/* Primary display: direct AUX read */
	if (!index) {
		dp_aux_lock(adev, index, true);
		if (!read_edid_form_aux(adev, index, valid_edid)) {
			dp_aux_lock(adev, index, false);
			DRM_ERROR("Primary AUX EDID read failed\n");
			return 0;
		}
		dp_aux_lock(adev, index, false);
		return process_edp_edid(connector, valid_edid);
	}

	/* Secondary display: process HDMI and DP interfaces */
	mode_count = process_display_interfaces(adev, index, connector, dc_crtc, phy);
	DRM_DEBUG("EDID processing: %lu modes found\n", mode_count);

	return mode_count;
}

int ls2k3000_dc_get_mdoes(struct loonggpu_bridge_phy *phy, int used_method, struct drm_connector *connector, struct edid *edid)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	int count = 0;

	switch (used_method) {
	case via_i2c:
	case via_encoder:
		if (phy->ddc_funcs && phy->ddc_funcs->get_modes) {
			count = phy->ddc_funcs->get_modes(phy, connector);
		} else {
			count = ls2k3000_dc_read_edid(adev, connector->index, connector, edid);
		}
		break;
	}

	return count;
}

static const struct bridge_phy_cfg_funcs ls2k3000_cfg_funcs = {
        .mode_valid = ls2k3000_mode_valid,
};

int internal_bridge_ls2k3000_register(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls2k3000_phy;
	int index = dc_bridge->display_pipe_index;
	struct connector_resource *connector_res =
			dc_bridge->adev->dc->link_info[index].connector;

	ls2k3000_phy = kzalloc(sizeof(*ls2k3000_phy), GFP_KERNEL);
	if (IS_ERR(ls2k3000_phy)) {
		DRM_ERROR("Failed to alloc loonggpu bridge phy!\n");
		return -1;
	}

	ls2k3000_phy->display_pipe_index = index;
	ls2k3000_phy->bridge.driver_private = ls2k3000_phy;
	ls2k3000_phy->adev = dc_bridge->adev;
	ls2k3000_phy->res = dc_bridge;
	ls2k3000_phy->li2c = dc_bridge->adev->i2c[index];
	ls2k3000_phy->connector_type = connector_res->type;

	ls2k3000_phy->cfg_funcs = &ls2k3000_cfg_funcs;
	ls2k3000_phy->hpd_funcs = &ls2k3000_hpd_funcs;

	/* TODO:
	 * Use VBIOS config */
	if (dc_bridge->display_pipe_index == 0)
		ls2k3000_phy->connector_type = DRM_MODE_CONNECTOR_DisplayPort;
	if (dc_bridge->display_pipe_index == 1)
		ls2k3000_phy->connector_type = DRM_MODE_CONNECTOR_HDMIA;

	dc_bridge->internal_bp = ls2k3000_phy;
	DRM_DEBUG_DRIVER("internal bridge phy register success!\n");

	return 0;
}

int bridge_phy_ls2k3000_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls2k3000_phy;

	ls2k3000_phy = bridge_phy_alloc(dc_bridge);

	return bridge_phy_init(ls2k3000_phy);
}

