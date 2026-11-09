
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/reset.h>

#include "loonggpu.h"
#include "loonggpu_dc.h"
#include "loonggpu_dc_i2c.h"
#include "loonggpu_dc_hdmi.h"
#include "loonggpu_dc_reg.h"
#include "loonggpu_helper.h"

#define EIO             5       /* I/O error */
#define EINVAL          22      /* Invalid argument */
#define ETIMEDOUT       110     /* Connection timed out */
#define I2C_TIMEOUT		100000

/* Loongson I2C offset registers */
#define I2C_CR1			0x00
#define I2C_CR2			0x04
#define I2C_DR			0x10
#define I2C_SR1			0x14
#define I2C_SR2			0x18
#define I2C_CCR			0x1C
#define I2C_TRISE		0x20
#define I2C_FLTR		0x24

/* Loongson I2C control 1*/
#define I2C_CR1_POS		BIT(11)
#define I2C_CR1_ACK		BIT(10)
#define I2C_CR1_STOP		BIT(9)
#define I2C_CR1_START		BIT(8)
#define I2C_CR1_PE		BIT(0)

/* Loongson I2C control 2 */
#define I2C_CR2_FREQ_MASK	GENMASK(5, 0)
#define I2C_CR2_FREQ(n)		((n) & I2C_CR2_FREQ_MASK)
#define I2C_CR2_ITBUFEN		BIT(10)
#define I2C_CR2_ITEVTEN		BIT(9)
#define I2C_CR2_ITERREN		BIT(8)
#define I2C_CR2_IRQ_MASK	(I2C_CR2_ITBUFEN | \
				 I2C_CR2_ITEVTEN | \
				 I2C_CR2_ITERREN)

/* Loongson I2C Status 1 */
#define I2C_SR1_OVR		BIT(11)
#define I2C_SR1_AF		BIT(10)
#define I2C_SR1_ARLO		BIT(9)
#define I2C_SR1_BERR		BIT(8)
#define I2C_SR1_TXE		BIT(7)
#define I2C_SR1_RXNE		BIT(6)
#define I2C_SR1_BTF		BIT(2)
#define I2C_SR1_ADDR		BIT(1)
#define I2C_SR1_SB		BIT(0)
#define I2C_SR1_ITEVTEN_MASK	(I2C_SR1_BTF | \
				 I2C_SR1_ADDR | \
				 I2C_SR1_SB)
#define I2C_SR1_ITBUFEN_MASK	(I2C_SR1_TXE | \
				 I2C_SR1_RXNE)
#define I2C_SR1_ITERREN_MASK	(I2C_SR1_AF | \
				 I2C_SR1_ARLO | \
				 I2C_SR1_BERR)

/* Loongson I2C Status 2 */
#define I2C_SR2_BUSY		BIT(1)

/* Loongson I2C Control Clock */
#define I2C_CCR_FS		BIT(15)
#define I2C_CCR_DUTY		BIT(14)

enum ls_i2c_speed {
	LS_I2C_SPEED_STANDARD,	/* 100 kHz */
	LS_I2C_SPEED_FAST,	/* 400 kHz */
};

static inline void i2c_set_bits(void __iomem *reg, u32 mask)
{
	writel(readl(reg) | mask, reg);
}

static inline void i2c_clr_bits(void __iomem *reg, u32 mask)
{
	writel(readl(reg) & ~mask, reg);
}

/**
 * wait_on_bit_9a() - poll a register until a bit is set or cleared
 * @reg:     register address
 * @mask:    bitmask to test
 * @set:     true to wait for bit set, false for bit cleared
 *
 * Return: 0 on success, -ETIMEDOUT if bit state not reached.
 */
static int wait_on_bit_9a(struct loonggpu_dc_i2c *dc_i2c, unsigned int reg, unsigned int mask, unsigned char set)
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

	if (sr1 & I2C_SR1_BERR) {
		DRM_INFO("I2C: bus error\n");
		err = 1;
	}
	if (sr1 & I2C_SR1_ARLO) {
		DRM_INFO("I2C: arbitration lost\n");
		err = 1;
	}
	if (sr1 & I2C_SR1_AF) {
		DRM_INFO("I2C: ack failure\n");
		err = 1;
	}
	if (sr1 & I2C_SR1_OVR) {
		DRM_INFO("I2C: overrun/underrun\n");
		err = 1;
	}

	if (err)
		writel(readl(dc_i2c->reg_base + I2C_SR1) | I2C_CR1_PE | I2C_CR1_STOP, dc_i2c->reg_base + I2C_CR1);

	return err;
}

/**
 * i2c_wait_and_check() - wait for a status bit and then check errors
 * @mask: bitmask to wait for
 * @set:  true to wait for set, false for cleared
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_wait_and_check(struct loonggpu_dc_i2c *dc_i2c, unsigned int mask, unsigned char set)
{
	int ret;

	ret = wait_on_bit_9a(dc_i2c, I2C_SR1, mask, set);
	if (ret)
		return ret;

//	if (poll_errors(dc_i2c))
//		return -EIO;

	return 0;
}

/**
 * i2c_stop() - generate I2C stop condition
 */
static int i2c_stop(struct loonggpu_dc_i2c *dc_i2c)
{
	writel(I2C_CR1_PE | I2C_CR1_STOP, dc_i2c->reg_base + I2C_CR1);
	return wait_on_bit_9a(dc_i2c, I2C_CR1, I2C_CR1_STOP, false);
}

/**
 * i2c_write_byte() - write one data byte to the bus
 * @data: byte to transmit
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_write_byte(struct loonggpu_dc_i2c *dc_i2c, unsigned char data)
{
	if (i2c_wait_and_check(dc_i2c, I2C_SR1_TXE, true))
		return -EIO;

	writel(data, dc_i2c->reg_base + I2C_DR);

	return i2c_wait_and_check(dc_i2c, I2C_SR1_BTF, true);
}

static int i2c_write(struct loonggpu_dc_i2c *dc_i2c, unsigned char *buf, int count)
{
	struct priv_msg *msg = &dc_i2c->msg;
	void __iomem *reg = dc_i2c->reg_base + I2C_CR2;
	int ret;

	if (msg->count) {
		ret = i2c_write_byte(dc_i2c, *msg->buf++);
		if (ret)
			return ret;

		msg->count--;
		if (!msg->count)
			i2c_clr_bits(reg, I2C_CR2_ITBUFEN);
	} else {
		i2c_set_bits(dc_i2c->reg_base + I2C_CR1, (1 << (8 + msg->stop)));
	}

	return 0;
}

static void ls_i2c_read_msg(struct loonggpu_dc_i2c *dc_i2c)
{
	struct priv_msg *msg = &dc_i2c->msg;

	*msg->buf++ = readl(dc_i2c->reg_base + I2C_DR);
	msg->count--;
}
/**
 * i2c_read_last_byte() - receive the final byte with NACK + STOP
 * @data: output buffer for the received byte
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_read_last_byte(struct loonggpu_dc_i2c *dc_i2c, unsigned char *data)
{
	if (i2c_wait_and_check(dc_i2c, I2C_SR1_RXNE, true))
		return -EIO;

	writel(I2C_CR1_PE | I2C_CR1_STOP, dc_i2c->reg_base + I2C_CR1);
	*data = readl(dc_i2c->reg_base + I2C_DR);

	while (readl(dc_i2c->reg_base + I2C_CR1) & I2C_CR1_STOP);

	return 0;
}

static int i2c_read(struct loonggpu_dc_i2c *dc_i2c, unsigned char *buf, int count)
{
	unsigned char val;
	int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = i2c_read_last_byte(dc_i2c, &val);
		if (ret)
			return ret;
		buf[i] = val;
	}

	return 0;
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
	if (i2c_wait_and_check(dc_i2c, I2C_SR1_ADDR, true)) {
		readl(dc_i2c->reg_base + I2C_SR2);
		return -EIO;
	}

	/* Clear ADDR flag by reading SR2 */
	readl(dc_i2c->reg_base + I2C_SR2);

	return 0;
}

/**
 * i2c_start() - generate I2C start condition
 *
 * Return: 0 on success, negative errno on failure.
 */
static int i2c_start(struct loonggpu_dc_i2c *dc_i2c)
{

	writel(I2C_CR1_PE | I2C_CR1_START, dc_i2c->reg_base + I2C_CR1);

	return i2c_wait_and_check(dc_i2c, I2C_SR1_SB, true);
}

static int ls9a_i2c_doxfer(struct loonggpu_dc_i2c *dc_i2c, struct i2c_msg *msgs, int num)
{
	struct i2c_msg *msg = msgs;
	int ret;
	int i;

	for (i = 0; i < num; i++) {
		ret = i2c_start(dc_i2c);
		ret = i2c_send_addr(dc_i2c, msg->addr);
		if (ret)
			return ret;

		if (msg->flags & I2C_M_RD) {
			if (i2c_read(dc_i2c, msg->buf, msg->len) < 0)
				return -1;
		} else {
			if (i2c_write(dc_i2c, msg->buf, msg->len) < 0)
				return -1;
		}

		++msg;
		if (i2c_stop(dc_i2c) < 0)
			return -1;
	}

	if (i2c_stop(dc_i2c) < 0)
		return -1;

	return i;
}

/**
 * ls9a_i2c_xfer() - Transfer combined I2C message
 * @i2c_adap: Adapter pointer to the controller
 * @msgs: Pointer to data to be written.
 * @num: Number of messages to be executed
 */
static int ls9a_i2c_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg msgs[], int num)
{
	struct loonggpu_dc_i2c *dc_i2c = i2c_get_adapdata(i2c_adap);
	int ret = 0;
	int retry;

	for (retry = 0; retry < i2c_adap->retries; retry++) {
		ret = ls9a_i2c_doxfer(dc_i2c, msgs, num);
		if (ret != -EAGAIN)
			return ret;

		udelay(100);
	}

	return -EREMOTEIO;
}

static u32 ls9a_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ls_i2c_algo = {
	.master_xfer = ls9a_i2c_xfer,
	.functionality = ls9a_i2c_func,
};

int ls9a1000_i2c_init(struct loonggpu_device *adev, uint32_t i2c_index)
{
	struct loonggpu_dc_i2c *dc_i2c;
	int ret;

	dc_i2c = kzalloc(sizeof(struct loonggpu_dc_i2c), GFP_KERNEL);
	if (!dc_i2c)
		return -ENOMEM;

	dc_i2c->adev = adev;
	dc_i2c->i2c_num = i2c_index;
	dc_i2c->adapter.owner = THIS_MODULE;
	dc_i2c->adapter.algo = &ls_i2c_algo;
	dc_i2c->adapter.dev.parent = adev->ddev->dev;
	dc_i2c->adapter.nr = -1;
	dc_i2c->adapter.retries = 1000;
	dc_i2c->adapter.timeout = msecs_to_jiffies(200); //100

	dc_i2c->reg_base = adev->rmmio_sc + 0x2000 + 0x100 * i2c_index;
	snprintf(dc_i2c->adapter.name, sizeof(dc_i2c->adapter.name), "LS9A-I2C:%d", i2c_index);

	writel(2000, dc_i2c->reg_base + I2C_CCR);
	writel(161, dc_i2c->reg_base + I2C_TRISE);

	/* reset */
	writel(1 << 15, dc_i2c->reg_base + I2C_CR1);
	writel(0, dc_i2c->reg_base + I2C_CR1);
	writel(160, dc_i2c->reg_base + I2C_CR2);

	/* Enable I2C */
	writel(I2C_CR1_PE, dc_i2c->reg_base + I2C_CR1);

	i2c_set_adapdata(&dc_i2c->adapter, dc_i2c);
	ret = i2c_add_adapter(&dc_i2c->adapter);
	if (ret) {
		DRM_ERROR("Failed to register 9A i2c %d\n", i2c_index);
		goto out_free;
	}

	adev->i2c[i2c_index] = dc_i2c;

out_free:
	if (ret) {
		kfree(dc_i2c);
		adev->i2c[i2c_index] = NULL;
	}

	return ret;
}
