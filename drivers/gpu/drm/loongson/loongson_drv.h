#ifndef __LOONGSON_DRV_H__
#define __LOONGSON_DRV_H__

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/module.h>
#include <linux/types.h>
#include <asm/loongson.h>

#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_gem.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_vblank.h>

#include "loongson_i2c.h"
#include "loongson_vbios.h"

#define to_loongson_crtc(x) container_of(x, struct loongson_crtc, base)
#define to_loongson_encoder(x) container_of(x, struct loongson_encoder, base)
#define to_loongson_connector(x) container_of(x, struct loongson_connector, base)

#define LOONGSON_MAX_FB_HEIGHT	4096
#define LOONGSON_MAX_FB_WIDTH	4096

#define CUR_WIDTH_SIZE		32
#define CUR_HEIGHT_SIZE		32

#define LO_OFF	0
#define HI_OFF	8

#define LS7A_PIX0_PLL		(void *)TO_UNCACHE(LS7A_CHIPCFG_REG_BASE + 0x04b0)
#define LS7A_PIX1_PLL		(void *)TO_UNCACHE(LS7A_CHIPCFG_REG_BASE + 0x04c0)

#define CURIOSET_CORLOR		0x4607
#define CURIOSET_POSITION	0x4608
#define CURIOLOAD_ARGB		0x4609
#define CURIOLOAD_IMAGE		0x460A
#define CURIOHIDE_SHOW		0x460B
#define FBEDID_GET		0X860C

#define CRTC_REG_OFFSET		0x10

#define CFG_FMT			GENMASK(2,0)
#define CFG_FBSWITCH		BIT(7)
#define CFG_ENABLE		BIT(8)
#define CFG_PANELSWITCH 	BIT(9)
#define CFG_FBNUM_BIT		11
#define CFG_FBNUM		BIT(11)
#define CFG_GAMMAR		BIT(12)
#define CFG_RESET		BIT(20)

#define FB_CFG_DVO_REG		(0x1240)
#define FB_ADDR0_DVO_REG	(0x1260)
#define FB_ADDR1_DVO_REG	(0x1580)
#define FB_STRI_DVO_REG		(0x1280)
#define FB_DITCFG_DVO_REG	(0x1360)
#define FB_DITTAB_LO_DVO_REG	(0x1380)
#define FB_DITTAB_HI_DVO_REG	(0x13a0)
#define FB_PANCFG_DVO_REG	(0x13c0)
#define FB_PANTIM_DVO_REG	(0x13e0)
#define FB_HDISPLAY_DVO_REG	(0x1400)
#define FB_HSYNC_DVO_REG	(0x1420)
#define FB_VDISPLAY_DVO_REG	(0x1480)
#define FB_VSYNC_DVO_REG	(0x14a0)
#define FB_GAMINDEX_DVO_REG	(0x14e0)
#define FB_GAMDATA_DVO_REG	(0x1500)

#define HDMI_ZONEIDLE_REG 	(0x1700)
#define HDMI_CTRL_REG		(0x1720)

#define HDMI_AUDIO_BUF_REG	(0x1740)
#define HDMI_AUDIO_NCFG_REG	(0x1760)
#define HDMI_AUDIO_CTSCFG_REG	(0x1780)
#define HDMI_AUDIO_CTSCALCFG_REG	(0x17a0)
#define HDMI_AUDIO_INFOFRAME_REG	(0x17c0)
#define HDMI_AUDIO_SAMPLE_REG	(0x17e0)

#define HDMI_PHY_CTRL_REG	(0x1800)
#define HDMI_PHY_PLLCFG_REG	(0x1820)

#define FB_CUR_CFG_REG		(0x1520)
#define FB_CUR_ADDR_REG		(0x1530)
#define FB_CUR_LOC_ADDR_REG	(0x1540)
#define FB_CUR_BACK_REG		(0x1550)
#define FB_CUR_FORE_REG		(0x1560)
#define FB_INT_REG		(0x1570)

#define INT_DVO1_VSYNC		0
#define INT_DVO1_HSYNC		1
#define INT_DVO0_VSYNC		2
#define INT_DVO0_HSYNC		3
#define INT_CURSOR_FB_END	4
#define INT_DVO1_FB_END		5
#define INT_DVO0_FB_END		6

#define MAX_CRTC 2

enum loongson_chip {
	dc_7a1000,
	dc_7a2000
};

struct pix_pll {
	unsigned int l2_div;
	unsigned int l1_loopc;
	unsigned int l1_frefc;
};

struct config_reg {
	u8 dev_addr;
	u8 reg;
	u8 value;
} __packed;

struct cfg_encoder {
	u8 reg_num;
	u32 hdisplay;
	u32 vdisplay;
	struct config_reg config_regs[256];
};

struct loongson_crtc {
	struct drm_crtc base;
	unsigned int crtc_id;
	uint32_t cfg_reg;
	struct drm_plane *primary; /* Primary panel belongs to this crtc */
	struct drm_pending_vblank_event *event;
	struct loongson_drm_device *ldev;
};

struct loongson_encoder {
	struct drm_encoder base;
	u32 type;
	u32 i2c_id;
	int encoder_id;
	struct loongson_i2c *i2c;
	struct loongson_crtc *lcrtc; /* Binding crtc, not actual one */
};

struct loongson_connector {
	struct drm_connector base;
	u16 id;
	u32 type;
	u16 i2c_id;
	u16 hotplug;
	u16 edid_method;
	u8 *vbios_edid;
	struct loongson_i2c *i2c;
	struct loongson_drm_device *ldev;
};

struct loongson_mode_info {
	bool mode_config_initialized;
	struct loongson_crtc *crtc;
	struct loongson_encoder *encoder;
	struct loongson_connector *connector;
};

struct loongson_drm_device {
	struct drm_device *dev;
	struct drm_atomic_state	*state;
	struct pci_dev *gpu_pdev; /* LS7A gpu device info */

	resource_size_t	mmio_base;
	resource_size_t	mmio_size;
	void __iomem	*mmio;
	uint32_t	int_reg;

	struct drm_display_mode		mode;
	struct loongson_mode_info	mode_info[2];
	struct drm_gem_dma_object	*cursor;

	int 			num_crtc;
	struct loongson_crtc 	lcrtc[MAX_CRTC];
	struct loongson_i2c	i2c_bus[DC_I2C_BUS_MAX];

	void *vbios;
	struct list_head desc_list;

	bool	inited;
	bool 	suspended;
	bool	cursor_showed;
	int	cursor_crtc_id;

	int connector_active0;
	int connector_active1;

	enum loongson_chip chip;
};

extern spinlock_t loongson_reglock;

/* FIXME: LS7A2000's switch_panel is not available, just return false. */
static inline bool clone_mode(struct loongson_drm_device *ldev)
{
	return false;

	if (ldev->num_crtc < 2)
		return true;
	if (ldev->mode_info[0].connector->base.status != connector_status_connected)
		return true;
	if (ldev->mode_info[1].connector->base.status != connector_status_connected)
		return true;
	if (ldev->lcrtc[0].base.x || ldev->lcrtc[0].base.y)
		return false;
	if (ldev->lcrtc[1].base.x || ldev->lcrtc[1].base.y)
		return false;

	return true;
}

int loongson_irq_install(struct drm_device *dev, int irq);
void loongson_irq_uninstall(struct drm_device *dev);
int loongson_irq_enable_vblank(struct drm_device *dev,unsigned int crtc_id);
void loongson_irq_disable_vblank(struct drm_device *dev,unsigned int crtc_id);

u32 crtc_read(struct loongson_crtc *lcrtc, u32 offset);
void crtc_write(struct loongson_crtc *lcrtc, u32 offset, u32 val);

int loongson_gpio_init(struct loongson_drm_device *ldev);
int loongson_crtc_init(struct loongson_drm_device *ldev);
struct drm_encoder *loongson_encoder_init(struct drm_device *dev, unsigned int index);
struct drm_connector *loongson_connector_init(struct drm_device *dev, unsigned int index);

int loongson_fbdev_init(struct loongson_drm_device *ldev);
void loongson_fbdev_fini(struct loongson_drm_device *ldev);
void loongson_fbdev_restore_mode(struct loongson_drm_device *ldev);

int loongson_drm_drm_suspend(struct drm_device *dev, bool suspend,
                                   bool fbcon, bool freeze);
int loongson_drm_drm_resume(struct drm_device *dev, bool resume, bool fbcon);
		   /* loongson_cursor.c */
int loongson_crtc_cursor_set2(struct drm_crtc *crtc, struct drm_file *file_priv,
					uint32_t handle, uint32_t width, uint32_t height, int32_t hot_x, int32_t hot_y);
int loongson_crtc_cursor_move(struct drm_crtc *crtc, int x, int y);

#endif
