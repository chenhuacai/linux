#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_vbios.h"
#include "lt8718_drv.h"

static bool lt8718_register_volatile(struct device *dev, unsigned int reg)
{
        switch (reg) {
        case LT8718_REG_PAGE_SELECT:
            return false;
        default:
            return true;
        }
}

static bool lt8718_register_readable(struct device *dev, unsigned int reg)
{
        switch (reg) {
        case 0x00:
            return false;
        case LT8718_REG_PAGE_SELECT:
        default:
            return true;
        }
}

static const struct regmap_range lt8718_rw_regs_range[] = {
        regmap_reg_range(LT8718_REG_START, LT8718_REG_END),
};

static const struct regmap_range lt8718_ro_regs_range[] = {
        regmap_reg_range(LT8718_REG_CHIP_VERSION_BASE,
            LT8718_REG_CHIP_VERSION_BASE + CHIP_VERSION_LEN),
};

static const struct regmap_range lt8718_vo_regs_range[] = {
        regmap_reg_range(LT8718_REG_START, LT8718_REG_END),
};

static const struct regmap_range_cfg lt8718_regmap_range_cfg[] = {
        {
        .name = "lt8718_registers",
        .range_min = LT8718_REG_START,
        .range_max = LT8718_REG_END,
        .window_start = LT8718_REG_START,
        .window_len = LT8718_REG_PAGE,
        .selector_reg = LT8718_REG_PAGE_SELECT,
        .selector_mask = 0xFF,
        .selector_shift = 0,
        },
};

static const struct regmap_access_table lt8718_read_table = {
        .yes_ranges = lt8718_rw_regs_range,
        .n_yes_ranges = ARRAY_SIZE(lt8718_rw_regs_range),
};

static const struct regmap_access_table lt8718_write_table = {
        .yes_ranges = lt8718_rw_regs_range,
        .n_yes_ranges = ARRAY_SIZE(lt8718_rw_regs_range),
        .no_ranges = lt8718_ro_regs_range,
        .n_no_ranges = ARRAY_SIZE(lt8718_ro_regs_range),
};

static const struct regmap_config lt8718_regmap_config = {
        .reg_bits = 8,
        .val_bits = 8,
        .reg_stride = 1,
        .max_register = LT8718_REG_END,
        .ranges = lt8718_regmap_range_cfg,
        .num_ranges = ARRAY_SIZE(lt8718_regmap_range_cfg),

        .fast_io = false,
        .cache_type = REGCACHE_RBTREE,

        .volatile_reg = lt8718_register_volatile,
        .readable_reg = lt8718_register_readable,
        .rd_table = &lt8718_read_table,
        .wr_table = &lt8718_write_table,
};

void dpcd_write_funtion(struct loonggpu_bridge_phy *phy, unsigned int  address, unsigned char data)
{
        unsigned char address_h = 0x0f & (address>>16);
        unsigned char address_m = 0xff & (address>>8);
        unsigned char address_l = 0xff &  address;

        regmap_write(phy->phy_regmap, 0x8033, address_l);
        regmap_write(phy->phy_regmap, 0x8034, address_m);
        regmap_write(phy->phy_regmap, 0x8035, address_h);
        regmap_write(phy->phy_regmap, 0x8036, 0x00);
        regmap_write(phy->phy_regmap, 0x8037, data);
        regmap_write(phy->phy_regmap, 0x8038, 0x00);
}

unsigned char dpcd_read_funtion(struct loonggpu_bridge_phy *phy, unsigned int address)
{
        unsigned char address_h = 0x0f & (address >> 16);
        unsigned char address_m = 0xff & (address >> 8);
        unsigned char address_l = 0xff &  address;
        unsigned int dpcd_value = 0x00;

        regmap_write(phy->phy_regmap, 0x8036, 0x80);
        regmap_write(phy->phy_regmap, 0x8033, address_l);
        regmap_write(phy->phy_regmap, 0x8034, address_m);
        regmap_write(phy->phy_regmap, 0x8035, address_h);
        regmap_write(phy->phy_regmap, 0x8038, 0x00);

        schedule_timeout(usecs_to_jiffies(300));
        regmap_read(phy->phy_regmap, 0x802d, &dpcd_value);

        return dpcd_value;
}

void link_configuration(struct loonggpu_bridge_phy *phy)
{
        regmap_write(phy->phy_regmap, 0x8002, 0x0a);
        regmap_write(phy->phy_regmap, 0x8003, 0xc0 | 0x02);
        regmap_write(phy->phy_regmap, 0x8004, 0x10);
        regmap_write(phy->phy_regmap, 0x8005, 0x00);
        dpcd_write_funtion(phy, 0x00600, 0x02); /* 00600h : set power down mode */
        dpcd_write_funtion(phy, 0x00600, 0x01); /* 00600h : set normal power mode */

        dpcd_write_funtion(phy, 0x00100, 0x0a);

        dpcd_write_funtion(phy, 0x00101, 0x80 | 0x02); /* 4 lanes; Enhanced       1, 2, 4 */
        dpcd_write_funtion(phy, 0x00107, 0x00);	/* 10:ssc enable; 00:ssc disable */
        dpcd_write_funtion(phy, 0x00108, 0x01);	/* 8B_10B Encode */
}

void drive_write_funtion(struct loonggpu_bridge_phy *phy, unsigned char data)
{
        regmap_write(phy->phy_regmap, 0x8036, 0x03);
        regmap_write(phy->phy_regmap, 0x8033, 0x03);
        regmap_write(phy->phy_regmap, 0x8034, 0x01);
        regmap_write(phy->phy_regmap, 0x8035, 0x00);
        regmap_write(phy->phy_regmap, 0x8037, data);
        regmap_write(phy->phy_regmap, 0x8037, data);
        regmap_write(phy->phy_regmap, 0x8037, data);
        regmap_write(phy->phy_regmap, 0x8037, data);
        regmap_write(phy->phy_regmap, 0x8038, 0x00);
}

int  tps_status(struct loonggpu_bridge_phy *phy, unsigned char tps)
{
        unsigned char lane0_1status = 0x00;
        unsigned char lane2_3status = 0x00;

        lane0_1status = dpcd_read_funtion(phy, 0x00202);
        lane2_3status = dpcd_read_funtion(phy, 0x00203);

        if (tps == TPS1) {
            if (lane0_1status == 0x11) {
                return 1;
            }
        }

        if (tps == TPS2) {
            if (lane0_1status == 0x77) {
                return 1;
            }
        }

        return 0;
}

unsigned char read_adjust_request(struct loonggpu_bridge_phy *phy, unsigned char tps)
{
        unsigned char lane0_1request = 0x00;
        unsigned char lane2_3request = 0x00;
        int i;

        loop_counter++;
        lane0_1request = dpcd_read_funtion(phy, 0x00206);
        lane2_3request = dpcd_read_funtion(phy, 0x00207);

        if (tps1_count > 10) {
            return 0x05;
        }

        if ((tps == TPS2) && (loop_counter == 5)) {
            return MAXLOOP;
        }

        if ((lane0_1request&0x03) == 0x03) {
            return MAXSWING;
        }

        if (iteration_counter == 5) {
            return MAXCOUNTER;
        }

        switch (lane0_1request) {
        case 0x00:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x00:0x36);
            }
            drive_write_funtion(phy,(SWINGLEVEL0|PRE_EMPHASIS_LEVEL0));
            iteration_counter++;
            break;
        case 0x44:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x11:0x44);
            }
            drive_write_funtion(phy,(SWINGLEVEL0|PRE_EMPHASIS_LEVEL1));
            break;
        case 0x88:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x22:0x51);
            }
            drive_write_funtion(phy,(SWINGLEVEL0|PRE_EMPHASIS_LEVEL2));
            break;
        case 0xcc:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x44:0x6c);
            }
            drive_write_funtion(phy,(SWINGLEVEL0|PRE_EMPHASIS_LEVEL3));
            break;
        case 0x11:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x00:0x51);
            }
            drive_write_funtion(phy,(SWINGLEVEL1|PRE_EMPHASIS_LEVEL0));
            break;
        case 0x55:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x19:0x66);
            }
            drive_write_funtion(phy,(SWINGLEVEL1|PRE_EMPHASIS_LEVEL1));
            break;
        case 0x99:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x33:0x7a);
            }
            drive_write_funtion(phy,(SWINGLEVEL1|PRE_EMPHASIS_LEVEL2));
            break;
        case 0x22:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x00:0x6c);
            }
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL0));
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL0));
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL0));
            break;
        case 0x66:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x22:0x88);
            }
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL1));
            break;
        case 0xaa:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x6a:0xff);
            }
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL2));
            break;
        case 0x33:
            for (i = 0; i < 8; i++) {
                regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x00:0xa3);
            }
            drive_write_funtion(phy,(SWINGLEVEL2|PRE_EMPHASIS_LEVEL3));
            break;
        default:
            break;
        }

        return 0;
}

void vd_dp_tx_swing_init(struct loonggpu_bridge_phy *phy,unsigned char swing)
{
        int i = 0;

        regmap_write(phy->phy_regmap, 0x7021, 0x0d);
        regmap_write(phy->phy_regmap, 0x7021, 0x0f);
        regmap_write(phy->phy_regmap, 0x7022, 0x77);
        regmap_write(phy->phy_regmap, 0x7023, 0x77);
        for (i = 0; i < 8; i++)
            regmap_write(phy->phy_regmap, 0x7024 + i, i % 2 ? 0x00:swing);
}

void end_training(struct loonggpu_bridge_phy *phy)
{
        dpcd_write_funtion(phy, 0x00102, 0x00);
        schedule_timeout(msecs_to_jiffies(100));
        regmap_write(phy->phy_regmap, 0x8414, 0x80);
        regmap_write(phy->phy_regmap, 0x8414, 0xc0);
}

static int lt8718_aux_traing(struct loonggpu_bridge_phy *phy)
{
        int num;
        tps1_count = 0;
        iteration_counter = 0;

        regmap_write(phy->phy_regmap, 0x6007, 0xc7);

        link_configuration(phy);
        regmap_write(phy->phy_regmap, 0x601f, 0xf0);
        regmap_write(phy->phy_regmap, 0x601f, 0xff);
        regmap_write(phy->phy_regmap, 0x8414, 0x81);

        dpcd_write_funtion(phy, 0x00102, 0x21);
        drive_write_funtion(phy, 0x00);
        schedule_timeout(usecs_to_jiffies(400));

        while (!tps_status(phy, TPS1)) {
            if (read_adjust_request(phy, TPS1) != 0x00 ) {
                break;
            }
            num++;
            tps1_count++;
            schedule_timeout(usecs_to_jiffies(400));
            if (num == 5) {
                return -1;
            }
        }

        num = 0;
        iteration_counter = 0;
        loop_counter = 0;
        regmap_write(phy->phy_regmap, 0x8003, 0x42);
        regmap_write(phy->phy_regmap, 0x8414, 0x84);
        dpcd_write_funtion(phy, 0x00102, 0x22);
        schedule_timeout(usecs_to_jiffies(100));

        while (!tps_status(phy, TPS2)) {
            if (read_adjust_request(phy, TPS2)!=0x00) {
                break;
            }
            num++;
            if (num == 5) {
                return -1;
            }
            schedule_timeout(usecs_to_jiffies(400));
        }

        vd_dp_tx_swing_init(phy, 0x80);
        regmap_write(phy->phy_regmap,0x6007,0xff);
        schedule_timeout(msecs_to_jiffies(100));
        end_training(phy);

        return 0;
}

void write_funtion(struct loonggpu_bridge_phy *phy, unsigned char number, unsigned char *data)
{
        unsigned char i;
        unsigned int val;

Again:
        regmap_write(phy->phy_regmap, 0x802a, 0x01);
        for (i = 0; i < number; i++) {
            regmap_write(phy->phy_regmap, 0x802b, data[i]);
        }

        regmap_write(phy->phy_regmap, 0x802c, 0x00);
        schedule_timeout(usecs_to_jiffies(400));

        if (regmap_read(phy->phy_regmap, 0x802b, &val) != 0x00) {
            goto Again;
        }
}

void lt8718_edid_read(struct loonggpu_bridge_phy *phy)
{
        /* 4bit command 1/4/5(MOT set|0 read/) + 20bit address 00050 + 8bit length 00/03/0f + data */
        unsigned char  AUX_EDID_ADDR[3] = {0x40, 0x00, 0x50};   /* 0100 write with addr 0x50   MOT 0101 = 5 read, 0100 = 4 write   0001 = 1 read */
        unsigned char  AUX_W_I2C_OFFSET[5] = {0x40, 0x00, 0x50, 0x00, I2C_OFFSET};   /* write  with addr 0x50 and one byte date 0x00 (EDID offset) request */
        unsigned char  AUX_R_EDID[4] = {0x50, 0x00, 0x50, 0x0f};         /* length should be (LEN7:0 + 1),here 0x0f means 16 bytes */
        unsigned char  AUX_I2C_STOP[3] = {0x10, 0x00, 0x50};            /* when read data done, clr MOT bit */
        unsigned char i = 0;
        unsigned char j = 0;
        unsigned int val, k;

        /* AUX cmd should be 1->2->3->x->5   write ->read ->read done */
        write_funtion(phy, sizeof(AUX_EDID_ADDR), AUX_EDID_ADDR);
        write_funtion(phy, sizeof(AUX_W_I2C_OFFSET), AUX_W_I2C_OFFSET);
        for (j = 0; j < 16; j++) {
            write_funtion(phy, sizeof(AUX_R_EDID), AUX_R_EDID);
            schedule_timeout(msecs_to_jiffies(10));
            for (i = 0; i < 16; i++) {
                regmap_read(phy->phy_regmap, 0x802b, &val);
                lt8718_edid[16*j+i] = val;
            }
        }
        write_funtion(phy, sizeof(AUX_I2C_STOP), AUX_I2C_STOP);
        regmap_write(phy->phy_regmap, 0x802a, 0x00);
        regmap_write(phy->phy_regmap, 0x8028, 0x05);
        regmap_write(phy->phy_regmap, 0x8028, 0x01);

        for (k = 0; k < 256; k++)
            regmap_write(phy->phy_regmap, 0x8029, lt8718_edid[k]);
        regmap_write(phy->phy_regmap, 0x8028, 0x02); /* Read EDID shadow Enable */
}

static void hdmi_format(struct loonggpu_bridge_phy *phy)
{
        unsigned int lane_byte_h = 0x0000;
        unsigned int lane_byte_l = 0x0000;
        unsigned int lane_byte = 0x0000;

        regmap_read(phy->phy_regmap, 0x8833, &lane_byte_h);
        regmap_read(phy->phy_regmap, 0x8834, &lane_byte_l);

        regmap_write(phy->phy_regmap, 0x881a, 0x30);
        regmap_write(phy->phy_regmap, 0x884b, 0xe0);

        lane_byte = lane_byte_h;
        lane_byte <<= 8;
        lane_byte |= lane_byte_l;
        lane_byte = lane_byte * 3 / 2;
        regmap_write(phy->phy_regmap, 0x881b, lane_byte >> 8);
        regmap_write(phy->phy_regmap, 0x881c, lane_byte & 0xff);

        regmap_write(phy->phy_regmap, 0x8817, 0x08);	/* Format: RGB; Depth: 8bit */
        regmap_write(phy->phy_regmap, 0x8818, 0x20);	/* MISC0 Value: 8bit */
        regmap_write(phy->phy_regmap, 0x881d, 0x36);	/* Pixels_Onetime; Pixels_Holdtime before：0x59 */
        regmap_write(phy->phy_regmap, 0x881f, 0x4e);	/* FIFO_Empty[2:0]; FIFO_Full[4:0] */
        regmap_write(phy->phy_regmap, 0x8820, 0x66);	/* Out_HighByte; Out_LowByte */
}

void dp_out_video_open(struct loonggpu_bridge_phy *phy)
{
        hdmi_format(phy);
        regmap_write(phy->phy_regmap, 0x881e, 0x30);
}

static int lt8718_bl_ctrl(struct loonggpu_bridge_phy *phy, int mode)
{
        if (mode == DRM_MODE_DPMS_ON) {
            regmap_write(phy->phy_regmap, 0x7044, 0x0c);
            regmap_write(phy->phy_regmap, 0x80d1, 0x0c);
            regmap_write(phy->phy_regmap, 0x80d3, 0x00);
            regmap_write(phy->phy_regmap, 0x80d3, 0x30);
            regmap_write(phy->phy_regmap, 0x80c8, 0x20);
            regmap_write(phy->phy_regmap, 0x80c9, 0x00);
            regmap_write(phy->phy_regmap, 0x80ca, 0x27);
            regmap_write(phy->phy_regmap, 0x80cb, 0x10);
            regmap_write(phy->phy_regmap, 0x80cc, 0x00);
            regmap_write(phy->phy_regmap, 0x80cd, 0x27);
            regmap_write(phy->phy_regmap, 0x80ce, 0x10);
        } else {
            regmap_write(phy->phy_regmap, 0x80d1, 0x04);
            regmap_write(phy->phy_regmap, 0x80cc, 0x00);
            regmap_write(phy->phy_regmap, 0x80cd, 0x00);
            regmap_write(phy->phy_regmap, 0x80cd, 0x00);
            regmap_write(phy->phy_regmap, 0x80ce, 0x00);
        }

        return 0;
}

static enum hpd_status lt8718_get_hpd_status(struct loonggpu_bridge_phy *phy)
{
        struct lt8718_device *lt8718_dev = phy->priv;
        unsigned int val = 0;

        regmap_read(phy->phy_regmap, LT8718_REG_LINK_STATUS, &val);
        if ((val & LINK_STATUS_STABLE) == LINK_STATUS_STABLE) {
            schedule_timeout(msecs_to_jiffies(30));
            regmap_read(phy->phy_regmap, LT8718_REG_LINK_STATUS, &val);
            if ((val & LINK_STATUS_STABLE) == LINK_STATUS_STABLE) {
                if (lt8718_dev->status) {
                    lt8718_dev->status = 0;
                    lt8718_aux_traing(phy);
                }
                return hpd_status_plug_on;
            }
        }

        lt8718_dev->status = 1;

        return hpd_status_plug_off;
}

static enum drm_connector_status lt8718_get_connect_status(struct loonggpu_bridge_phy *phy)
{
	return lt8718_get_hpd_status(phy)
				? connector_status_connected
				: connector_status_disconnected;
}

static struct bridge_phy_hpd_funcs lt8718_hpd_funcs = {
        .get_hpd_status = lt8718_get_hpd_status,
        .get_connect_status = lt8718_get_connect_status,
};

static int lt8718_get_modes(struct loonggpu_bridge_phy *phy,
	                     struct drm_connector *connector)
{
        struct edid *edid;
        int size = sizeof(unsigned char) * EDID_LENGTH * 2;
        unsigned int count = 0;

        edid = kmalloc(size, GFP_KERNEL);
        if (edid) {
            lt8718_edid_read(phy);
            memcpy(edid, lt8718_edid, size);
            drm_connector_update_edid_property(connector, edid);
            count = drm_add_edid_modes(connector, edid);
            kfree(edid);
        } else {
            DRM_ERROR("[LT8718] resources is invalid.\n");
        }

        return count;
}

static bool lt8718_chip_id_verify(struct loonggpu_bridge_phy *phy, char *str)
{
        struct lt8718_device *lt8718_dev;
        unsigned char version_val[2];

        regmap_bulk_read(phy->phy_regmap, LT8718_REG_CHIP_VERSION_BASE,
            version_val, CHIP_VERSION_LEN);

        if (version_val[0] != 0x16) {
            DRM_ERROR("Invalid lt8718 chip version {%02x}\n", version_val[0]);
            strcpy(str, "Unknown");
            return false;
        }

        phy->chip_version = version_val[1];
	lg_strncpy(str, version_val, ARRAY_SIZE(version_val));
        lt8718_dev = phy->priv;
        if (version_val[1] == 0x06)
            lt8718_dev->ver = LT8718_VER_1;
        else
            lt8718_dev->ver = LT8718_VER_Unknown;

        DRM_INFO("Get chip version: LT8718_VER_U%d\n", lt8718_dev->ver);

        return true;
}

static int lt8718_mode_set(struct loonggpu_bridge_phy *phy,
		const struct drm_display_mode *mode,
		const struct drm_display_mode *adj_mode)
{
        if (!phy)
            return 1;

        lt8718_aux_traing(phy);
        dp_out_video_open(phy);

	return 0;
}

static struct bridge_phy_misc_funcs lt8718_misc_funcs = {
        .chip_id_verify = lt8718_chip_id_verify,
};

static struct bridge_phy_ddc_funcs lt8718_ddc_funcs = {
        .get_modes = lt8718_get_modes,
};

static int lt8718_rx_phy(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_rx_phy_cfg, ARRAY_SIZE(lt8718_rx_phy_cfg));
        return 0;
}

static int lt8718_rx_pll(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_rx_pll_cfg, ARRAY_SIZE(lt8718_rx_pll_cfg));
        return 0;
}

static int lt8718_audio_iis(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_audio_iis_cfg, ARRAY_SIZE(lt8718_audio_iis_cfg));
        return 0;
}

static int lt8718_video_input(struct loonggpu_bridge_phy *phy)
{
        lt8718_rx_phy(phy);
        lt8718_rx_pll(phy);
        lt8718_audio_iis(phy);
        regmap_write(phy->phy_regmap, 0x7048, 0x00);
        return 0;
}

static int lt8718_tx_phy(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_tx_phy_cfg, ARRAY_SIZE(lt8718_tx_phy_cfg));
        return 0;
}

static int lt8718_tx_pll(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_tx_pll_cfg, ARRAY_SIZE(lt8718_tx_pll_cfg));
        return 0;
}

static int lt8718_video_processor(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_video_processor_cfg, ARRAY_SIZE(lt8718_video_processor_cfg));
        return 0;
}

static int lt8718_output_set(struct loonggpu_bridge_phy *phy)
{
        regmap_multi_reg_write(phy->phy_regmap, lt8718_output_set_cfg, ARRAY_SIZE(lt8718_output_set_cfg));
        return 0;
}

static int lt8718_video_output(struct loonggpu_bridge_phy *phy)
{
        lt8718_tx_phy(phy);
        lt8718_tx_pll(phy);
        lt8718_video_processor(phy);
        regmap_write(phy->phy_regmap, 0x8400, 0x00);
        regmap_write(phy->phy_regmap, 0x8415, 0x8d);
        regmap_write(phy->phy_regmap, 0x8416, 0xcf);
        regmap_write(phy->phy_regmap, 0x7021, 0x0d);
        regmap_write(phy->phy_regmap, 0x7021, 0x0f);
        lt8718_aux_traing(phy);
        lt8718_output_set(phy);
        schedule_timeout(msecs_to_jiffies(500));

        return 0;
}

static enum drm_mode_status lt8718_mode_valid(struct drm_connector *connector,
					      struct drm_display_mode *mode)
{
        if (mode->hdisplay == 1680 && mode->vdisplay == 1050)
            return MODE_BAD;
        if (mode->hdisplay == 1680 && mode->vdisplay == 945)
            return MODE_BAD;
        if (mode->hdisplay == 1440 && mode->vdisplay == 1050)
            return MODE_BAD;
        if (mode->hdisplay == 1400 && mode->vdisplay == 1050)
            return MODE_BAD;
        if (mode->hdisplay == 1360 && mode->vdisplay == 768)
            return MODE_BAD;

	return MODE_OK;
}

static const struct bridge_phy_cfg_funcs lt8718_cfg_funcs = {
        .mode_set = lt8718_mode_set,
        .mode_valid = lt8718_mode_valid,
        .backlight_ctrl = lt8718_bl_ctrl,
        .video_input_cfg = lt8718_video_input,
        .video_output_cfg = lt8718_video_output,
};

int bridge_phy_lt8718_init(struct loonggpu_dc_bridge *dc_bridge)
{
        struct loonggpu_bridge_phy *lt8718_phy;
        struct lt8718_device *lt8718_dev;

        lt8718_phy = bridge_phy_alloc(dc_bridge);

        lt8718_dev = kmalloc(sizeof(struct lt8718_device), GFP_KERNEL);
        if (IS_ERR(lt8718_dev))
            return PTR_ERR(lt8718_dev);

	lt8718_phy->is_ext_encoder = true;
        lt8718_dev->status = 1;
        lt8718_phy->priv = lt8718_dev;
	lt8718_phy->cfg_funcs = &lt8718_cfg_funcs;
	lt8718_phy->regmap_cfg = &lt8718_regmap_config;
	lt8718_phy->misc_funcs = &lt8718_misc_funcs;
	lt8718_phy->ddc_funcs = &lt8718_ddc_funcs;
	lt8718_phy->hpd_funcs = &lt8718_hpd_funcs;

	return bridge_phy_init(lt8718_phy);
}

