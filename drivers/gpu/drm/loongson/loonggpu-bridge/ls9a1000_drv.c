#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_dp.h"
#include "bridge_phy.h"

#define I2C_CR1			0x0
#define I2C_CR2			0x4
#define I2C_OAR			0x8
#define I2C_DR				0x10
#define I2C_SR1			0x14
#define I2C_SR2			0x18
#define I2C_CCR			0x1c
#define I2C_TRISE			0x20

/* CR1 register bits */
#define CR1_PE				BIT(0)
#define CR1_START			BIT(8)
#define CR1_STOP			BIT(9)
#define CR1_SWRST			BIT(15)
#define CR1_ACK			BIT(10)
#define CR1_POS			BIT(11)

/* SR1 register bits */
#define SR1_SB				BIT(0)
#define SR1_ADDR			BIT(1)
#define SR1_BTF			BIT(2)
#define SR1_STOPF			BIT(4)
#define SR1_RXNE			BIT(6)
#define SR1_TXE			BIT(7)
#define SR1_BERR			BIT(8)
#define SR1_ARLO			BIT(9)
#define SR1_AF				BIT(10)
#define SR1_OVR			BIT(11)

/* SR2 register bits */
#define SR2_MSL			BIT(0)
#define SR2_BUSY			BIT(1)
#define SR2_TRA			BIT(2)

#define DCC_ADDR			0xa0
#define EDID_LENGTH			128

/*
 * Maximum loop iterations for register polling before
 * declaring a timeout (empirical value).
 */
#define I2C_TIMEOUT		100000

#define EIO			5       /* I/O error */
#define EINVAL			22      /* Invalid argument */
#define ETIMEDOUT		110     /* Connection timed out */

#define true	1
#define false	0

/**
 * wait_on_bit_hdmi() - poll a register until a bit is set or cleared
 * @reg:     register address
 * @mask:    bitmask to test
 * @set:     true to wait for bit set, false for bit cleared
 *
 * Return: 0 on success, -ETIMEDOUT if bit state not reached.
 */
static int wait_on_bit_hdmi(struct loonggpu_dc_i2c *dc_i2c, unsigned int reg, unsigned int mask, unsigned char set)
{
	int timeout = I2C_TIMEOUT;

	while (timeout--) {
		if ((readl(dc_i2c->reg_base + reg) & mask) == (set ? mask : 0))
			return 0;
	}

	return -ETIMEDOUT;
}

/**
 * poll_errors() - check I2C SR1 error flags
 *
 * Prints an error message and issues STOP if any error is detected.
 * Return: non-zero if an error occurred.
 */
static int poll_errors(struct loonggpu_dc_i2c *dc_i2c)
{
	int err = 0;
	unsigned int sr1 = readl(dc_i2c->reg_base + I2C_SR1);

	if (sr1 & SR1_BERR) {
		DRM_INFO("I2C: bus error\n");
		err = 1;
	}
	if (sr1 & SR1_ARLO) {
		DRM_INFO("I2C: arbitration lost\n");
		err = 1;
	}
	if (sr1 & SR1_AF) {
		DRM_INFO("I2C: ack failure\n");
		err = 1;
	}
	if (sr1 & SR1_OVR) {
		DRM_INFO("I2C: overrun/underrun\n");
		err = 1;
	}

	if (err)
		writel(CR1_PE | CR1_STOP, dc_i2c->reg_base + I2C_CR1);

	return err;
}

/**
 * wait_and_check() - wait for a status bit and then check errors
 * @mask: bitmask to wait for
 * @set:  true to wait for set, false for cleared
 *
 * Return: 0 on success, negative errno on failure.
 */
static int wait_and_check(struct loonggpu_dc_i2c *dc_i2c, unsigned int mask, unsigned char set)
{
	int ret;

	ret = wait_on_bit_hdmi(dc_i2c, I2C_SR1, mask, set);
	if (ret)
		return ret;

	if (poll_errors(dc_i2c))
		return -EIO;

	return 0;
}

/**
 * i2c_start() - generate I2C start condition
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_start(struct loonggpu_dc_i2c *dc_i2c)
{
	writel(CR1_PE | CR1_START, dc_i2c->reg_base + I2C_CR1);

	return wait_and_check(dc_i2c, SR1_SB, true);
}

/**
 * i2c_stop() - generate I2C stop condition
 */
static void i2c_stop(struct loonggpu_dc_i2c *dc_i2c)
{
	writel(CR1_PE | CR1_STOP, dc_i2c->reg_base + I2C_CR1);
	wait_on_bit_hdmi(dc_i2c, I2C_CR1, CR1_STOP, false);
}

/**
 * i2c_send_addr() - send slave address with R/W bit
 * @dev_addr_rw: 7-bit address shifted left by 1, LSB is R/W
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_send_addr(struct loonggpu_dc_i2c *dc_i2c, unsigned char dev_addr_rw)
{
	writel(dev_addr_rw, dc_i2c->reg_base + I2C_DR);
	if (wait_and_check(dc_i2c, SR1_ADDR, true))
		return -EIO;

	/* Clear ADDR flag by reading SR2 */
	readl(dc_i2c->reg_base + I2C_SR2);

	return 0;
}

/**
 * i2c_write_byte() - write one data byte to the bus
 * @data: byte to transmit
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_write_byte(struct loonggpu_dc_i2c *dc_i2c, unsigned char data)
{
	if (wait_and_check(dc_i2c, SR1_TXE, true))
		return -EIO;

	writel(data, dc_i2c->reg_base + I2C_DR);

	return wait_and_check(dc_i2c, SR1_BTF, true);
}

/**
 * i2c_read_last_byte() - receive the final byte with NACK + STOP
 * @data: output buffer for the received byte
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_read_last_byte(struct loonggpu_dc_i2c *dc_i2c, unsigned char *data)
{
	if (wait_and_check(dc_i2c, SR1_RXNE, true))
		return -EIO;

	writel(CR1_PE | CR1_STOP, dc_i2c->reg_base + I2C_CR1);
	*data = readl(dc_i2c->reg_base + I2C_DR);

	while (readl(dc_i2c->reg_base + I2C_CR1) & CR1_STOP);

	return 0;
}

/**
 * i2c_read() - read one byte from DDC EEPROM at given register offset
 * @reg_addr: byte-address within the EEPROM
 *
 * Return: the read value (0-255) on success, negative errno on failure.
 */
static int i2c_read(struct loonggpu_dc_i2c *dc_i2c, unsigned int reg_addr)
{
	unsigned char val;
	int ret;

	ret = i2c_start(dc_i2c);
	if (ret)
		goto fail;

	ret = i2c_send_addr(dc_i2c, DCC_ADDR);
	if (ret)
		goto fail;

	ret = i2c_write_byte(dc_i2c, (unsigned char)reg_addr);
	if (ret)
		goto fail;

	/* Repeated start for read phase */
	ret = i2c_start(dc_i2c);
	if (ret)
		goto fail;

	ret = i2c_send_addr(dc_i2c, DCC_ADDR | 0x01);
	if (ret)
		goto fail;

	ret = i2c_read_last_byte(dc_i2c, &val);
	if (ret)
		goto fail;

	return val;

fail:
	i2c_stop(dc_i2c);
	return ret;
}

/**
 * i2c_init() - initialise the I2C controller
 *
 * Configure clock, rise time, perform soft reset, and enable peripheral.
 */
static void i2c_init(struct loonggpu_dc_i2c *dc_i2c)
{
	int ccr = dc_i2c->adev->clock.default_sclk / 4 / 10;

	writel(ccr, dc_i2c->reg_base + I2C_CCR);
	writel(161, dc_i2c->reg_base + I2C_TRISE);

	writel(CR1_SWRST, dc_i2c->reg_base + I2C_CR1);
	writel(0, dc_i2c->reg_base + I2C_CR1);

	writel(160, dc_i2c->reg_base + I2C_CR2);

	writel(CR1_PE, dc_i2c->reg_base + I2C_CR1);
}

void ls9a_i2c_read(struct loonggpu_dc_i2c *dc_i2c, uint8_t dev_addr,
		   uint8_t reg_addr, uint8_t *val)
{
	uint8_t addr = dev_addr << 1;

	i2c_init(dc_i2c);

	i2c_start(dc_i2c);
	i2c_send_addr(dc_i2c, addr);
	i2c_write_byte(dc_i2c, reg_addr);
	i2c_start(dc_i2c);
	i2c_send_addr(dc_i2c, addr + 1);
	i2c_read_last_byte(dc_i2c, val);
	i2c_stop(dc_i2c);
}

void ls9a_i2c_write(struct loonggpu_dc_i2c *dc_i2c, uint8_t dev_addr,
		    uint8_t reg_addr, uint8_t value)
{
	uint8_t addr = dev_addr << 1;

	i2c_init(dc_i2c);

	i2c_start(dc_i2c);
	i2c_send_addr(dc_i2c, addr);
	i2c_write_byte(dc_i2c, reg_addr);
	i2c_write_byte(dc_i2c, value);
	i2c_stop(dc_i2c);
}

/**
 * read_edid() - read and validate the full 128-byte EDID block from EEPROM
 */
static int read_edid(unsigned char *edid, struct loonggpu_dc_i2c *dc_i2c)
{
	int ret;
	int i;

	i2c_init(dc_i2c);

	for (i = 0; i < (EDID_LENGTH * 2); i++) {
		ret = i2c_read(dc_i2c, i);
		if (ret < 0) {
			DRM_INFO("EDID: read error at byte %d\n", i);
			return ret;
		}
		edid[i] = (unsigned char)ret;
	}

	return 0;
}

static void ls9a1000_hpd_init(struct loonggpu_bridge_phy *phy, struct loonggpu_connector* lconnector, bool has_ext_encoder)
{
	struct loonggpu_device *adev = phy->adev;
	struct connector_resource *connector_res = adev->dc->link_info[lconnector->connector_id].connector;

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

static enum drm_connector_status ls9a1000_get_connect_status(struct loonggpu_bridge_phy *phy)
{
	struct drm_connector *connector = phy->connector;
	struct loonggpu_device *adev = phy->adev;
	struct loonggpu_dc_crtc *crtc = adev->dc->link_info[phy->connector->index].crtc;
	enum drm_connector_status status = connector_status_disconnected;
	struct loonggpu_dc_video *dc_video = &crtc->dc_video;
	int i;
	int reg_val;

	if (connector->polled == 0) {
		status = connector_status_connected;
		for (i = 0; i < crtc->interfaces; i++)
			crtc->intf[i].connected = true;
	} else {
		for (i = 0; i < crtc->interfaces; i++) {
			crtc->intf[i].connected = false;
			switch (crtc->intf[i].type) {
			case INTERFACE_HDMI:
				reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[dc_video->hdmi_num].irq_hpd);
				if (reg_val & 0x1) {
					status = connector_status_connected;
					crtc->intf[i].connected = true;
				}
				break;
			case INTERFACE_DP:
				reg_val = dc_readl(adev, gdc_reg->dp_reg[dc_video->dp_num].dp_hpd);
				if (reg_val & 0x1) {
					status = connector_status_connected;
					crtc->intf[i].connected = true;
				}
				break;
			case INTERFACE_VGA:
				if (dc_video->vga_connected) {
					status = connector_status_connected;
					crtc->intf[i].connected = true;
				}
				break;
			case INTERFACE_DVO:
				reg_val = dc_readl(adev, gdc_reg->dvo_reg.dvo_int_status);
				if (reg_val & 0x2) {
					status = connector_status_connected;
					crtc->intf[i].connected = true;
				}
				break;
			}
		}
	}

	if (status == connector_status_disconnected) {
		memset(dc_video->raw_edid, 0, sizeof(dc_video->raw_edid));
		dc_video->edid_length = 0;
	}

	return status;
}

static struct bridge_phy_hpd_funcs ls9a1000_hpd_funcs = {
	.hpd_init = ls9a1000_hpd_init,
	.get_connect_status = ls9a1000_get_connect_status,
};

static int ls9a1000_get_modes(struct loonggpu_bridge_phy *phy,
			      struct drm_connector *connector)
{
	struct loonggpu_device *adev = connector->dev->dev_private;
	struct loonggpu_dc_crtc *dc_crtc = adev->dc->link_info[connector->index].crtc;
	struct loonggpu_dc_video *dc_video = &dc_crtc->dc_video;
	unsigned int count = 0;
	int ret;
	struct edid *edid;

	if (dc_video->edid_length) {
		drm_connector_update_edid_property(connector,
				(struct edid *) dc_video->raw_edid);
		return drm_add_edid_modes(connector,
				(struct edid *) dc_video->raw_edid);
	}

	edid = kzalloc(EDID_LENGTH * 3, GFP_KERNEL);
	if (!edid)
		return 0;

	switch (dc_crtc->intf[0].type) {
	case INTERFACE_HDMI:
		ret = read_edid((unsigned char *)edid,
				adev->i2c[dc_video->hdmi_num]);
		if (ret) {
			DRM_INFO("9A HDMI:%d edid error\n", dc_video->hdmi_num);
			return 0;
		}
		break;
	case INTERFACE_DP:
		ret = ls9a_dp_read_edid(adev, dc_crtc, (unsigned char *)edid);
		if (!ret) {
			DRM_INFO("9A DP:%d edid error\n", dc_video->dp_num);
			return 0;
		}
		break;
	}

	dc_video->edid_length = EDID_LENGTH * (edid->extensions + 1);
	memmove(dc_video->raw_edid, (uint8_t *)edid, dc_video->edid_length);
	drm_connector_update_edid_property(connector,
			(struct edid *) dc_video->raw_edid);
	count = drm_add_edid_modes(connector,
			(struct edid *) dc_video->raw_edid);
	dc_crtc->dc_video.scdc_scrambling_supported =
			connector->display_info.hdmi.scdc.scrambling.supported;
	kfree(edid);

	return count;
}

static struct bridge_phy_ddc_funcs ls9a1000_ddc_funcs = {
	.get_modes = ls9a1000_get_modes,
};

static enum drm_mode_status ls9a1000_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
	if (mode->hdisplay > 4096 || mode->vdisplay > 4096)
		return MODE_BAD;

	if (mode->clock > 600000)
		return MODE_CLOCK_HIGH;

	if (mode->vdisplay < 480)
		return MODE_BAD;

	return MODE_OK;
}

static const struct bridge_phy_cfg_funcs ls9a1000_cfg_funcs = {
	.mode_valid = ls9a1000_mode_valid,
};

int internal_bridge_ls9a1000_register(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls9a1000_phy;
	int index = dc_bridge->display_pipe_index;
	struct connector_resource *connector_res =
			dc_bridge->adev->dc->link_info[index].connector;

	ls9a1000_phy = kzalloc(sizeof(*ls9a1000_phy), GFP_KERNEL);
	if (IS_ERR(ls9a1000_phy)) {
		DRM_ERROR("Failed to alloc loonggpu bridge phy!\n");
		return -1;
	}

	ls9a1000_phy->display_pipe_index = index;
	ls9a1000_phy->bridge.driver_private = ls9a1000_phy;
	ls9a1000_phy->adev = dc_bridge->adev;
	ls9a1000_phy->res = dc_bridge;
	ls9a1000_phy->li2c = dc_bridge->adev->i2c[index];
	ls9a1000_phy->connector_type = connector_res->type;

	ls9a1000_phy->cfg_funcs = &ls9a1000_cfg_funcs;
	ls9a1000_phy->hpd_funcs = &ls9a1000_hpd_funcs;
	ls9a1000_phy->ddc_funcs = &ls9a1000_ddc_funcs;

	dc_bridge->internal_bp = ls9a1000_phy;
	DRM_DEBUG_DRIVER("internal bridge phy register success!\n");

	return 0;
}

int bridge_phy_ls9a1000_init(struct loonggpu_dc_bridge *dc_bridge)
{
	struct loonggpu_bridge_phy *ls9a1000_phy;

	ls9a1000_phy = bridge_phy_alloc(dc_bridge);

	return bridge_phy_init(ls9a1000_phy);
}
