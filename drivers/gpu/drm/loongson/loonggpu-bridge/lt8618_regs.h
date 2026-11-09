/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2021 Loongson Technology Co., Ltd.
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __LT8618_REGS_H__
#define __LT8618_REGS_H__

#include "bridge_phy.h"
#include "lt8618_drv.h"

/**
 * @brief Config lt8618 registers attribute
 */
static const u8 lt8618_reg_defaults_raw[] = {
	0x54, 0x49, 0x12, 0x16, 0x1c, 0x60, 0x00, 0x00,
	0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x0c, 0x08,
	0x96, 0xa0, 0x60, 0x20, 0x01, 0xf3, 0x82, 0xf4,
	0x00, 0x74, 0xe0, 0x03, 0x00, 0x00, 0xff, 0x00,
};

static bool lt8618_register_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LT8618_REG_PAGE_SELECT:
		return false;
	default:
		return true;
	}
}

static bool lt8618_register_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00:
		return false;
	case LT8618_REG_PAGE_SELECT:
	default:
		return true;
	}
}

static const struct regmap_range lt8618_rw_regs_range[] = {
	regmap_reg_range(LT8618_REG_START, LT8618_REG_END),
};

static const struct regmap_range lt8618_wo_regs_range[] = {
	regmap_reg_range(0x00FF, 0x00FF),
};

static const struct regmap_range lt8618_ro_regs_range[] = {
	regmap_reg_range(LT8618_REG_CHIP_VERSION_BASE,
			 LT8618_REG_CHIP_VERSION_BASE + CHIP_VERSION_LEN),
};

static const struct regmap_range lt8618_vo_regs_range[] = {
	regmap_reg_range(LT8618_REG_START, LT8618_REG_END),
};

/**
 * @brief lt8618 val sequence
 */
static const u8 lt8618_pll_range_timing[][3][3] = {
	{
		{ 0x00, 0x9e, 0xaa },
		{ 0x00, 0x9e, 0x99 },
		{ 0x00, 0x9e, 0x88 },
	},
	{
		{ 0x00, 0x94, 0xaa },
		{ 0x01, 0x94, 0x99 },
		{ 0x03, 0x94, 0x88 },
	},
	{
		{ 0x00, 0x9e, 0xaa },
		{ 0x00, 0x9e, 0x99 },
		{ 0x00, 0x9e, 0x88 },
	},
};

static const u8 lt8618_pll_timing_u3[][3] = {
	{ 0x00, 0x9e, 0xaa },
	{ 0x00, 0x9e, 0x99 },
	{ 0x00, 0x9e, 0x88 },
};
static const u8 lt8618_pll_timing_u2[][3] = {
	{ 0x00, 0x94, 0xaa },
	{ 0x01, 0x94, 0x99 },
	{ 0x03, 0x94, 0x88 },
};

/**
 * @brief lt8618 regmap sequence
 */
static const struct reg_sequence lt8618_sw_reset_seq[] = {
	/* Reset MIPI Rx logic */
	{ 0x8011, 0x00 },
	/* Reset TTL video process */
	{ 0x8013, 0xF1 },
	{ 0x8013, 0xF9 },
};

static const struct reg_sequence lt8618_input_analog_seq[] = {
	/* TTL mode */
	{ 0x8102, 0x66 },
	{ 0x810A, 0x06 },
	{ 0x8115, 0x06 },
	/* for U2 */
	{ 0x814E, 0xA8 },
	/* Frequency meter 2 timer cycle for sys_clk */
	/* 0x61A8 = 25000, 0x77EC=30700 */
	{ 0x821B, 0x77 },
	{ 0x821C, 0xEC },
};

static const struct reg_sequence lt8618_output_analog_seq[] = {
	/* HDMI TX PLL */
	{ 0x8123, 0x40 },
	{ 0x8124, 0x64 },
	{ 0x8126, 0x55 },
	/* U3 SDR/DDR fixed phase */
	{ 0x8129, 0x04 },
};

static const struct reg_sequence lt8618_pll_cfg_seq[] = {
	{ 0x82de, 0x00 },
	{ 0x82de, 0xC0 },

	{ 0x8016, 0xF1 },
	/* TX PLL sw_rst_n */
	{ 0x8018, 0xDC },
	{ 0x8018, 0xFC },
	{ 0x8016, 0xF3 },
};

#endif
