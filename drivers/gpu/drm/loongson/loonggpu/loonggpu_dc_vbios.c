#include <asm/loongson.h>

#include "loonggpu.h"
#include "loonggpu_dc_vbios.h"
#include "loonggpu_dc_resource.h"
#include "bridge_phy.h"

#define VBIOS_START 0x1000
#define VBIOS_SIZE 0x40000
#define VBIOS_OFFSET 0x100000
#define VBIOS_DESC_OFFSET 0x6000
#define VBIOS_DESC_TOTAL 0xA00
#define LOONGSON_VBIOS_TITLE "Loongson-VBIOS"

const char * const loonggpu_vram_type_names[] = {
	[DDR3] = "DDR3",
	[DDR4] = "DDR4",
	[DDR5] = "DDR5",
	[LPDDR4] = "LPDDR4",
	[VRAM_UNKNOWN] = "Unknown",
};

static bool is_valid_vbios(void *vbios)
{
	struct vbios_info *vb_header = NULL;
	u8 header[16] = {0};

	vb_header = (struct vbios_info *)vbios;
	memcpy(&header[0], vb_header->title, sizeof(vb_header->title));

	if (0 != memcmp((char *)&header[0],
			LOONGSON_VBIOS_TITLE,
			strlen(LOONGSON_VBIOS_TITLE))) {
		DRM_WARN("vbios signature is invation!\n");
		return false;
	}

	return true;
}

static bool read_bios_from_vram(struct loonggpu_dc *dc)
{
	void *bios;
	u64 vbios_addr = dc->adev->gmc.aper_base +
			 dc->adev->gmc.aper_size - VBIOS_OFFSET;
	bios = ioremap(vbios_addr, VBIOS_SIZE);
	if (!bios)
		return false;

	dc->vbios->vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!dc->vbios->vbios_ptr)
		return false;

	memcpy(dc->vbios->vbios_ptr, bios, VBIOS_SIZE);
	iounmap(bios);
	if (!is_valid_vbios(dc->vbios->vbios_ptr)) {
		kfree(dc->vbios->vbios_ptr);
		return false;
	}

	DRM_INFO("LOONGGPU get vbios from vram Success \n");
	return true;
}

static bool read_bios_from_sysconf(struct loonggpu_dc *dc)
{
#if defined(LG_LOONGSON_SYS_CONF_HAS_VGABIOS_ADDR)
	if (!loongson_sysconf.vgabios_addr)
		return false;
	else if(!virt_addr_valid((void *)loongson_sysconf.vgabios_addr))
		return false;
#else
	return false;
#endif

	dc->vbios->vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!dc->vbios->vbios_ptr)
		return false;

#if defined(LG_LOONGSON_SYS_CONF_HAS_VGABIOS_ADDR)
	memcpy(dc->vbios->vbios_ptr, (void *)loongson_sysconf.vgabios_addr, VBIOS_SIZE);
#endif
	DRM_INFO("LOONGGPU get vbios from sysconf Success \n");

	return true;
}

#ifdef CONFIG_ACPI
static bool read_bios_from_acpi(struct loonggpu_dc *dc)
{
	struct acpi_table_header *hdr;
	struct acpi_viat_table *viat;
	void *vaddr;
	acpi_size tbl_size;

	if (!ACPI_SUCCESS(acpi_get_table("VIAT", 1, &hdr)))
		return false;

	tbl_size = hdr->length;
	if (tbl_size != sizeof(struct acpi_viat_table)) {
		DRM_WARN("ACPI viat table present but broken (length error #1)\n");
		return false;
	}

	viat = (struct acpi_viat_table *)hdr;
	dc->vbios->vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!dc->vbios->vbios_ptr)
		return false;

	if (!pfn_valid(PFN_DOWN(viat->vbios_addr))) {
		kfree(dc->vbios->vbios_ptr);
		return false;
	}

	vaddr = phys_to_virt(viat->vbios_addr);
	memcpy(dc->vbios->vbios_ptr, vaddr, VBIOS_SIZE);
	DRM_INFO("Get vbios from ACPI success!\n");
	return true;
}
#else
static bool read_bios_from_acpi(struct loonggpu_device *adev)
{
	return false;
}
#endif

static bool get_vbios_data(struct loonggpu_dc *dc)
{
	if (read_bios_from_vram(dc))
		goto success;

	if (read_bios_from_acpi(dc))
		goto success;

	if (read_bios_from_sysconf(dc))
		goto success;

	DRM_ERROR("Unable to locate a BIOS ROM\n");
	return false;

success:
	return true;
}

static bool parse_vbios_header(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_header_resource)
		ret = vbios->funcs->create_header_resource(vbios, data, vb_desc->size);

	return ret;
}

static bool parse_vbios_crtc(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_crtc_resource)
		ret = vbios->funcs->create_crtc_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_connector(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_connecor_resource)
		ret = vbios->funcs->create_connecor_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_encoder(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_encoder_resource)
		ret = vbios->funcs->create_encoder_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_i2c(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_i2c_resource)
		ret = vbios->funcs->create_i2c_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_pwm(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_pwm_resource)
		ret = vbios->funcs->create_pwm_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_backlight(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_backlight_resource)
		ret = vbios->funcs->create_backlight_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_gpu(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_gpu_resource)
		ret = vbios->funcs->create_gpu_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_panel(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_panel_resource)
		ret = vbios->funcs->create_panel_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_ext_encoder(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;
	if (vbios->funcs && vbios->funcs->create_ext_encoder_resource)
		ret = vbios->funcs->create_ext_encoder_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_lcd_ctrl(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_lcd_ctrl_resource)
		ret = vbios->funcs->create_lcd_ctrl_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_scale(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_scale_resource)
		ret = vbios->funcs->create_scale_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_dpm_config(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	bool ret = false;
	u8 *data;

	if (IS_ERR_OR_NULL(vb_desc) || IS_ERR_OR_NULL(vbios))
		return ret;

	data = (u8 *)vbios->vbios_ptr + vb_desc->offset;

	if (vbios->funcs && vbios->funcs->create_dpm_config_resource)
		ret = vbios->funcs->create_dpm_config_resource(vbios, data, vb_desc->link, vb_desc->size);

	return ret;
}

static bool parse_vbios_default(struct vbios_desc *vb_desc, struct loonggpu_vbios *vbios)
{
	DRM_WARN("Current descriptor[T-%d][V-%d] cannot be interprete.\n",
		  vb_desc->type, vb_desc->ver);
	return false;
}

#define FUNC(t, v, f)                                                         \
	{                                                                     \
		.type = t, .ver = v, .func = f,                               \
	}

static struct desc_func tables[] = {
	FUNC(desc_header, ver_v1, parse_vbios_header),
	FUNC(desc_crtc, ver_v1, parse_vbios_crtc),
	FUNC(desc_encoder, ver_v1, parse_vbios_encoder),
	FUNC(desc_connector, ver_v1, parse_vbios_connector),
	FUNC(desc_i2c, ver_v1, parse_vbios_i2c),
	FUNC(desc_pwm, ver_v1, parse_vbios_pwm),
	FUNC(desc_backlight, ver_v1, parse_vbios_backlight),
	FUNC(desc_gpu, ver_v1, parse_vbios_gpu),
	FUNC(desc_panel, ver_v1, parse_vbios_panel),
	FUNC(desc_res_encoder, ver_v1, parse_vbios_ext_encoder),
	FUNC(desc_lcd_ctrl, ver_v1, parse_vbios_lcd_ctrl),
	FUNC(desc_scale, ver_v1, parse_vbios_scale),
	FUNC(desc_scale, ver_v1, parse_vbios_dpm_config),
};

static inline parse_func *get_parse_func(struct vbios_desc *desc)
{
	parse_func *func = parse_vbios_default;
	u32 tt_num = ARRAY_SIZE(tables);
	u32 type = desc->type;
	u32 ver = desc->ver;
	int i;

	for (i = 0; i < tt_num; i++) {
		if ((tables[i].ver == ver) && (tables[i].type == type)) {
			func = tables[i].func;
			break;
		}
	}

	return func;
}

static inline void parse_vbios_info(struct loonggpu_vbios *vbios)
{
	struct vbios_info *vb_info;
	struct header_resource *header;

	if (IS_ERR_OR_NULL(vbios))
		return;

	header = vbios->funcs->get_header_resource(vbios);
	if (IS_ERR_OR_NULL(header))
		return;

	vb_info = (struct vbios_info *)vbios->vbios_ptr;
	header->links = vb_info->link_num;
	header->ver_majro = vb_info->version_major;
	header->ver_minor = vb_info->version_minor;
	memcpy(header->name, vb_info->title, 16);
	memcpy(header->board, vb_info->board, 20);
}

static bool dc_vbios_parse(struct loonggpu_vbios *vbios)
{
	struct vbios_header *vb_header;
	struct vbios_desc *start;
	struct vbios_desc *desc;
	enum desc_type type;
	parse_func *func;
	u8 *vbios_ptr;
	bool ret;

	if (IS_ERR_OR_NULL(vbios))
		return false;

	vbios_ptr = (u8 *)vbios->vbios_ptr;
	if (IS_ERR_OR_NULL(vbios_ptr))
		return false;

	/* get header for global information of vbios */
	desc = (struct vbios_desc *)(vbios_ptr + VBIOS_DESC_OFFSET);
	if (desc->type != desc_header) {
		pr_err("vbios first desc not header type\n");
		return false;
	}

	func = get_parse_func(desc);
	if (IS_ERR_OR_NULL(func)) {
		pr_err("vbios get header parser funcs err %pf \n", func);
		return false;
	}

	ret = (*func)(desc, vbios);
	if (!ret) {
		pr_err("get vbios header info error \n");
		return false;
	}

	vb_header = (struct vbios_header *)(vbios_ptr + desc->offset);
	DRM_DEBUG("oem-vendor %s oem-product %s\n", vb_header->oem_vendor,
		 vb_header->oem_product);

	/* start parsing vbios components */
	start = desc = (struct vbios_desc *)(vbios_ptr + vb_header->desc_offset);
	while (1) {
		type = desc->type;
		if (type == desc_header) {
			desc++;
			continue;
		}

		if (type == desc_max || ((desc - start) > vb_header->desc_size) ||
		    ((desc - start) > VBIOS_DESC_TOTAL))
			break;

		func = get_parse_func(desc);
		if (IS_ERR_OR_NULL(func))
			continue;

		ret = (*func)(desc, vbios);
		if (!ret)
			pr_err("Parse T-%d V-%d failed[%d]\n", desc->ver, desc->type, ret);

		desc++;
	}

	/* append legacy information to header resource */
	parse_vbios_info(vbios);

	return true;
}

static bool vbios_resource_pool_create(struct loonggpu_vbios *vbios)
{
	if (IS_ERR_OR_NULL(vbios))
		return false;

	return dc_vbios_parse(vbios);
}

static bool vbios_resource_pool_destory(struct loonggpu_vbios *vbios)
{
	struct resource_object *entry, *tmp;

	if (IS_ERR_OR_NULL(vbios))
		return false;

	if (list_empty(&vbios->resource_list))
		return true;

	list_for_each_entry_safe (entry, tmp, &vbios->resource_list, node) {
		list_del(&entry->node);
		kvfree(entry);
		entry = NULL;
	}

	return true;
}

static bool vbios_create_header_resource(struct loonggpu_vbios *vbios, void *data, u32 size)
{
	struct vbios_header vb_header;
	struct header_resource *header;
	u32 header_size = sizeof(struct vbios_header);

	header = kvmalloc(sizeof(*header), GFP_KERNEL);
	if (IS_ERR_OR_NULL(header))
		return false;

	memset(&vb_header, VBIOS_DATA_INVAL, header_size);
	memcpy(&vb_header, data, min(size, header_size));

	memcpy(header->oem_product, vb_header.oem_product, 32);
	memcpy(header->oem_vendor, vb_header.oem_vendor, 32);
	header->base.type = LOONGGPU_RESOURCE_HEADER;

	list_add_tail(&header->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_backlight(struct loonggpu_vbios *vbios, void *data,
				   u32 link, u32 size)
{
	struct vbios_backlight vb_backlight;
	struct backlight_resource *backlight;
	u32 backlight_size = sizeof(struct vbios_backlight);

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	backlight = kvmalloc(sizeof(*backlight), GFP_KERNEL);
	if (IS_ERR_OR_NULL(backlight))
		return false;

	memset(&vb_backlight, VBIOS_DATA_INVAL, backlight_size);
	memcpy(&vb_backlight, data, min(size, backlight_size));

	backlight->base.link = link;
	backlight->base.type = LOONGGPU_RESOURCE_BACKLIGHT;
	backlight->feature = vb_backlight.feature;
	backlight->used = vb_backlight.used;
	backlight->type = vb_backlight.type;

	list_add_tail(&backlight->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_crtc_resource(struct loonggpu_vbios *vbios,
				       void *data, u32 link, u32 size)
{
	struct vbios_crtc vb_crtc;
	struct crtc_resource *crtc;
	u32 crtc_size = sizeof(struct vbios_crtc);

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	crtc = kvmalloc(sizeof(*crtc), GFP_KERNEL);
	if (IS_ERR_OR_NULL(crtc))
		return false;

	memset(&vb_crtc, VBIOS_DATA_INVAL, crtc_size);
	memcpy(&vb_crtc, data, min(size, crtc_size));

	crtc->base.link = link;
	crtc->base.type = LOONGGPU_RESOURCE_CRTC;
	crtc->feature = vb_crtc.feature;
	crtc->crtc_id = vb_crtc.crtc_id;
	crtc->encoder_id = vb_crtc.encoder_id;
	crtc->max_freq = vb_crtc.max_freq;
	crtc->max_width = vb_crtc.max_width;
	crtc->max_height = vb_crtc.max_height;
	crtc->is_vb_timing = vb_crtc.is_vb_timing;

	list_add_tail(&crtc->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_encoder_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct vbios_encoder vb_encoder;
	struct encoder_resource *encoder;
	u32 encoder_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	encoder = kvmalloc(sizeof(*encoder), GFP_KERNEL);
	if (IS_ERR_OR_NULL(encoder))
		return false;

	encoder_size = sizeof(struct vbios_encoder);
	memset(&vb_encoder, VBIOS_DATA_INVAL, encoder_size);
	memcpy(&vb_encoder, data, min(size, encoder_size));

	encoder->base.link = link;
	encoder->base.type = LOONGGPU_RESOURCE_ENCODER;
	encoder->feature = vb_encoder.feature;
	encoder->i2c_id = vb_encoder.i2c_id;
	encoder->connector_id = vb_encoder.connector_id;
	encoder->type = vb_encoder.type;
	encoder->config_type = vb_encoder.config_type;
	encoder->chip_addr = vb_encoder.chip_addr;
	encoder->chip = vb_encoder.chip;
	if (encoder->feature >= 1)
		encoder->reset_gpio = vb_encoder.reset_gpio;

	list_add_tail(&encoder->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_connector_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct vbios_connector vb_connector;
	struct connector_resource *connector;
	u32 connector_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	connector = kvmalloc(sizeof(*connector), GFP_KERNEL);
	if (IS_ERR_OR_NULL(connector))
		return false;

	connector_size = sizeof(struct vbios_connector);
	memset(&vb_connector, VBIOS_DATA_INVAL, connector_size);
	memcpy(&vb_connector, data, min(size, connector_size));

	connector->base.link = link;
	connector->base.type = LOONGGPU_RESOURCE_CONNECTOR;

	connector->feature = vb_connector.feature;
	connector->i2c_id = vb_connector.i2c_id;

	connector->type = vb_connector.type;
	connector->hotplug = vb_connector.hotplug;
	connector->edid_method = vb_connector.edid_method;
	connector->irq_gpio = vb_connector.irq_gpio;
	connector->gpio_placement = vb_connector.gpio_placement;
	memcpy(connector->internal_edid, vb_connector.internal_edid, 256);
	if (connector->feature >= 1)
		connector->multi_interface = vb_connector.multi_interface;
	else
		connector->multi_interface = false;

	list_add_tail(&connector->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_i2c_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct i2c_resource *i2c_resource;
	struct vbios_i2c vb_i2c;
	u32 i2c_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	i2c_resource = kvmalloc(sizeof(*i2c_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(i2c_resource))
		return false;

	i2c_size = sizeof(struct vbios_i2c);
	memset(&vb_i2c, VBIOS_DATA_INVAL, i2c_size);
	memcpy(&vb_i2c, data, min(size, i2c_size));

	i2c_resource->type = vb_i2c.type;
	i2c_resource->base.link = link;
	i2c_resource->base.type = LOONGGPU_RESOURCE_I2C;

	i2c_resource->feature = vb_i2c.feature;
	i2c_resource->id = vb_i2c.id;

	list_add_tail(&i2c_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_gpio_resource(struct loonggpu_vbios *vbios,
				       void *data, u32 link, u32 size)
{
	return false;
}

static bool vbios_create_pwm_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct pwm_resource *pwm_resource;
	struct vbios_pwm vb_pwm;
	u32 pwm_size, index;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	pwm_resource = kvmalloc(sizeof(*pwm_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(pwm_resource))
		return false;
	memset(pwm_resource, 0, sizeof(*pwm_resource));

	pwm_size = sizeof(struct vbios_pwm);
	memset(&vb_pwm, VBIOS_DATA_INVAL, pwm_size);
	memcpy(&vb_pwm, data, min(size, pwm_size));

	pwm_resource->base.link = link;
	pwm_resource->base.type = LOONGGPU_RESOURCE_PWM;

	pwm_resource->feature = vb_pwm.feature;
	pwm_resource->pwm = vb_pwm.pwm;
	pwm_resource->polarity = vb_pwm.polarity;
	pwm_resource->period = vb_pwm.period;

	if (pwm_resource->feature >= 2) {
		/* Ensure that the count data is valid */
		if (vb_pwm.light.level_count && vb_pwm.light.level_count <= PWM_LIGHT_TABLES_MAX) {
			pwm_resource->light.used = vb_pwm.light.used;
			pwm_resource->light.level_count = vb_pwm.light.level_count;
			for (index = 0; index < vb_pwm.light.level_count; index++) {
				pwm_resource->light.tables[index].level = vb_pwm.light.tables[index].level;
				pwm_resource->light.tables[index].duty = vb_pwm.light.tables[index].duty;
			}
		}
	}

	list_add_tail(&pwm_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_gpu_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct gpu_resource *gpu_resource;
	struct vbios_gpu vb_gpu;
	u32 gpu_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	gpu_resource = kvmalloc(sizeof(*gpu_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(gpu_resource))
		return false;

	gpu_size = sizeof(struct vbios_gpu);
	memset(&vb_gpu, VBIOS_DATA_INVAL, gpu_size);
	memcpy(&vb_gpu, data, min(size, gpu_size));

	gpu_resource->base.link = 0;
	gpu_resource->base.type = LOONGGPU_RESOURCE_GPU;

	gpu_resource->vram_type = vb_gpu.type;
	gpu_resource->bit_width = vb_gpu.bit_width;
	gpu_resource->cap = vb_gpu.cap;
	gpu_resource->count_freq = vb_gpu.count_freq;
	gpu_resource->freq = vb_gpu.freq;
	gpu_resource->shaders_num = vb_gpu.shaders_num;
	gpu_resource->shaders_freq = vb_gpu.shaders_freq;

	list_add_tail(&gpu_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_ext_encoder_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct ext_encoder_resources *ext_encoder_resource;
	struct vbios_ext_encoder *vb_ext_encoder;
	u32 ext_encoder_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	ext_encoder_resource = kvmalloc(sizeof(*ext_encoder_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(ext_encoder_resource))
		return false;

	vb_ext_encoder = kvmalloc(sizeof(*vb_ext_encoder), GFP_KERNEL);
	if (IS_ERR_OR_NULL(vb_ext_encoder)) {
		kvfree(ext_encoder_resource);
		return false;
	}

	ext_encoder_size = sizeof(struct vbios_ext_encoder);
	memset(vb_ext_encoder, VBIOS_DATA_INVAL, ext_encoder_size);
	memcpy(vb_ext_encoder, data, min(size, ext_encoder_size));

	ext_encoder_resource->base.link = link;
	ext_encoder_resource->base.type = LOONGGPU_RESOURCE_EXT_ENCODER;

	ext_encoder_resource->data_checksum = vb_ext_encoder->data_checksum;
	ext_encoder_resource->data_size = vb_ext_encoder->data_size;
	memcpy(ext_encoder_resource->data, vb_ext_encoder->data, vb_ext_encoder->data_size);

	list_add_tail(&ext_encoder_resource->base.node, &vbios->resource_list);

	kvfree(vb_ext_encoder);
	return true;
}

static bool vbios_create_panel_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct panel_resource *panel_resource;
	struct vbios_panel vb_panel;
	u32 panel_size;
	u32 index;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	panel_resource = kvmalloc(sizeof(*panel_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(panel_resource))
		return false;

	panel_size = sizeof(struct vbios_panel);
	memset(&vb_panel, VBIOS_DATA_INVAL, panel_size);
	memcpy(&vb_panel, data, min(size, panel_size));

	panel_resource->base.link = link;
	panel_resource->base.type = LOONGGPU_RESOURCE_PANEL;

	panel_resource->feature = vb_panel.feature;
	panel_resource->count = vb_panel.count;

	panel_resource->max_vrefresh = vb_panel.max_vrefresh;
	panel_resource->max_hdisplay = vb_panel.max_hdisplay;
	panel_resource->max_vdisplay = vb_panel.max_vdisplay;

	for (index = 0; index < vb_panel.count; index++) {
		panel_resource->timing[index].vrefresh = vb_panel.timing[index].vrefresh;
		panel_resource->timing[index].hdisplay = vb_panel.timing[index].hdisplay;
		panel_resource->timing[index].vdisplay = vb_panel.timing[index].vdisplay;
	}

	if (panel_resource->feature >= 1)
		panel_resource->max_pixclk = vb_panel.max_pixclk;

	list_add_tail(&panel_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_scale_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct scale_resource *scale_resource;
	struct vbios_scale vb_scale;
	u32 scale_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	scale_resource = kvmalloc(sizeof(*scale_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(scale_resource))
		return false;

	scale_size = sizeof(struct vbios_scale);
	memset(&vb_scale, VBIOS_DATA_INVAL, scale_size);
	memcpy(&vb_scale, data, min(size, scale_size));

	scale_resource->base.link = link;
	scale_resource->base.type = LOONGGPU_RESOURCE_SCALE;

	scale_resource->feature = vb_scale.feature;
	scale_resource->enable = vb_scale.enable;

	list_add_tail(&scale_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_dpm_config_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct dpm_config_resource *dpm_config_resource;
	struct vbios_dpm_config vb_dpm_config;
	u32 dpm_config_size;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	dpm_config_resource = kvmalloc(sizeof(*dpm_config_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(dpm_config_resource))
		return false;

	dpm_config_size = sizeof(struct vbios_dpm_config);
	memset(&vb_dpm_config, VBIOS_DATA_INVAL, dpm_config_size);
	memcpy(&vb_dpm_config, data, min(size, dpm_config_size));

	dpm_config_resource->base.link = link;
	dpm_config_resource->base.type = LOONGGPU_RESOURCE_DPM_CONFIG;

	dpm_config_resource->feature = vb_dpm_config.feature;
	dpm_config_resource->enable = vb_dpm_config.enable;
	dpm_config_resource->sclk_table = vb_dpm_config.sclk_table;

	list_add_tail(&dpm_config_resource->base.node, &vbios->resource_list);

	return true;
}

static bool vbios_create_lcd_ctrl_resource(struct loonggpu_vbios *vbios, void *data, u32 link, u32 size)
{
	struct lcd_ctrl_resource *lcd_ctrl_resource;
	struct vbios_lcd_ctrl vb_lcd_ctrl;
	u32 lcd_ctrl_size;
	u32 index;

	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(data))
		return false;

	lcd_ctrl_resource = kvmalloc(sizeof(*lcd_ctrl_resource), GFP_KERNEL);
	if (IS_ERR_OR_NULL(lcd_ctrl_resource))
		return false;

	lcd_ctrl_size = sizeof(struct vbios_lcd_ctrl);
	memset(&vb_lcd_ctrl, VBIOS_DATA_INVAL, lcd_ctrl_size);
	memcpy(&vb_lcd_ctrl, data, min(size, lcd_ctrl_size));

	lcd_ctrl_resource->base.link = link;
	lcd_ctrl_resource->base.type = LOONGGPU_RESOURCE_LCD_CTRL;

	lcd_ctrl_resource->feature = vb_lcd_ctrl.feature;
	lcd_ctrl_resource->open_sequence = vb_lcd_ctrl.open_sequence;
	lcd_ctrl_resource->close_sequence = vb_lcd_ctrl.close_sequence;
	lcd_ctrl_resource->pre_open_delay = vb_lcd_ctrl.pre_open_delay;
	lcd_ctrl_resource->pre_close_delay = vb_lcd_ctrl.pre_close_delay;
	for (index = 0; index < LCD_HW_SIGNAL_MAX; index++) {
		lcd_ctrl_resource->signal_delay[index] = vb_lcd_ctrl.signal_delay[index];
	}
	lcd_ctrl_resource->gpio_detect_open_pin = vb_lcd_ctrl.gpio_detect_open_pin;
	lcd_ctrl_resource->gpio_detect_open_timer = vb_lcd_ctrl.gpio_detect_open_timer;
	lcd_ctrl_resource->gpio_detect_open_polarity = vb_lcd_ctrl.gpio_detect_open_polarity;
	lcd_ctrl_resource->gpio_ctrl_vdd = vb_lcd_ctrl.gpio_ctrl_vdd;
	lcd_ctrl_resource->gpio_ctrl_en = vb_lcd_ctrl.gpio_ctrl_en;

	list_add_tail(&lcd_ctrl_resource->base.node, &vbios->resource_list);

	return true;
}

static struct header_resource *vbios_get_header_resource(struct loonggpu_vbios *vbios)
{
	struct resource_object *entry;
	struct header_resource *header;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if (entry->type == LOONGGPU_RESOURCE_HEADER) {
			header = container_of(entry, struct header_resource, base);
			return header;
		}
	}

	return NULL;
}

static struct crtc_resource *vbios_get_crtc_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct crtc_resource *crtc;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && entry->type == LOONGGPU_RESOURCE_CRTC) {
			crtc = container_of(entry, struct crtc_resource, base);
			return crtc;
		}
	}

	return NULL;
}

static struct encoder_resource *vbios_get_encoder_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct encoder_resource *encoder;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && entry->type == LOONGGPU_RESOURCE_ENCODER) {
			encoder = container_of(entry, struct encoder_resource, base);
			return encoder;
		}
	}

	return NULL;
}

static struct connector_resource *vbios_get_connector_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct connector_resource *connector;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && entry->type == LOONGGPU_RESOURCE_CONNECTOR) {
			connector = container_of(entry, struct connector_resource, base);
			return connector;
		}
	}

	return NULL;
}

static struct i2c_resource *vbios_get_i2c_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct i2c_resource *i2c_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_I2C)) {
			i2c_resource = container_of(entry, struct i2c_resource, base);
			return i2c_resource;
		}
	}

	return NULL;
}

static struct gpio_resource
*vbios_get_gpio_resource(struct loonggpu_vbios *vbios, u32 link)
{
	return NULL;
}

static struct pwm_resource *vbios_get_pwm_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct pwm_resource *pwm_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_PWM)) {
			pwm_resource = container_of(entry, struct pwm_resource, base);
			return pwm_resource;
		}
	}

	return NULL;
}

static struct gpu_resource *vbios_get_gpu_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct gpu_resource *gpu_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_GPU)) {
			gpu_resource = container_of(entry, struct gpu_resource, base);
			return gpu_resource;
		}
	}

	return NULL;
}

static struct ext_encoder_resources *vbios_get_ext_encoder_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct ext_encoder_resources *ext_encoder_resources;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_EXT_ENCODER)) {
			ext_encoder_resources = container_of(entry, struct ext_encoder_resources, base);
			return ext_encoder_resources;
		}
	}

	return NULL;
}

static struct backlight_resource *vbios_get_backlight_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct backlight_resource *backlight_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_BACKLIGHT)) {
			backlight_resource = container_of(entry, struct backlight_resource, base);
			return backlight_resource;
		}
	}

	return NULL;
}

static struct panel_resource *vbios_get_panel_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct panel_resource *panel_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_PANEL)) {
			panel_resource = container_of(entry, struct panel_resource, base);
			return panel_resource;
		}
	}

	return NULL;
}

static struct scale_resource *vbios_get_scale_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct scale_resource *scale_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_SCALE)) {
			scale_resource = container_of(entry, struct scale_resource, base);
			return scale_resource;
		}
	}

	return NULL;
}

static struct dpm_config_resource *vbios_get_dpm_config_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct dpm_config_resource *dpm_config_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_DPM_CONFIG)) {
			dpm_config_resource = container_of(entry, struct dpm_config_resource, base);
			return dpm_config_resource;
		}
	}

	return NULL;
}

static struct lcd_ctrl_resource *vbios_get_lcd_ctrl_resource(struct loonggpu_vbios *vbios, u32 link)
{
	struct resource_object *entry;
	struct lcd_ctrl_resource *lcd_ctrl_resource;

	if (IS_ERR_OR_NULL(vbios))
		return NULL;

	if (list_empty(&vbios->resource_list))
		return NULL;

	list_for_each_entry (entry, &vbios->resource_list, node) {
		if ((entry->link == link) && (entry->type == LOONGGPU_RESOURCE_LCD_CTRL)) {
			lcd_ctrl_resource = container_of(entry, struct lcd_ctrl_resource, base);
			return lcd_ctrl_resource;
		}
	}

	return NULL;
}

static struct vbios_funcs vbios_funcs = {
	.resource_pool_create = vbios_resource_pool_create,
	.resource_pool_destory = vbios_resource_pool_destory,

	.create_header_resource = vbios_create_header_resource,
	.create_crtc_resource = vbios_create_crtc_resource,
	.create_encoder_resource = vbios_create_encoder_resource,
	.create_connecor_resource = vbios_create_connector_resource,
	.create_i2c_resource = vbios_create_i2c_resource,
	.create_gpio_resource = vbios_create_gpio_resource,
	.create_pwm_resource = vbios_create_pwm_resource,
	.create_gpu_resource = vbios_create_gpu_resource,
	.create_ext_encoder_resource = vbios_create_ext_encoder_resource,
	.create_backlight_resource = vbios_create_backlight,
	.create_panel_resource = vbios_create_panel_resource,
	.create_lcd_ctrl_resource = vbios_create_lcd_ctrl_resource,
	.create_scale_resource = vbios_create_scale_resource,
	.create_dpm_config_resource = vbios_create_dpm_config_resource,

	.get_header_resource = vbios_get_header_resource,
	.get_crtc_resource = vbios_get_crtc_resource,
	.get_encoder_resource = vbios_get_encoder_resource,
	.get_connector_resource = vbios_get_connector_resource,
	.get_i2c_resource = vbios_get_i2c_resource,
	.get_gpio_resource = vbios_get_gpio_resource,
	.get_pwm_resource = vbios_get_pwm_resource,
	.get_gpu_resource = vbios_get_gpu_resource,
	.get_ext_encoder_resource = vbios_get_ext_encoder_resource,
	.get_backlight_resource = vbios_get_backlight_resource,
	.get_panel_resource = vbios_get_panel_resource,
	.get_lcd_ctrl_resource = vbios_get_lcd_ctrl_resource,
	.get_scale_resouce = vbios_get_scale_resource,
	.get_dpm_config_resouce = vbios_get_dpm_config_resource,
};

u8 loonggpu_vbios_checksum(const u8 *data, int size)
{
	u8 sum = 0;

	while (size--)
		sum += *data++;
	return sum;
}

u32 loonggpu_vbios_version(struct loonggpu_vbios *vbios)
{
	struct vbios_info *vb_info = vbios->vbios_ptr;
	u32 minor, major, version;

	major = vb_info->version_major;
	minor = vb_info->version_minor;
	version = major * 10 + minor;

	return version;
}

static bool dc_vbios_create(struct loonggpu_vbios *vbios)
{
	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(vbios->funcs))
		return false;

	if (vbios->funcs->resource_pool_create)
		return vbios->funcs->resource_pool_create(vbios);

	return false;
}

void *dc_get_vbios_resource(struct loonggpu_vbios *vbios, u32 link,
			    enum resource_type type)
{
	if (IS_ERR_OR_NULL(vbios) || IS_ERR_OR_NULL(vbios->funcs)) {
		DRM_ERROR("LOONGGPU get vbios resource%d failed\n", type);
		return NULL;
	}

	switch (type) {
	case LOONGGPU_RESOURCE_HEADER:
		if (vbios->funcs->get_header_resource)
			return (void *)vbios->funcs->get_header_resource(vbios);
		break;
	case LOONGGPU_RESOURCE_CRTC:
		if (vbios->funcs->get_crtc_resource)
			return (void *)vbios->funcs->get_crtc_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_ENCODER:
		if (vbios->funcs->get_encoder_resource)
			return (void *)vbios->funcs->get_encoder_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_CONNECTOR:
		if (vbios->funcs->get_connector_resource)
			return (void *)vbios->funcs->get_connector_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_GPIO:
		if (vbios->funcs->get_gpio_resource)
			return (void *)vbios->funcs->get_gpio_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_I2C:
		if (vbios->funcs->get_i2c_resource)
			return (void *)vbios->funcs->get_i2c_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_PWM:
		if (vbios->funcs->get_pwm_resource)
			return (void *)vbios->funcs->get_pwm_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_GPU:
		if (vbios->funcs->get_gpu_resource)
			return (void *)vbios->funcs->get_gpu_resource(vbios, 0);
		break;
	case LOONGGPU_RESOURCE_EXT_ENCODER:
		if (vbios->funcs->get_ext_encoder_resource)
			return (void *)vbios->funcs->get_ext_encoder_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_BACKLIGHT:
		if (vbios->funcs->get_backlight_resource)
			return (void *)vbios->funcs->get_backlight_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_PANEL:
		if (vbios->funcs->get_panel_resource)
			return (void *)vbios->funcs->get_panel_resource(vbios, link);
		break;
	case LOONGGPU_RESOURCE_SCALE:
		if (vbios->funcs->get_scale_resouce)
			return (void *)vbios->funcs->get_scale_resouce(vbios, link);
		break;
	case LOONGGPU_RESOURCE_DPM_CONFIG:
		if (vbios->funcs->get_dpm_config_resouce)
			return (void *)vbios->funcs->get_dpm_config_resouce(vbios, link);
		break;
	case LOONGGPU_RESOURCE_LCD_CTRL:
		if (vbios->funcs->get_lcd_ctrl_resource)
			return (void *)vbios->funcs->get_lcd_ctrl_resource(vbios, link);
		break;
	default:
		return NULL;
		break;
	}

	return NULL;
}

void dc_vbios_show(struct loonggpu_device *adev, struct loonggpu_vbios *vbios)
{
	struct header_resource *header_res = NULL;
	struct crtc_resource *crtc_res;
	struct gpu_resource *gpu_res;
	struct lcd_ctrl_resource *lcd_ctrl_res;
	int i;

	header_res = dc_get_vbios_resource(vbios, 0, LOONGGPU_RESOURCE_HEADER);
	if (header_res == NULL)
		return;

	DRM_INFO("LOONGGPU vbios header info:\n");
	DRM_INFO("ver:%d.%d links:%d max_planes:%d name:%s board:%s\n",
		header_res->ver_majro, header_res->ver_minor, header_res->links,
		header_res->max_planes, header_res->name, header_res->board);
	DRM_INFO("oem-vendor:%s oem-product:%s\n", header_res->oem_vendor,
		 header_res->oem_product);

	for (i = 0; i < header_res->links; i++) {
		crtc_res = dc_get_vbios_resource(vbios, i, LOONGGPU_RESOURCE_CRTC);
		DRM_INFO("LOONGGPU vbios crtc-%d max_frep:%d width:%d height:%d\n",
			 i, crtc_res->max_freq,
			 crtc_res->max_width, crtc_res->max_height);
	}

	gpu_res = dc_get_vbios_resource(vbios, 0, LOONGGPU_RESOURCE_GPU);
	if (!gpu_res) {
		adev->gmc.vram_type = DDR4;
		adev->gmc.vram_width = 32;
		DRM_WARN("The video memory and gpu information is not obtained from the vbios! \n");
	} else {
		if (gpu_res->vram_type >= VRAM_UNKNOWN)
			gpu_res->vram_type = VRAM_UNKNOWN;

		dev_info(vbios->dc->adev->dev, "VRAM: %dM %s %dbit %dMhz.\n",
			gpu_res->cap,
			loonggpu_vram_type_names[gpu_res->vram_type],
			gpu_res->bit_width, gpu_res->freq);
		dev_info(vbios->dc->adev->dev, "LOONGGPU: shaders_num: %d, shaders_freq: %d, freq_count: %d.\n",
			gpu_res->shaders_num, gpu_res->shaders_freq, gpu_res->count_freq);

		adev->gmc.vram_type = gpu_res->vram_type;
		adev->gmc.vram_width = gpu_res->bit_width;
	}

	for (i = 0; i < header_res->links; i++) {
		lcd_ctrl_res = dc_get_vbios_resource(vbios, i, LOONGGPU_RESOURCE_LCD_CTRL);
		if (lcd_ctrl_res) {
			DRM_INFO("Vbios lcd ctrl_%d open detect gpio: func:%s pin:%d timer:%dms polarity:%s\n", i,
					lcd_ctrl_res->gpio_detect_open_pin ? "enable": "disable",
					lcd_ctrl_res->gpio_detect_open_pin, lcd_ctrl_res->gpio_detect_open_timer,
					lcd_ctrl_res->gpio_detect_open_polarity ? "high": "low");
			DRM_INFO("Vbios lcd ctrl_%d gpio pin: vdd_pin:%d en_pin:%d\n", i, lcd_ctrl_res->gpio_ctrl_vdd,
					lcd_ctrl_res->gpio_ctrl_en);
			DRM_INFO("Vbios lcd ctrl_%d open signal: %d->%s:%d->%s:%d->%s:%d->%s:%d\n", i,
					lcd_ctrl_res->pre_open_delay,
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 0)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 0)], LCD_CTRL_ACTION_OPEN),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 1)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 1)], LCD_CTRL_ACTION_OPEN),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 2)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 2)], LCD_CTRL_ACTION_OPEN),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 3)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->open_sequence, 3)], LCD_CTRL_ACTION_OPEN));
			DRM_INFO("Vbios lcd ctrl_%d close signal: %d->%s:%d->%s:%d->%s:%d->%s:%d\n", i,
					lcd_ctrl_res->pre_open_delay,
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 0)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 0)], LCD_CTRL_ACTION_CLOSE),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 1)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 1)], LCD_CTRL_ACTION_CLOSE),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 2)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 2)], LCD_CTRL_ACTION_CLOSE),
					lcd_sig_to_str(LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 3)),
					LCD_CTRL_DELAY_GET(lcd_ctrl_res->signal_delay[LCD_CTRL_SIGNAL_GET(lcd_ctrl_res->close_sequence, 3)], LCD_CTRL_ACTION_CLOSE));
		}
	}
}

static ssize_t gpu_info_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct gpu_resource *gpu_res = NULL;
	struct loonggpu_device *adev;
	const char *type_name, *type_src;
	const char *vram_src, *size_src, *gpu_freq_src;
	const char *gpu_family;
	u64 size_mb;
	u32 vram_freq, vram_width, gpu_freq, cores;
	bool is_no_gpu, is_7a2000;
	int len = 0;

	if (!ddev)
		return -ENODEV;

	adev = ddev->dev_private;
	if (!adev)
		return -ENODEV;

	is_no_gpu = adev->family_type == CHIP_NO_GPU;
	is_7a2000 = adev->chip == dev_7a2000;

	vram_src = size_src = gpu_freq_src =
		 type_src = "";
	size_mb = adev->gmc.real_vram_size >> 20;
	vram_freq = adev->clock.default_mclk / 100;
	vram_width = adev->gmc.vram_width;
	type_name = loonggpu_vram_type_names[adev->gmc.vram_type];
	gpu_freq = adev->clock.default_sclk / 100;
	cores = adev->gfx.cu_info.number;
	gpu_family = loonggpu_family_name[adev->family_type];

	if (adev->dc && adev->dc->vbios) {
		gpu_res = dc_get_vbios_resource(adev->dc->vbios, 0,
						LOONGGPU_RESOURCE_GPU);
		if (gpu_res) {
			vram_src = gpu_freq_src =
				type_src = "*";
			vram_freq = gpu_res->freq;
			vram_width = gpu_res->bit_width;
			gpu_freq = gpu_res->shaders_freq;

			if (is_7a2000) {
				size_src = "*";
				size_mb = gpu_res->cap;
			}
		}
	}

	if (is_no_gpu) {
		gpu_family = "NO_GPU";
		gpu_freq_src = "";
		gpu_freq = 0;
		cores = 0;
	}

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "GPU family        : %s\n", gpu_family);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "Device ID         : 0x%04x\n", adev->pdev->device);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "VRAM size         : %llu MB %s\n",
			 (unsigned long long)size_mb, size_src);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "VRAM type         : %s %s\n", type_name, type_src);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "VRAM width        : %u bit %s\n",
			 vram_width, vram_src);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "VRAM frequency    : %u MHz %s\n",
			 vram_freq, vram_src);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "GPU max frequency : %u MHz %s\n",
			 gpu_freq, gpu_freq_src);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "GPU ALU32         : %u\n", cores * 64);
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "GPU ALU64         : %u\n", cores * 4);

	return len;
}

static DEVICE_ATTR_RO(gpu_info);

int loonggpu_gpu_info_sysfs_init(struct loonggpu_device *adev)
{
	return device_create_file(adev->ddev->dev,
				  &dev_attr_gpu_info);
}

void loonggpu_gpu_info_sysfs_exit(struct loonggpu_device *adev)
{
	device_remove_file(adev->ddev->dev,
			   &dev_attr_gpu_info);
}

static bool dc_vbios_default(struct loonggpu_vbios *vbios)
{
	struct header_resource *header;
	struct crtc_resource *crtc_0;
	struct encoder_resource *encoder_0;
	struct connector_resource *connector_0;
	struct crtc_resource *crtc_1;
	struct encoder_resource *encoder_1;
	struct connector_resource *connector_1;
	struct crtc_resource *crtc_2;
	struct encoder_resource *encoder_2;
	struct connector_resource *connector_2;
	struct crtc_resource *crtc_3;
	struct encoder_resource *encoder_3;
	struct connector_resource *connector_3;
	struct gpu_resource *gpu_resource;

	DRM_INFO("Add vbios default for 9A1000\n");

	header = kvmalloc(sizeof(*header), GFP_KERNEL);
	memcpy(header->oem_product, "LS9A1000", 32);
	memcpy(header->oem_vendor, "LOONGGPU", 32);
	memcpy(header->name, "Loongson-VBIOS", 16);
	header->base.type = LOONGGPU_RESOURCE_HEADER;
	header->links = 4;
	header->max_planes = 4;
	header->ver_majro = 0;
	header->ver_minor = 5;
	list_add_tail(&header->base.node, &vbios->resource_list);

	crtc_0 = kvmalloc(sizeof(*crtc_0), GFP_KERNEL);
	crtc_0->base.link = 0;
	crtc_0->base.type = LOONGGPU_RESOURCE_CRTC;
	crtc_0->feature = 0; //feature这个变量现在没有用到,可以考虑增加接口类型、数量等信息
	crtc_0->crtc_id = 0; //video-0
	crtc_0->encoder_id = 0;
	crtc_0->max_freq = 600000;
	crtc_0->max_width = 4096;
	crtc_0->max_height = 4096;
	crtc_0->is_vb_timing = 0;
	list_add_tail(&crtc_0->base.node, &vbios->resource_list);

	connector_0 = kvmalloc(sizeof(*connector_0), GFP_KERNEL);
	connector_0->base.link = 0;
	connector_0->base.type = LOONGGPU_RESOURCE_CONNECTOR;
	connector_0->feature = 0; //可以考虑在这个变量上增加复用的接口及其类型
	connector_0->i2c_id = 0;
	connector_0->type = DRM_MODE_CONNECTOR_HDMIA;
	connector_0->hotplug = IRQ;
	connector_0->edid_method = via_encoder; //在fpga上用9A的get_modes接口
	connector_0->irq_gpio = 0;
	connector_0->gpio_placement = 0;
	list_add_tail(&connector_0->base.node, &vbios->resource_list);

	encoder_0 = kvmalloc(sizeof(*encoder_0), GFP_KERNEL);
	encoder_0->base.link = 0;
	encoder_0->base.type = LOONGGPU_RESOURCE_ENCODER;
	encoder_0->feature = 0;
	encoder_0->i2c_id = 0;
	encoder_0->connector_id = 0;
	encoder_0->type = encoder_tmds;
	encoder_0->config_type = 3;
	encoder_0->chip_addr = 0;
	encoder_0->chip = ENCODER_CHIP_ID_INTERNAL_HDMI;
	list_add_tail(&encoder_0->base.node, &vbios->resource_list);

	crtc_1 = kvmalloc(sizeof(*crtc_1), GFP_KERNEL);
	crtc_1->base.link = 1;
	crtc_1->base.type = LOONGGPU_RESOURCE_CRTC;
	crtc_1->feature = 0;
	crtc_1->crtc_id = 1;
	crtc_1->encoder_id = 1;
	crtc_1->max_freq = 600000;
	crtc_1->max_width = 4096;
	crtc_1->max_height = 4096;
	crtc_1->is_vb_timing = 0;
	list_add_tail(&crtc_1->base.node, &vbios->resource_list);

	connector_1 = kvmalloc(sizeof(*connector_1), GFP_KERNEL);
	connector_1->base.link = 1;
	connector_1->base.type = LOONGGPU_RESOURCE_CONNECTOR;
	connector_1->feature = 0; //DP-0
	connector_1->i2c_id = 1;
	connector_1->type = DRM_MODE_CONNECTOR_DisplayPort;
	connector_1->hotplug = IRQ;
	connector_1->edid_method = via_encoder; //在fpga上用9A的get_modes接口
	connector_1->irq_gpio = 0;
	connector_1->gpio_placement = 0;
	list_add_tail(&connector_1->base.node, &vbios->resource_list);

	encoder_1 = kvmalloc(sizeof(*encoder_1), GFP_KERNEL);
	encoder_1->base.link = 1;
	encoder_1->base.type = LOONGGPU_RESOURCE_ENCODER;
	encoder_1->feature = 0;
	encoder_1->i2c_id = 1;
	encoder_1->connector_id = 1;
	encoder_1->type = encoder_tmds;
	encoder_1->config_type = 3;
	encoder_1->chip_addr = 0;
	encoder_1->chip = ENCODER_CHIP_ID_INTERNAL_DP;
	list_add_tail(&encoder_1->base.node, &vbios->resource_list);

	crtc_2 = kvmalloc(sizeof(*crtc_2), GFP_KERNEL);
	crtc_2->base.link = 2;
	crtc_2->base.type = LOONGGPU_RESOURCE_CRTC;
	crtc_2->feature = 0;
	crtc_2->crtc_id = 2;
	crtc_2->encoder_id = 2;
	crtc_2->max_freq = 600000;
	crtc_2->max_width = 4096;
	crtc_2->max_height = 4096;
	crtc_2->is_vb_timing = 0;
	list_add_tail(&crtc_2->base.node, &vbios->resource_list);

	connector_2 = kvmalloc(sizeof(*connector_2), GFP_KERNEL);
	connector_2->base.link = 2;
	connector_2->base.type = LOONGGPU_RESOURCE_CONNECTOR;
	connector_2->feature = 1; //HDMI-1
	connector_2->i2c_id = 2;
	connector_2->type = DRM_MODE_CONNECTOR_HDMIA;
	connector_2->hotplug = IRQ;
	connector_2->edid_method = via_encoder;
	connector_2->irq_gpio = 0;
	connector_2->gpio_placement = 0;
	list_add_tail(&connector_2->base.node, &vbios->resource_list);

	encoder_2 = kvmalloc(sizeof(*encoder_2), GFP_KERNEL);
	encoder_2->base.link = 2;
	encoder_2->base.type = LOONGGPU_RESOURCE_ENCODER;
	encoder_2->feature = 0;
	encoder_2->i2c_id = 2;
	encoder_2->connector_id = 2;
	encoder_2->type = encoder_tmds;
	encoder_2->config_type = 3;
	encoder_2->chip_addr = 0;
	encoder_2->chip = ENCODER_CHIP_ID_INTERNAL_HDMI;
	list_add_tail(&encoder_2->base.node, &vbios->resource_list);

	crtc_3 = kvmalloc(sizeof(*crtc_3), GFP_KERNEL);
	crtc_3->base.link = 3;
	crtc_3->base.type = LOONGGPU_RESOURCE_CRTC;
	crtc_3->feature = 0;
	crtc_3->crtc_id = 3;
	crtc_3->encoder_id = 3;
	crtc_3->max_freq = 600000;
	crtc_3->max_width = 4096;
	crtc_3->max_height = 4096;
	crtc_3->is_vb_timing = 0;
	list_add_tail(&crtc_3->base.node, &vbios->resource_list);

	connector_3 = kvmalloc(sizeof(*connector_3), GFP_KERNEL);
	connector_3->base.link = 3;
	connector_3->base.type = LOONGGPU_RESOURCE_CONNECTOR;
	connector_3->feature = 1; //DP-1
	connector_3->i2c_id = 3;
	connector_3->type = DRM_MODE_CONNECTOR_DisplayPort;
	connector_3->hotplug = IRQ;
	connector_3->edid_method = via_encoder;
	connector_3->irq_gpio = 0;
	connector_3->gpio_placement = 0;
	list_add_tail(&connector_3->base.node, &vbios->resource_list);

	encoder_3 = kvmalloc(sizeof(*encoder_3), GFP_KERNEL);
	encoder_3->base.link = 3;
	encoder_3->base.type = LOONGGPU_RESOURCE_ENCODER;
	encoder_3->feature = 0;
	encoder_3->i2c_id = 3;
	encoder_3->connector_id = 3;
	encoder_3->type = encoder_tmds;
	encoder_3->config_type = 3;
	encoder_3->chip_addr = 0;
	encoder_3->chip = ENCODER_CHIP_ID_INTERNAL_DP;
	list_add_tail(&encoder_3->base.node, &vbios->resource_list);

	gpu_resource = kvmalloc(sizeof(*gpu_resource), GFP_KERNEL);
	gpu_resource->base.link = 0;
	gpu_resource->base.type = LOONGGPU_RESOURCE_GPU;
	gpu_resource->vram_type = DDR5;
	gpu_resource->bit_width = 512;
	gpu_resource->cap = 4096;
	gpu_resource->count_freq = 1000;
	gpu_resource->freq = 1000;
	gpu_resource->shaders_num = 4096;
	gpu_resource->shaders_freq = 1500;
	list_add_tail(&gpu_resource->base.node, &vbios->resource_list);

	return true;
}

bool dc_vbios_init(struct loonggpu_dc *dc)
{
	struct vbios_info *header;
	bool status;
	bool ret;

	if (IS_ERR_OR_NULL(dc))
		return false;

	dc->vbios = kmalloc(sizeof(*dc->vbios), GFP_KERNEL);
	if (IS_ERR_OR_NULL(dc->vbios))
		return false;

	dc->vbios->funcs = &vbios_funcs;
	dc->vbios->dc = dc;
	INIT_LIST_HEAD(&dc->vbios->resource_list);

	status = get_vbios_data(dc);
	if (!status) {
		DRM_ERROR("LOONGGPU Can not get vbios from sysconf!!!\n");
	} else {
		header = dc->vbios->vbios_ptr;
	}

	if (dc->adev->chip == dev_9a1000) {
		header = dc->vbios->vbios_ptr;
		ret = dc_vbios_default(dc->vbios);
	} else
		ret = dc_vbios_create(dc->vbios);
	if (ret == false) {
		pr_err("%s %d failed \n", __func__, __LINE__);
		kvfree(dc->vbios);
		dc->vbios = NULL;
	}

	dc_vbios_show(dc->adev, dc->vbios);

	return true;
}

void dc_vbios_exit(struct loonggpu_vbios *vbios)
{
	if (IS_ERR_OR_NULL(vbios))
		return;

	if (!IS_ERR_OR_NULL(vbios->vbios_ptr)) {
		kvfree(vbios->vbios_ptr);
		vbios->vbios_ptr = NULL;
	}

	if (!IS_ERR_OR_NULL(vbios->funcs) &&
	    (vbios->funcs->resource_pool_destory))
		vbios->funcs->resource_pool_destory(vbios);

	kvfree(vbios);
	vbios = NULL;
}

bool check_vbios_info(void)
{
	void *data;
	void *vaddr;
	u8 *vbios_ptr;
	u32 encoder_size;
	u64 vbios_addr;
	acpi_size tbl_size;
	bool support = false;
	bool get_vbios = false;
	enum desc_type desc_type;
	resource_size_t vram_base;
	resource_size_t vram_size = 0;
	struct vbios_desc *start;
	struct vbios_desc *desc;
	struct vbios_header *vb_header;
	struct vbios_encoder vb_encoder;
	struct acpi_table_header *hdr;
	struct acpi_viat_table *viat;

	struct pci_dev *pdev = pci_get_device(0x0014, 0x7A25, NULL);
	if (!pdev)
		pdev = pci_get_device(0x0014, 0x7A35, NULL);

	if (!pdev)
		pdev = pci_get_device(0x0014, 0x7A15, NULL);

	/* TODO: 9a1000 vbios is not supported now */
	if (!pdev) {
		pdev = pci_get_device(0x0014, 0x9A10, NULL);
		if (pdev)
			return true;
	}

	if (!pdev) {
		pdev = pci_get_device(0x0014, 0x7A46, NULL);
		if (!pdev)
			goto acpi;

		vram_base = (unsigned long)loonggpu_get_vram_info(&pdev->dev, (unsigned long *)(&vram_size));
		if (vram_base == 0 || vram_size == 0)
			goto acpi;
	} else {
		support = pci_enable_device(pdev);
		if (support != 0) {
			DRM_ERROR("Enable loonggpu device error!\n");
			return support;
		}
		vram_base = pci_resource_start(pdev, 2);
		vram_size = pci_resource_len(pdev, 2);
	}
	vbios_addr = vram_base + vram_size - VBIOS_OFFSET;
	vaddr = ioremap(vbios_addr, VBIOS_SIZE);
	if (!vaddr)
		goto acpi;

	vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
	if (!vbios_ptr) {
		iounmap(vaddr);
		goto acpi;
	}

	memcpy(vbios_ptr, vaddr, VBIOS_SIZE);
	iounmap(vaddr);
	if (!is_valid_vbios((void *)vbios_ptr)) {
		kfree(vbios_ptr);
		get_vbios = false;
	} else
		get_vbios = true;

acpi:
	if (!get_vbios) {
#ifdef CONFIG_ACPI
		if (!ACPI_SUCCESS(acpi_get_table("VIAT", 1, &hdr)))
			goto sysconf;

		tbl_size = hdr->length;
		if (tbl_size != sizeof(struct acpi_viat_table))
			goto sysconf;

		viat = (struct acpi_viat_table *)hdr;
		vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
		if (!vbios_ptr)
			goto sysconf;

		if (!pfn_valid(PFN_DOWN(viat->vbios_addr))) {
			kfree(vbios_ptr);
			goto sysconf;
		}

		vaddr = phys_to_virt(viat->vbios_addr);
		memcpy(vbios_ptr, vaddr, VBIOS_SIZE);

		DRM_DEBUG_DRIVER("Get vbios from ACPI success!\n");
		get_vbios = true;
#else
		get_vbios = false;
#endif
	}

sysconf:
	if (!get_vbios) {
	#if defined(LG_LOONGSON_SYS_CONF_HAS_VGABIOS_ADDR)
		if (!loongson_sysconf.vgabios_addr)
			return false;
		else if(!virt_addr_valid((void *)loongson_sysconf.vgabios_addr))
			return false;
	#else
		return false;
	#endif

		vbios_ptr = kmalloc(VBIOS_SIZE, GFP_KERNEL);
		if (!vbios_ptr)
			return false;

	#if defined(LG_LOONGSON_SYS_CONF_HAS_VGABIOS_ADDR)
		memcpy(vbios_ptr, (void *)loongson_sysconf.vgabios_addr,
		       VBIOS_SIZE);
	#endif
	}

	desc = (struct vbios_desc *)(vbios_ptr + 0x6000);
	vb_header = (struct vbios_header *)(vbios_ptr + desc->offset);
	start = (struct vbios_desc *)(vbios_ptr + vb_header->desc_offset);
	desc = (struct vbios_desc *)(vbios_ptr + vb_header->desc_offset);
	while (1) {
		desc_type = desc->type;
		if (desc_type != desc_encoder && desc_type != desc_max) {
			desc++;
			continue;
		}

		if (desc_type == desc_max ||
		    ((desc - start) > vb_header->desc_size) ||
		    ((desc - start) > VBIOS_DESC_TOTAL))
			break;

		data = (u8 *)vbios_ptr + desc->offset;
		encoder_size = sizeof(struct vbios_encoder);
		memset(&vb_encoder, 0xff, min(desc->size, encoder_size));
		memcpy(&vb_encoder, data, min(desc->size, encoder_size));
		DRM_DEBUG_DRIVER("vbios desc type:%d encoder_chip:0x%x\n",
				 desc->type, vb_encoder.chip);

		switch (vb_encoder.chip) {
		case ENCODER_CHIP_ID_INTERNAL_DVO:
		case ENCODER_CHIP_ID_INTERNAL_HDMI:
		case ENCODER_CHIP_ID_INTERNAL_EDP:
		case ENCODER_CHIP_ID_INTERNAL_DP:
		case ENCODER_CHIP_ID_INTERNAL_COMPOSITE:
		case ENCODER_CHIP_ID_EDP_LT9721:
		case ENCODER_CHIP_ID_EDP_LT6711:
		case ENCODER_CHIP_ID_LVDS_LT8619:
		case ENCODER_CHIP_ID_EDP_NCS8805:
		case ENCODER_CHIP_ID_EDP_ICNM7601:
		case ENCODER_CHIP_ID_EDP_CS5611AQ_S:
		case ENCODER_CHIP_ID_DP_LT8718:
		case ENCODER_CHIP_ID_HDMI_LT8618:
		case ENCODER_CHIP_ID_HDMI_IT66121:
		case ENCODER_CHIP_ID_HDMI_MS7210:
		case ENCODER_CHIP_ID_VGA_ADV7125:
		case ENCODER_CHIP_ID_DVI_TFP410:
			support = true;
			break;
		default:
			kfree(vbios_ptr);
			return false;
		}

		desc++;
	}

	kfree(vbios_ptr);
	return support;
}
