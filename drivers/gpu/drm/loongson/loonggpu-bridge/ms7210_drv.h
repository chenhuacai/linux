/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2021 Loongson Technology Co., Ltd.
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __MS7210_H__
#define __MS7210_H__

#define to_dev(phy) (regmap_get_device(phy->phy_regmap))

#define __SIMPLE_REG_SEQ_FUNC(NAME, CMD) NAME##_##CMD
#define _SIMPLE_REG_SEQ_FUNC(NAME, CMD) __SIMPLE_REG_SEQ_FUNC(NAME, CMD)
#define SIMPLE_REG_SEQ_FUNC(CMD) _SIMPLE_REG_SEQ_FUNC(CHIP_PREFIX, CMD)

#define __DEFINE_SIMPLE_REG_SEQ_FUNC(NAME, CMD)                                \
	static int NAME##_##CMD(struct loonggpu_bridge_phy *phy)               \
	{                                                                      \
		regmap_multi_reg_write(phy->phy_regmap, NAME##_##CMD##_seq,    \
					ARRAY_SIZE(NAME##_##CMD##_seq));       \
		return 0;                                                      \
	}
#define _DEFINE_SIMPLE_REG_SEQ_FUNC(NAME, CMD)                                 \
	__DEFINE_SIMPLE_REG_SEQ_FUNC(NAME, CMD)
#define DEFINE_SIMPLE_REG_SEQ_FUNC(CMD)                                        \
	_DEFINE_SIMPLE_REG_SEQ_FUNC(CHIP_PREFIX, CMD)

#define MS7210_CHIP_NAME "MS7210"
#define MS7210_CHIP_ADDR 0x59
#define MS7210_GPIO_PLACEMENT_OFFSET 16U

#define MS7210_REG_START 0x0000
#define MS7210_REG_END 0xFF00

/* General Registers */
#define MS7210_REG_BASE MS7210_REG_START
#define MS7210_REG_CHIP_VERSION_BASE MS7210_REG_BASE
#define CHIP_VERSION_LEN 3U
#define MS7210_REG_CHIP_VERSION(x) (MS7210_REG_CHIP_VERSION_BASE + (x))
#define CHIP_ID0 0U
#define CHIP_ID1 1U
#define CHIP_ID2 2U
#define MS7210_CHIP_ID0 0xA2
#define MS7210_CHIP_ID1 0x20
#define MS7210_CHIP_ID2 0x0A

#define MS7210_REG_DDC_CTRL 0x1300
#define CTRL_ON 0xFF
#define MS7210_REG_DDC_ENABLE 0x1200
#define DDC_ENABLE 0x0E
#define DDC_DISENABLE 0x0F

#define MS7210_REG_HPD_STATUS 0x0205
#define LINK_STATUS_OUTPUT_DC_POS 0U
#define LINK_STATUS_OUTPUT_DC_MSK BIT_MASK(LINK_STATUS_OUTPUT_DC_POS)
#define LINK_STATUS_STABLE 1U
#define LINK_STATUS_UNSTABLE 0U

#define MS7210_REG_INT_CTRL 0x1800
#define INT_CTRL_HPD_POS 5U
#define INT_CTRL_HPD_MSK BIT_MASK(INT_CTRL_HPD_POS)
#define INT_HPD_ENABLE 1U
#define INT_HPD_DISABLE 0U

#define MS7210_REG_INT_STATUS 0xC005
#define INT_STATUS_HPD_POS 0U
#define INT_STATUS_HPD_MSK BIT_MASK(INT_STATUS_HPD_POS)
#define INT_HPD_PLUG_ON  0U
#define INT_HPD_PLUG_OFF 1U

#define MS7210_REG_INT_CLEAR 0x0005
#define INT_CLEAR_HPD_POS 0U
#define INT_CLEAR_HPD_MSK BIT_MASK(INT_CLEAR_HPD_POS)
#define INT_HPD_CLEAR 1U
#define MS7210_REG_BDOP (MS7210_REG_BASE + 0xA300)

enum ms7210_pll_level {
	MS7210_PLL_LEVEL_LOW = 0,
	MS7210_PLL_LEVEL_MIDDLE,
	MS7210_PLL_LEVEL_HIGH,
};

enum ms7210_ddc_cmd {
	CMD_ABORT = 0x0,
	CMD_READ_BURST = 0x1,
	CMD_WRITE_BURST = 0x2,
	CMD_READ_SEEK = 0x3,
	CMD_READ_EDDC = 0x4,
	CMD_RESET = 0x6,
};

struct ms7210_device {
	enum ms7210_pll_level pll_level;
};

#endif
