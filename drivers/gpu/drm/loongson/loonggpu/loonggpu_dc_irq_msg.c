#include <linux/delay.h>
#include <drm/drm_vblank.h>
#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_ih.h"
#include "loonggpu_irq.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_dc_video.h"

static int video_irq_msg_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry)
{
	int i;
	int link;
	u32 hsync_cnt;
	u32 vsync_cnt;
	u32 irq_num;
	int video_num;
	struct loonggpu_dc_video *dc_video;

	/* video共有9种中断 */
	/* video内部中断消息一共58bit, 56-33 bit vsync_cnt, 32-9 bit hsunc_cnt, 8-0 bit是src */
	video_num = (entry->src_data[1] >> 26) & 0x3f;
	hsync_cnt = (entry->src_data[0] >> 8) | (entry->src_data[1] & 0x1);
	vsync_cnt = (entry->src_data[1] >> 1) & 0x7fffff;
	irq_num = entry->src_data[0] & 0x1ff;

	for (i = 0; i < adev->dc->links; i++) {
		dc_video = &adev->dc->link_info[i].crtc->dc_video;
		if (dc_video->video_num == video_num) {
			link = i;
			break;
		}
	}

	i = 0;
	while (irq_num) {
		if (!(irq_num & 0x1)) {
			irq_num = irq_num >> 1;
			i++;
			continue;
		}

		switch (1 << i) {
		case 0x1: /* errovl-内存带宽不足时导致显示错误溢出并不可恢复 */
			DRM_DEBUG("handle video irq errovl\n");
			break;
		case 0x2: /* hsync */
			DRM_DEBUG("handle video irq hsync\n");
			break;
		case 0x4: /* vsync */
			DRM_DEBUG("handle video vsync irq\n");
			break;
		case 0x8: /* mem acc done 表示所有图层的数据已从内存里读出 */
			dc_handle_vsync_irq(adev->mode_info.crtcs[link]);
			break;
		case 0x10: /* frame done video运算完一帧 */
			DRM_DEBUG("handle video irq frame done\n");
			break;
		case 0x20: /* cfgmiss 运行下一帧时，寄存器仍在配置状态 */
			DRM_DEBUG("handle video irq cfgmiss\n");
			break;
		case 0x40: /* cfgreq 表示可以对下一帧进行配置了 */
			DRM_DEBUG("handle video irq cfgreq\n");
			break;
		case 0x80: /* soft rst done 表示video完成了软中断 */
			complete(&dc_video->reset_complete);
			break;
		case 0x100: /* dsperr video访存带宽不足 */
			DRM_INFO("video dsperr video:%d\n", video_num);
			break;
		default:
			DRM_ERROR("Unsupport video irq num:%d !\n", irq_num);
			return -1;
		}

		irq_num = irq_num >> 1;
		i++;

		if (i > 8)
			return 0;
	}

	return 0;
}

static void dc_hotplug_work_func(struct work_struct *work)
{
	struct loonggpu_device *adev = container_of(to_delayed_work(work),
					struct loonggpu_device, hotplug_work);
	struct drm_device *dev = adev->ddev;
	struct loonggpu_dc_video *dc_video;
	unsigned long flags;
	int i;

	drm_helper_hpd_irq_event(dev);

	for (i = 0; i < adev->dc->links; i++) {
		dc_video = &adev->dc->link_info[i].crtc->dc_video;
		spin_lock_irqsave(&dc_video->hpd_lock, flags);
		if (dc_video->hdmi_irq_en) {
			dc_writel(adev,
				gdc_reg->hdmi_reg_v2[dc_video->hdmi_num].irq_en,
				dc_video->hdmi_irq_en);
			dc_video->hdmi_irq_en = 0;
		}
		if (dc_video->dp_hpd_enable) {
			dc_writel(adev,
				gdc_reg->dp_reg[dc_video->dp_num].hpd_enable,
				dc_video->dp_hpd_enable);
			dc_video->dp_hpd_enable = 0;
		}
		spin_unlock_irqrestore(&dc_video->hpd_lock, flags);
	}
}

static int hdmi_irq_msg_handler(struct loonggpu_device *adev, int hdmi_num,
				struct loonggpu_iv_entry *entry)
{
	struct loonggpu_dc_crtc *dc_crtc;
	struct loonggpu_dc_video *dc_video;
	int i, reg_val;
	/* 识别中断消息中的信息，一共4bit,0-3bit分别表示接入、拔出、启动、关闭 */
	int irq_msg_num = entry->src_data[0];
	unsigned long flags, delay;

	/* 休眠过程中 DC 即将下电，不处理热插拔 */
	if (adev->dc->cached_state)
		return 0;

	/* 获取link编号 */
	for (i = 0; i < adev->dc->links; i++) {
		if (adev->dc->link_info[i].crtc->dc_video.hdmi_num == hdmi_num) {
			dc_crtc = adev->dc->link_info[i].crtc;
			dc_video = &dc_crtc->dc_video;
			break;
		}
	}

	switch (irq_msg_num) {
	case 0x1:
	case 0x2:
		spin_lock_irqsave(&dc_video->hpd_lock, flags);
		reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].irq_en);
		if (reg_val & 0x3) {
			dc_video->hdmi_irq_en = reg_val;
			dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].irq_en,
				reg_val & ~0x3);
		} else {
			spin_unlock_irqrestore(&dc_video->hpd_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&dc_video->hpd_lock, flags);

		delay = (irq_msg_num & 0x1) ? msecs_to_jiffies(20) : 0;
		mod_delayed_work(system_wq, &adev->hotplug_work, delay);
		break;
	case 0x4:
	case 0x8:
		/* 启动或关闭的中断处理流程 */
		break;
	default:
		break;
	}

	return 0;
}

static int dp_irq_msg_handler(struct loonggpu_device *adev, int dp_num,
			      struct loonggpu_iv_entry *entry)
{
	struct loonggpu_dc_crtc *dc_crtc;
	struct loonggpu_dc_video *dc_video;
	int i, reg_val;
	/* 识别中断消息中的信息，一共4bit,0-3bit分别表示接入、拔出、启动、关闭 */
	int irq_msg_num = entry->src_data[0];
	unsigned long flags, delay;

	/* 休眠过程中 DC 即将下电，不处理热插拔 */
	if (adev->dc->cached_state)
		return 0;

	/* 获取link编号 */
	for (i = 0; i < adev->dc->links; i++) {
		if (adev->dc->link_info[i].crtc->dc_video.dp_num == dp_num) {
			dc_crtc = adev->dc->link_info[i].crtc;
			dc_video = &dc_crtc->dc_video;
			break;
		}
	}

	switch (irq_msg_num) {
	case 0x1:
	case 0x2:
		spin_lock_irqsave(&dc_video->hpd_lock, flags);
		reg_val = dc_readl(adev, gdc_reg->dp_reg[dp_num].hpd_enable);
		if (reg_val & 0x3) {
			dc_video->dp_hpd_enable = reg_val;
			dc_writel(adev, gdc_reg->dp_reg[dp_num].hpd_enable,
				reg_val & ~0x3);
		} else {
			spin_unlock_irqrestore(&dc_video->hpd_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&dc_video->hpd_lock, flags);

		delay = (irq_msg_num & 0x1) ? msecs_to_jiffies(20) : 0;
		mod_delayed_work(system_wq, &adev->hotplug_work, delay);
		break;
	case 0x4:
	case 0x8:
		/* 启动或关闭的中断处理流程 */
		break;
	default:
		break;
	}

	return 0;
}

static int vga_irq_msg_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry)
{
	int reg_val;
	int i;
	struct loonggpu_dc_video *dc_video;
	struct loonggpu_link_info *link_info = adev->dc->link_info;

	for (i = 0; i < adev->dc->links; i++) {
		if (link_info[i].crtc->dc_video.has_vga)
			dc_video = &link_info[i].crtc->dc_video;
	}

	/* 区分当前中断是接入中断还是拔出中断 */
	reg_val = dc_readl(adev, gdc_reg->vga_reg.vga_cfg);
	if ((reg_val & DC_VGA_HPD_STATUS_MASK) == 1) {
		/* 更改对应video中的vga连接状态变量 */
		dc_video->vga_connected = connector_status_connected;
		reg_val &= ~0x3;
		reg_val |= 0x2; /* 改为拔出检测 */
	} else if ((reg_val & DC_VGA_HPD_STATUS_MASK) == 2) {
		dc_video->vga_connected = connector_status_disconnected;
		reg_val &= ~0x3;
		reg_val |= 0x1; /* 改为接入检测 */
	} else {
		dc_video->vga_connected = connector_status_disconnected;
		dc_writel(adev, gdc_reg->vga_reg.vga_cfg, 0x0);
		DRM_ERROR("Error VGA HPD status\n");
		return false;
	}
	dc_writel(adev, gdc_reg->vga_reg.vga_cfg, reg_val);

	/* 发送热插拔事件 */
	mod_delayed_work(system_wq, &adev->hotplug_work, 0);

	return 0;
}

static int dvo_irq_msg_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry)
{
	/* 发送热插拔事件 */
	mod_delayed_work(system_wq, &adev->hotplug_work, 0);

	return 0;
}

static int wb_irq_msg_handler(struct loonggpu_device *adev, struct loonggpu_iv_entry *entry)
{
	DRM_INFO("unsupport wb irq!\n");
	return 0;
}

/* 该函数用于处理全部DC的中断消息，识别对应模块后分发中断消息至DC内部各个模块的中断处理函数 */
static int dc_irq_msg_handler(struct loonggpu_device *adev,
			      struct loonggpu_irq_src *source,
			      struct loonggpu_iv_entry *entry)
{
	u32 irq_msg_id; /* 区分中断来自哪一个DC的子模块,一共14个可产生中断的子模块 */

	/* entry->src_id为0x5时表示9A DC发出中断
	   DC中断消息一共84bit, 83-72 bit全是0，71-64 bit msgtype(标识中断消息来自DC,默认全0)
	   63-0 bit 为DC内部中断消息，其中63-58 bit为子模块编号，57-0 bit为各子模块自定义
	   中断消息不需要手动清除中断,硬件发出中断消息后自动清除中断状态 */

	irq_msg_id = entry->src_data[1] >> 26;

	switch (irq_msg_id) {
	case IRQ_MSG_VIDEO_0:
	case IRQ_MSG_VIDEO_1:
	case IRQ_MSG_VIDEO_2:
	case IRQ_MSG_VIDEO_3:
		video_irq_msg_handler(adev, entry);
		break;
	case IRQ_MSG_HDMI_0:
		hdmi_irq_msg_handler(adev, 0, entry);
		break;
	case IRQ_MSG_HDMI_1:
		hdmi_irq_msg_handler(adev, 1, entry);
		break;
	case IRQ_MSG_DP_0:
		dp_irq_msg_handler(adev, 0, entry);
		break;
	case IRQ_MSG_DP_1:
		dp_irq_msg_handler(adev, 1, entry);
		break;
	case IRQ_MSG_VGA:
		vga_irq_msg_handler(adev, entry);
		break;
	case IRQ_MSG_DVO:
		dvo_irq_msg_handler(adev, entry);
		break;
	case IRQ_MSG_WB_0:
	case IRQ_MSG_WB_1:
	case IRQ_MSG_WB_2:
	case IRQ_MSG_WB_3:
		wb_irq_msg_handler(adev, entry);
		break;
	default:
		DRM_ERROR("unsupport irq msg id:%d !\n", irq_msg_id);
		break;
	}

	return 0;
}

static int video_irq_msg_enable(struct loonggpu_device *adev, int mod_num, int irq_num, bool enable)
{
	u32 reg_val;
	reg_val = dc_readl(adev, gdc_reg->video_reg[mod_num].intinfo);

	/* 清除中断使能状态 */
	reg_val &= ~(1 << irq_num);
	reg_val &= ~(1 << (irq_num + 22));

	if (irq_num == VIDEO_IRQ_SOFT_RST_DONE) {
		reg_val &= ~(enable << irq_num);
		reg_val |= enable << (irq_num + 22);
	} else
		reg_val |= enable << irq_num | enable << (irq_num + 22);

	dc_writel(adev, gdc_reg->video_reg[mod_num].intinfo, reg_val);

	return 0;
}

static void hdmi_irq_msg_enable(struct loonggpu_device *adev, int hdmi_num,
				int irq_num, bool enable)
{
	int reg_val = dc_readl(adev, gdc_reg->hdmi_reg_v2[hdmi_num].irq_en);

	/* bit-0是接入中断，bit-1是拔出中断 */
	/* bit-2是hdmi开启中断，bit-3是hdmi关闭中断,bit-31使能中断消息 */
	if (enable)
		reg_val = (1 << 31) | 0x3;
	else {
		reg_val &= ~(1 << 31);
		reg_val &= ~0xf;
	}

	dc_writel(adev, gdc_reg->hdmi_reg_v2[hdmi_num].irq_en, reg_val);
}

static void dp_irq_msg_enable(struct loonggpu_device *adev, int dp_num,
				int irq_num, bool enable)
{
	int reg_val = dc_readl(adev, gdc_reg->dp_reg[dp_num].hpd_enable);

	/* bit-0是接入中断，bit-1是拔出中断 */
	/* bit-2是dp开启中断，bit-3是dp关闭中断,bit-31使能中断消息 */
	if (enable)
		reg_val = (1 << 31) | 0x3;
	else {
		reg_val &= ~(1 << 31);
		reg_val &= ~0xf;
	}

	dc_writel(adev, gdc_reg->dp_reg[dp_num].hpd_enable, reg_val);
}

static void vga_irq_msg_enable(struct loonggpu_device *adev, bool enable)
{
	int reg_val = dc_readl(adev, gdc_reg->vga_reg.vga_cfg);

	reg_val &= ~0x3;
	if (enable)
		reg_val |= 0x1; /* 使能VGA接入检测 */

	dc_writel(adev, gdc_reg->vga_reg.vga_cfg, reg_val);
}

static void dvo_irq_msg_enable(struct loonggpu_device *adev, bool enable)
{
	/* DVO硬件默认使用中断消息模式,0x8寄存器bit-1默认为0,该位为1时使用中断线 */
	int reg_val = dc_readl(adev, gdc_reg->dvo_reg.dvo_cfg);

	if (enable)
		reg_val |= (1 << 2); /* 使能DVO热插拔中断 */
	else
		reg_val &= ~(1 << 2); /* 关闭DVO热插拔中断 */

	dc_writel(adev, gdc_reg->dvo_reg.dvo_cfg, reg_val);
}

int get_irq_msg_type(int mod_num, int irq_num)
{
	return (irq_num << 8) | (mod_num & 0xff);
}

/* type 的0-7位是子模块编号,8-31位是中断编号 */
static int dc_set_irq_msg_state(struct loonggpu_device *adev,
			 struct loonggpu_irq_src *src,
			 unsigned type,
			 enum loonggpu_interrupt_state state)
{
	int mod_num = type & 0xff;
	int irq_num = type >> 8;

	switch (mod_num) {
	case IRQ_MSG_VIDEO_0:
	case IRQ_MSG_VIDEO_1:
	case IRQ_MSG_VIDEO_2:
	case IRQ_MSG_VIDEO_3:
		video_irq_msg_enable(adev, mod_num, irq_num, state);
		break;
	case IRQ_MSG_HDMI_0:
		hdmi_irq_msg_enable(adev, 0, irq_num, state);
		break;
	case IRQ_MSG_HDMI_1:
		hdmi_irq_msg_enable(adev, 1, irq_num, state);
		break;
	case IRQ_MSG_DP_0:
		dp_irq_msg_enable(adev, 0, irq_num, state);
		break;
	case IRQ_MSG_DP_1:
		dp_irq_msg_enable(adev, 1, irq_num, state);
		break;
	case IRQ_MSG_VGA:
		vga_irq_msg_enable(adev, state);
		break;
	case IRQ_MSG_DVO:
		dvo_irq_msg_enable(adev, state);
		break;
	case IRQ_MSG_WB_0:
	case IRQ_MSG_WB_1:
	case IRQ_MSG_WB_2:
	case IRQ_MSG_WB_3:
		DRM_INFO("unsupport wb irq id:%d !\n", mod_num);
		break;
	default:
		DRM_ERROR("unsupport irq msg id:%d !\n", mod_num);
		break;
	}

	return 0;
}

static int dc_register_irq_msg_handlers(struct loonggpu_device *adev)
{
	int ret;

	ret = loonggpu_irq_add_id(adev, LOONGGPU_IH_CLIENTID_LEGACY, 0x5, &adev->dc_irq_msg);
	if (ret) {
		DRM_ERROR("Failed to add dc irq msg!!\n");
		return ret;
	}

	return 0;
}

static const struct loonggpu_irq_src_funcs dc_irq_msg_funcs = {
	.set = dc_set_irq_msg_state,
	.process = dc_irq_msg_handler,
};

static void dc_set_irq_msg_funcs(struct loonggpu_device *adev)
{
	adev->dc_irq_msg.num_types = 0x9ff;
	adev->dc_irq_msg.funcs = &dc_irq_msg_funcs;
}

static void vga_irq_init(struct loonggpu_device *adev)
{
	int i;
	int reg_val;
	struct loonggpu_link_info *link_info = adev->dc->link_info;

	/* 初始化VGA接口的连接状态为unknow */
	for (i = 0; i < adev->dc->links; i++) {
		if (link_info[i].crtc->dc_video.has_vga)
			link_info[i].crtc->dc_video.vga_connected = connector_status_unknown;
	}

	/* 无论初始状态是连接或拔出均要触发一次中断，获取连接状态并更改寄存器探测状态
	   VGA,默认使用中断消息模式，默认配置为接入探测 */
	reg_val = dc_readl(adev, gdc_reg->vga_reg.vga_cfg);
	reg_val &= ~0x3;
	/* 第0位使能VGA接入检测 */
	dc_writel(adev, gdc_reg->vga_reg.vga_cfg, reg_val | 0x1);
}

static int dc_irq_msg_enable(struct loonggpu_device *adev, bool enable)
{
	int i;
	unsigned type;
	int links = adev->dc->links;
	struct loonggpu_link_info *link_info = adev->dc->link_info;
	struct loonggpu_dc_video *dc_video;

	/* 使能必要的中断，如reset done、hdmi hpd、dp hpd
	   按照硬件信息使能，不存在的video或接口不要操作 */
	for (i = 0; i < links; i++) {
		dc_video = &link_info[i].crtc->dc_video;

		type = get_irq_msg_type(dc_video->video_num, VIDEO_IRQ_SOFT_RST_DONE);
		dc_set_irq_msg_state(adev, &adev->dc_irq_msg, type, enable);

		type = get_irq_msg_type(dc_video->video_num, VIDEO_IRQ_DSP_ERR);
		dc_set_irq_msg_state(adev, &adev->dc_irq_msg, type, enable);

		if (dc_video->hdmi_num == 0)
			type = get_irq_msg_type(DC_MOD_HDMI_0, 0);
		else if (dc_video->hdmi_num == 1)
			type = get_irq_msg_type(DC_MOD_HDMI_1, 0);
		else if (dc_video->dp_num == 0)
			type = get_irq_msg_type(DC_MOD_DP_0, 0);
		else if (dc_video->dp_num == 1)
			type = get_irq_msg_type(DC_MOD_DP_1, 0);
		dc_set_irq_msg_state(adev, &adev->dc_irq_msg, type, enable);
	}

	return 0;
}

/* mem_acc_done中断用作vblank中断，以下实现开启和关闭 */
int video_set_vblank(struct drm_crtc *crtc, bool enable)
{
	int ret;
	unsigned type;
	struct loonggpu_device *adev = crtc->dev->dev_private;
	struct loonggpu_dc_crtc *dc_crtc = adev->dc->link_info[crtc->index].crtc;
	int video_num = dc_crtc->dc_video.video_num;

	type = get_irq_msg_type(video_num, VIDEO_IRQ_MEM_ACC_DONE);
	ret = dc_set_irq_msg_state(adev, &adev->dc_irq_msg, type, enable);

	return ret;
}

int dc_irq_msg_sw_init(struct loonggpu_device *adev)
{
	int r, i;

	/* max_vblank_count是根据中断消息中描述vsync计数的位长确定的 */
	adev->ddev->max_vblank_count = 0x007fffff;

	for (i = 0; i < adev->dc->links; i++)
		spin_lock_init(&adev->dc->link_info[i].crtc->dc_video.hpd_lock);

	/* 初始化HPD工作队列 */
	INIT_DELAYED_WORK(&adev->hotplug_work, dc_hotplug_work_func);

	dc_set_irq_msg_funcs(adev);

	r = dc_register_irq_msg_handlers(adev);
	if (r) {
		DRM_ERROR("DC: Failed to initialize IRQ\n");
		return r;
	}

	return 0;
}

int dc_irq_msg_hw_init(struct loonggpu_device *adev)
{
	struct loonggpu_link_info *link_info = adev->dc->link_info;
	struct loonggpu_dc_video *dc_video;
	int i;

	dc_irq_msg_enable(adev, 1);

	for (i = 0; i < adev->dc->links; i++) {
		dc_video = &link_info[i].crtc->dc_video;
		if (dc_video->has_vga)
			vga_irq_init(adev);
	}

	return 0;
}

int dc_irq_msg_hw_uninit(struct loonggpu_device *adev)
{
	cancel_delayed_work_sync(&adev->hotplug_work);
	dc_irq_msg_enable(adev, 0);
	return 0;
}

void dc_irq_msg_fini(struct loonggpu_device *adev)
{
	cancel_delayed_work_sync(&adev->hotplug_work);
}
