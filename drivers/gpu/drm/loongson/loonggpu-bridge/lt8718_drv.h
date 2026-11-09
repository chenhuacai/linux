#ifndef __LT8718_H__
#define __LT8718_H__

#include "bridge_phy.h"

#define LT8718_CHIP_NAME    		"LT8718"
#define LT8718_CHIP_ADDR    		0x66

#define LT8718_REG_START    		0x0000
#define LT8718_REG_END      		0x90FE
#define LT8718_REG_PAGE     		0x100
#define LT8718_REG_PAGE_SELECT  	0xFF
#define LT8718_REG_LINK_STATUS  	0x80B0
#define LINK_STATUS_OUTPUT_DC_POS 	4U
#define LINK_STATUS_STABLE			4U

/* General Registers */
#define LT8718_REG_CHIP_VERSION_BASE	0x6000
#define CHIP_VERSION_LEN			2U
#define LT8718_REG_CHIP_VERSION(x)	(LT8718_REG_CHIP_VERSION_BASE + (x))
#define SWINGLEVEL0					0x00
#define SWINGLEVEL1					0x01
#define SWINGLEVEL2					0x02
#define SWINGLEVEL3					0x07
#define	PRE_EMPHASIS_LEVEL0			0x00
#define PRE_EMPHASIS_LEVEL1			0x08
#define PRE_EMPHASIS_LEVEL2			0x10
#define	PRE_EMPHASIS_LEVEL3			0x38

#define TPS1						0x01
#define	TPS2						0x02
#define MAXSWING					0x01
#define MAXCOUNTER					0x02
#define MAXLOOP						0x03

#define I2C_OFFSET					0x00
#define EDID_HEADER_LEN				0x08

static unsigned char loop_counter = 0;
static unsigned char iteration_counter = 0;
static unsigned char tps1_count = 0;
static unsigned char lt8718_edid[256];

enum lt8718_chip_version {
		LT8718_VER_Unknown = 0,
		LT8718_VER_1,
};

struct lt8718_device {
		enum lt8718_chip_version ver;
		unsigned int status;
};

static const struct reg_sequence lt8718_rx_phy_cfg[] = {
		{ 0x7013, 0x03 },
		{ 0x7014, 0x24 },
		{ 0x8006, 0x03 },
		{ 0x8053, 0xc0 },
};

static const struct reg_sequence lt8718_rx_pll_cfg[] = {
		{ 0x7016, 0x40 },
		{ 0x7018, 0x13 },
		{ 0x7019, 0x1e },
		{ 0x701a, 0xa4 },
};

static const struct reg_sequence lt8718_audio_iis_cfg[] = {
		{ 0x881a, 0x00 },
		{ 0x8006, 0x23 },
		{ 0x9008, 0x8f },
		{ 0x8c42, 0x08 },
		{ 0x8c43, 0x10 },
		{ 0x8c44, 0x00 },
		{ 0x8c45, 0x00 },
		{ 0x8c0e, 0x09 },
		{ 0x8c4c, 0x00 },
		{ 0x8c50, 0x01 },
		{ 0x8c51, 0xf8 },
};

static const struct reg_sequence lt8718_tx_phy_cfg[] ={
		{ 0x7021, 0x0d },
		{ 0x7021, 0x0f },
		{ 0x7022, 0x77 },
		{ 0x7023, 0x77 },
		{ 0x7024, 0x80 },
		{ 0x7025, 0x00 },
		{ 0x7026, 0x80 },
		{ 0x7027, 0x00 },
		{ 0x7028, 0x80 },
		{ 0x7029, 0x00 },
		{ 0x702a, 0x80 },
		{ 0x702b, 0x00 },
		{ 0x702c, 0xb0 },
		{ 0x702c, 0xf0 },
		{ 0x702f, 0x70 },
		{ 0x7030, 0x24 },
		{ 0x7031, 0xfc },
		{ 0x8030, 0x0e },
};

static const struct reg_sequence lt8718_tx_pll_cfg[] ={
		{ 0x8040, 0x22 },
		{ 0x8041, 0x36 },
		{ 0x8042, 0x00 },
		{ 0x8043, 0x80 },
		{ 0x701c, 0x18 },
		{ 0x701d, 0x42 },
		{ 0x701e, 0x00 },
		{ 0x701e, 0x01 },
		{ 0x701f, 0x11 },
		{ 0x601e, 0xbf },
		{ 0x601e, 0xff },
		{ 0x701f, 0x13 },
		{ 0x8044, 0x41 },
		{ 0x8045, 0x03 },
		{ 0x8046, 0x0a },
		{ 0x8048, 0x0a },
		{ 0x8040, 0x22 },
};

static const struct reg_sequence lt8718_video_processor_cfg[] = {
		{ 0x8800, 0x4a },
		{ 0x8801, 0x02 },
		{ 0x8802, 0x10 },
		{ 0x8803, 0x01 },
		{ 0x8804, 0xff },
		{ 0x8805, 0x08 },
		{ 0x8806, 0x98 },
		{ 0x8807, 0x00 },
		{ 0x8808, 0xc0 },
		{ 0x8809, 0x00 },
		{ 0x880a, 0x2c },
		{ 0x880b, 0x07 },
		{ 0x880c, 0x80 },
		{ 0x880d, 0x04 },
		{ 0x880e, 0x65 },
		{ 0x880f, 0x02 },
		{ 0x8810, 0x33 },
		{ 0x8811, 0x00 },
		{ 0x8812, 0x29 },
		{ 0x8813, 0x00 },
		{ 0x8814, 0x05 },
		{ 0x8815, 0x04 },
		{ 0x8816, 0x38 },
		{ 0x881a, 0x00 },
		{ 0x881b, 0x0b },
		{ 0x881c, 0x40 },
		{ 0x8817, 0x08 },
		{ 0x8818, 0x20 },
		{ 0x8819, 0x00 },
		{ 0x881d, 0x36 },
		{ 0x881e, 0x30 },
		{ 0x881f, 0x4e },
		{ 0x8820, 0x66 },
		{ 0x8821, 0x1b },
		{ 0x8822, 0x09 },
		{ 0x8823, 0xff },
		{ 0x884b, 0xf2 },
		{ 0x884c, 0x00 },
		{ 0x884d, 0x00 },
};

static const struct reg_sequence lt8718_output_set_cfg[] = {
		{ 0x881e, 0x20},
		{ 0x8076, 0x40},
		{ 0x8052, 0x70},
		{ 0x881a, 0x30},
		{ 0x884b, 0x92},
		{ 0x8074, 0x28},
};

void dpcd_write_funtion(struct loonggpu_bridge_phy *phy, unsigned int  address, unsigned char data);
unsigned char dpcd_read_funtion(struct loonggpu_bridge_phy *phy, unsigned int address);
void link_configuration(struct loonggpu_bridge_phy *phy);
void drive_write_funtion(struct loonggpu_bridge_phy *phy, unsigned char data);
int tps_status(struct loonggpu_bridge_phy *phy, unsigned char tps);
unsigned char read_adjust_request(struct loonggpu_bridge_phy *phy, unsigned char tps);
void vd_dp_tx_swing_init(struct loonggpu_bridge_phy *phy,unsigned char swing);
void end_training(struct loonggpu_bridge_phy *phy);
void write_funtion(struct loonggpu_bridge_phy *phy, unsigned char number, unsigned char *data);
void lt8718_edid_read(struct loonggpu_bridge_phy *phy);
void dp_out_video_open(struct loonggpu_bridge_phy *phy);
#endif
