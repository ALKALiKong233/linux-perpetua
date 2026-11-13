// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW8624 RAM-mode haptics driver.
 *
 * Based on the AWINIC vendor driver and the AW8624 support in
 * Luca Weiss's aw8695-haptics driver.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define AW8624_ID			0x00
#define AW8624_CHIP_ID			0x24
#define AW8624_SOFT_RESET		0xaa

#define AW8624_SYSINT			0x02
#define AW8624_SYSINT_OV			BIT(6)
#define AW8624_SYSINT_UVLO		BIT(5)
#define AW8624_SYSINT_OCD		BIT(2)
#define AW8624_SYSINT_OT			BIT(1)
#define AW8624_PROTECTION_IRQS		(AW8624_SYSINT_OV | \
					 AW8624_SYSINT_UVLO | \
					 AW8624_SYSINT_OCD | AW8624_SYSINT_OT)
#define AW8624_SYSINTM			0x03
#define AW8624_IRQ_MASK_ALL		GENMASK(6, 0)

#define AW8624_SYSCTRL			0x04
#define AW8624_SYSCTRL_RAMINIT		BIT(5)
#define AW8624_SYSCTRL_PLAY_MODE		GENMASK(3, 2)
#define AW8624_SYSCTRL_STANDBY		BIT(0)
#define AW8624_GO			0x05
#define AW8624_GO_ENABLE			BIT(0)
#define AW8624_WAVSEQ1			0x07
#define AW8624_WAVSEQ2			0x08
#define AW8624_WAVLOOP1			0x0f
#define AW8624_WAVLOOP1_INFINITE		0xf0

#define AW8624_DBGCTRL			0x20
#define AW8624_DBGCTRL_INTN_SEL		BIT(5)
#define AW8624_DBGCTRL_INT_MODE		GENMASK(3, 2)
#define AW8624_DBGCTRL_INT_EDGE		BIT(2)
#define AW8624_BASE_ADDRH		0x21
#define AW8624_BASE_ADDRL		0x22
#define AW8624_PWMDBG			0x2e
#define AW8624_PWMDBG_MODE		GENMASK(6, 5)
#define AW8624_PWMDBG_24KHZ		BIT(6)
#define AW8624_DATDBG			0x3b
#define AW8624_GAIN_FULL			0x80
#define AW8624_RAMADDRH			0x40
#define AW8624_RAMADDRL			0x41
#define AW8624_RAMDATA			0x42
#define AW8624_GLB_STATE			0x47
#define AW8624_GLB_STATE_MASK		GENMASK(3, 0)
#define AW8624_MAX_REGISTER		0x7f

#define AW8624_RAM_BASE			0x0800
#define AW8624_RAM_HEADER_SIZE		5
#define AW8624_WAVE_START		(AW8624_RAM_BASE + AW8624_RAM_HEADER_SIZE)
#define AW8624_WAVE_END			(AW8624_WAVE_START + \
					 ARRAY_SIZE(aw8624_waveform) - 1)

/*
 * Signed 8-bit samples: round(64 * sin(2 * pi * n / 117)). At 24 kHz this
 * gives 205.13 Hz, close to Lisa's nominal 205 Hz LRA resonance. A peak of
 * 64 (about half of signed full scale, versus 120 in the working reference)
 * deliberately keeps initial hardware bring-up conservative even at unity
 * gain. This is a digital amplitude limit, not a calibrated motor voltage.
 */
static const u8 aw8624_waveform[] = {
	0x00, 0x03, 0x07, 0x0a, 0x0e, 0x11, 0x14, 0x17, 0x1b, 0x1e, 0x21, 0x24,
	0x26, 0x29, 0x2c, 0x2e, 0x30, 0x33, 0x35, 0x37, 0x38, 0x3a, 0x3b, 0x3c,
	0x3d, 0x3e, 0x3f, 0x40, 0x40, 0x40, 0x40, 0x40, 0x3f, 0x3f, 0x3e, 0x3d,
	0x3c, 0x3b, 0x39, 0x37, 0x36, 0x34, 0x32, 0x2f, 0x2d, 0x2a, 0x28, 0x25,
	0x22, 0x1f, 0x1c, 0x19, 0x16, 0x13, 0x0f, 0x0c, 0x09, 0x05, 0x02, 0xfe,
	0xfb, 0xf7, 0xf4, 0xf1, 0xed, 0xea, 0xe7, 0xe4, 0xe1, 0xde, 0xdb, 0xd8,
	0xd6, 0xd3, 0xd1, 0xce, 0xcc, 0xca, 0xc9, 0xc7, 0xc5, 0xc4, 0xc3, 0xc2,
	0xc1, 0xc1, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc8, 0xc9, 0xcb, 0xcd, 0xd0, 0xd2, 0xd4, 0xd7, 0xda, 0xdc, 0xdf,
	0xe2, 0xe5, 0xe9, 0xec, 0xef, 0xf2, 0xf6, 0xf9, 0xfd,
};

/* RAM payload: library version, then big-endian inclusive start/end addresses. */
static const u8 aw8624_ram_header[] = {
	0x01,
	AW8624_WAVE_START >> 8, AW8624_WAVE_START & 0xff,
	AW8624_WAVE_END >> 8, AW8624_WAVE_END & 0xff,
};

struct aw8624 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct input_dev *input;
	struct work_struct work;
	/* Serializes all register sequences, including the threaded IRQ. */
	struct mutex lock;
	bool ram_valid;
	/* The memless callback runs with the input event spinlock held. */
	spinlock_t state_lock;
	u8 gain;
	bool opened;
	bool suspended;
	bool removing;
	int irq;
};

static void aw8624_assert_reset(void *data)
{
	struct aw8624 *aw = data;

	gpiod_set_value_cansleep(aw->reset_gpio, 1);
	aw->ram_valid = false;
}

/* Caller holds lock. Always attempt standby, even if stopping GO failed. */
static int aw8624_stop(struct aw8624 *aw)
{
	unsigned int state;
	int error, ret;

	/* A previous failed stop may already have held the device in reset. */
	if (!aw->ram_valid)
		return 0;

	error = regmap_update_bits(aw->regmap, AW8624_GO, AW8624_GO_ENABLE, 0);
	if (error) {
		dev_err(aw->dev, "Failed to clear GO: %d\n", error);
	} else {
		error = regmap_read_poll_timeout(aw->regmap, AW8624_GLB_STATE,
				state, !(state & AW8624_GLB_STATE_MASK),
				2000, 200000);
		if (error)
			dev_err(aw->dev, "Failed to reach idle: %d\n", error);
	}

	/* UVLO is expected in standby; retain the other protection interrupts. */
	ret = regmap_update_bits(aw->regmap, AW8624_SYSINTM,
				 AW8624_SYSINT_UVLO, AW8624_SYSINT_UVLO);
	if (ret) {
		dev_err(aw->dev, "Failed to mask standby UVLO: %d\n", ret);
		if (!error)
			error = ret;
	}

	ret = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				 AW8624_SYSCTRL_STANDBY, AW8624_SYSCTRL_STANDBY);
	if (ret) {
		dev_err(aw->dev, "Failed to enter standby: %d\n", ret);
		if (!error)
			error = ret;
	}

	/* Reset is the last-resort stop; a later start must re-upload all RAM. */
	if (error)
		aw8624_assert_reset(aw);
	return error;
}

static int aw8624_play(struct input_dev *input, void *data,
		       struct ff_effect *effect)
{
	struct aw8624 *aw = input_get_drvdata(input);
	unsigned long flags;
	u16 magnitude = effect->u.rumble.strong_magnitude;

	if (!magnitude)
		magnitude = effect->u.rumble.weak_magnitude;

	spin_lock_irqsave(&aw->state_lock, flags);
	if (aw->opened && !aw->suspended && !aw->removing) {
		aw->gain = DIV_ROUND_UP((u32)magnitude * AW8624_GAIN_FULL, 0xffff);
		/* Queue under the gate lock so close/suspend cannot miss this work. */
		schedule_work(&aw->work);
	}
	spin_unlock_irqrestore(&aw->state_lock, flags);

	return 0;
}

static int aw8624_open(struct input_dev *input)
{
	struct aw8624 *aw = input_get_drvdata(input);
	unsigned long flags;
	int error = 0;

	spin_lock_irqsave(&aw->state_lock, flags);
	if (aw->removing)
		error = -ENODEV;
	else if (aw->suspended)
		error = -EBUSY;
	else
		aw->opened = true;
	spin_unlock_irqrestore(&aw->state_lock, flags);

	return error;
}

static void aw8624_close(struct input_dev *input)
{
	struct aw8624 *aw = input_get_drvdata(input);
	unsigned long flags;
	bool accessible;

	spin_lock_irqsave(&aw->state_lock, flags);
	aw->opened = false;
	aw->gain = 0;
	spin_unlock_irqrestore(&aw->state_lock, flags);
	cancel_work_sync(&aw->work);

	mutex_lock(&aw->lock);
	spin_lock_irqsave(&aw->state_lock, flags);
	accessible = !aw->suspended && !aw->removing;
	spin_unlock_irqrestore(&aw->state_lock, flags);
	if (accessible)
		aw8624_stop(aw);
	mutex_unlock(&aw->lock);
}

static irqreturn_t aw8624_irq(int irq, void *data)
{
	struct aw8624 *aw = data;
	unsigned long flags;
	unsigned int status;
	int error;

	mutex_lock(&aw->lock);
	if (!aw->ram_valid)
		goto out;
	error = regmap_read(aw->regmap, AW8624_SYSINT, &status);
	if (error)
		dev_err_ratelimited(aw->dev, "Failed to read interrupt: %d\n", error);
	else if (status & AW8624_PROTECTION_IRQS)
		dev_err_ratelimited(aw->dev, "Protection interrupt: %#02x\n", status);
	else
		goto out;

	/* Drop pending playback before stopping; an old worker cannot restart it. */
	spin_lock_irqsave(&aw->state_lock, flags);
	aw->gain = 0;
	spin_unlock_irqrestore(&aw->state_lock, flags);
	aw8624_stop(aw);
out:
	mutex_unlock(&aw->lock);
	return IRQ_HANDLED;
}

static int aw8624_upload_ram(struct aw8624 *aw)
{
	int error, ret;
	unsigned int i;

	error = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				   AW8624_SYSCTRL_RAMINIT, AW8624_SYSCTRL_RAMINIT);
	if (error)
		return error;

	error = regmap_write(aw->regmap, AW8624_BASE_ADDRH, AW8624_RAM_BASE >> 8);
	if (error)
		goto out;
	error = regmap_write(aw->regmap, AW8624_BASE_ADDRL, AW8624_RAM_BASE & 0xff);
	if (error)
		goto out;
	error = regmap_write(aw->regmap, AW8624_RAMADDRH, AW8624_RAM_BASE >> 8);
	if (error)
		goto out;
	error = regmap_write(aw->regmap, AW8624_RAMADDRL, AW8624_RAM_BASE & 0xff);
	if (error)
		goto out;

	/* RAMDATA is a fixed-address port with an auto-incrementing RAM pointer. */
	for (i = 0; i < ARRAY_SIZE(aw8624_ram_header); i++) {
		error = regmap_write(aw->regmap, AW8624_RAMDATA, aw8624_ram_header[i]);
		if (error)
			goto out;
	}
	for (i = 0; i < ARRAY_SIZE(aw8624_waveform); i++) {
		error = regmap_write(aw->regmap, AW8624_RAMDATA, aw8624_waveform[i]);
		if (error)
			goto out;
	}
out:
	ret = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				 AW8624_SYSCTRL_RAMINIT, 0);
	if (ret) {
		dev_err(aw->dev, "Failed to finish RAM initialization: %d\n", ret);
		if (!error)
			error = ret;
	}
	return error;
}

static int aw8624_init(struct aw8624 *aw)
{
	unsigned int id;
	int error;

	/* Logical 1 asserts the standard active-low reset GPIO. */
	gpiod_set_value_cansleep(aw->reset_gpio, 1);
	usleep_range(5000, 5500);
	gpiod_set_value_cansleep(aw->reset_gpio, 0);
	usleep_range(5000, 5500);

	error = regmap_read(aw->regmap, AW8624_ID, &id);
	if (error)
		return error;
	if (id != AW8624_CHIP_ID) {
		dev_err(aw->dev, "Unexpected chip ID: %#02x\n", id);
		return -ENODEV;
	}

	error = regmap_write(aw->regmap, AW8624_ID, AW8624_SOFT_RESET);
	if (error)
		return error;
	usleep_range(1000, 1500);

	/* Keep every source masked until state and the IRQ handler are ready. */
	error = regmap_write(aw->regmap, AW8624_SYSINTM, AW8624_IRQ_MASK_ALL);
	if (error)
		return error;
	error = regmap_update_bits(aw->regmap, AW8624_DBGCTRL,
				   AW8624_DBGCTRL_INTN_SEL | AW8624_DBGCTRL_INT_MODE,
				   AW8624_DBGCTRL_INTN_SEL | AW8624_DBGCTRL_INT_EDGE);
	if (error)
		return error;
	error = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				   AW8624_SYSCTRL_STANDBY, AW8624_SYSCTRL_STANDBY);
	if (error)
		return error;
	error = regmap_update_bits(aw->regmap, AW8624_PWMDBG,
				   AW8624_PWMDBG_MODE, AW8624_PWMDBG_24KHZ);
	if (error)
		return error;
	error = regmap_write(aw->regmap, AW8624_DATDBG, 0);
	if (error)
		return error;
	error = regmap_write(aw->regmap, AW8624_WAVSEQ1, 1);
	if (error)
		return error;
	error = regmap_write(aw->regmap, AW8624_WAVSEQ2, 0);
	if (error)
		return error;
	error = regmap_write(aw->regmap, AW8624_WAVLOOP1, AW8624_WAVLOOP1_INFINITE);
	if (error)
		return error;

	/* AW8624 has no AW8695 boost registers; retain its analog/trim defaults. */
	error = aw8624_upload_ram(aw);
	if (!error)
		aw->ram_valid = true;
	return error;
}

static int aw8624_enable_irqs(struct aw8624 *aw)
{
	unsigned int status;
	int error;

	error = regmap_read(aw->regmap, AW8624_SYSINT, &status);
	if (error)
		return error;

	return regmap_write(aw->regmap, AW8624_SYSINTM,
			    AW8624_IRQ_MASK_ALL &
			    ~(AW8624_PROTECTION_IRQS & ~AW8624_SYSINT_UVLO));
}

static int aw8624_start(struct aw8624 *aw, u8 gain)
{
	unsigned int status;
	int error;

	if (!aw->ram_valid) {
		error = aw8624_init(aw);
		if (!error)
			error = aw8624_enable_irqs(aw);
		if (error) {
			aw8624_assert_reset(aw);
			return error;
		}
	}

	error = aw8624_stop(aw);
	if (error)
		return error;

	error = regmap_write(aw->regmap, AW8624_DATDBG, gain);
	if (error)
		return error;

	/* RAM playback is mode zero; clear standby to make the device active. */
	error = regmap_update_bits(aw->regmap, AW8624_SYSCTRL,
				   AW8624_SYSCTRL_PLAY_MODE | AW8624_SYSCTRL_STANDBY,
				   0);
	if (error)
		return error;

	/* Clear the standby UVLO latch before unmasking it in active mode. */
	error = regmap_read(aw->regmap, AW8624_SYSINT, &status);
	if (error)
		return error;

	error = regmap_update_bits(aw->regmap, AW8624_SYSINTM,
				   AW8624_SYSINT_UVLO, 0);
	if (error)
		return error;

	return regmap_update_bits(aw->regmap, AW8624_GO,
				  AW8624_GO_ENABLE, AW8624_GO_ENABLE);
}

static void aw8624_work(struct work_struct *work)
{
	struct aw8624 *aw = container_of(work, struct aw8624, work);
	unsigned long flags;
	u8 gain;
	int error;

	mutex_lock(&aw->lock);
	spin_lock_irqsave(&aw->state_lock, flags);
	gain = aw->opened && !aw->suspended && !aw->removing ? aw->gain : 0;
	spin_unlock_irqrestore(&aw->state_lock, flags);

	if (gain) {
		error = aw8624_start(aw, gain);
		if (error) {
			dev_err(aw->dev, "Failed to start rumble: %d\n", error);
			aw8624_stop(aw);
		}
	} else {
		aw8624_stop(aw);
	}
	mutex_unlock(&aw->lock);
}

static int aw8624_suspend(struct device *dev)
{
	struct aw8624 *aw = dev_get_drvdata(dev);
	unsigned long flags;
	int error;

	mutex_lock(&aw->input->mutex);
	spin_lock_irqsave(&aw->state_lock, flags);
	aw->suspended = true;
	aw->gain = 0;
	spin_unlock_irqrestore(&aw->state_lock, flags);

	disable_irq(aw->irq);
	cancel_work_sync(&aw->work);
	mutex_lock(&aw->lock);
	error = aw8624_stop(aw);
	mutex_unlock(&aw->lock);
	if (error) {
		spin_lock_irqsave(&aw->state_lock, flags);
		aw->suspended = false;
		spin_unlock_irqrestore(&aw->state_lock, flags);
		enable_irq(aw->irq);
	}
	mutex_unlock(&aw->input->mutex);

	/* Leave reset deasserted so standby retains RAM through suspend. */
	return error;
}

static int aw8624_resume(struct device *dev)
{
	struct aw8624 *aw = dev_get_drvdata(dev);
	unsigned long flags;

	mutex_lock(&aw->input->mutex);
	spin_lock_irqsave(&aw->state_lock, flags);
	aw->suspended = false;
	spin_unlock_irqrestore(&aw->state_lock, flags);
	enable_irq(aw->irq);
	mutex_unlock(&aw->input->mutex);

	/* Only a new FF request may start playback; never replay a stale effect. */
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw8624_pm_ops, aw8624_suspend, aw8624_resume);

static void aw8624_remove(struct i2c_client *client)
{
	struct aw8624 *aw = i2c_get_clientdata(client);
	unsigned long flags;

	spin_lock_irqsave(&aw->state_lock, flags);
	aw->removing = true;
	aw->opened = false;
	aw->gain = 0;
	spin_unlock_irqrestore(&aw->state_lock, flags);
	disable_irq(aw->irq);
	cancel_work_sync(&aw->work);

	mutex_lock(&aw->lock);
	aw8624_stop(aw);
	aw8624_assert_reset(aw);
	mutex_unlock(&aw->lock);
}

static const struct regmap_config aw8624_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW8624_MAX_REGISTER,
	.cache_type = REGCACHE_NONE,
};

static int aw8624_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw8624 *aw;
	int error;

	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "Missing interrupt\n");

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->dev = dev;
	aw->irq = client->irq;
	mutex_init(&aw->lock);
	spin_lock_init(&aw->state_lock);
	INIT_WORK(&aw->work, aw8624_work);
	i2c_set_clientdata(client, aw);

	aw->regmap = devm_regmap_init_i2c(client, &aw8624_regmap_config);
	if (IS_ERR(aw->regmap))
		return dev_err_probe(dev, PTR_ERR(aw->regmap), "Failed to create regmap\n");

	aw->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(aw->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(aw->reset_gpio), "Failed to get reset GPIO\n");
	error = devm_add_action_or_reset(dev, aw8624_assert_reset, aw);
	if (error)
		return error;

	aw->input = devm_input_allocate_device(dev);
	if (!aw->input)
		return -ENOMEM;
	aw->input->name = "aw8624-haptics";
	aw->input->id.bustype = BUS_I2C;
	aw->input->open = aw8624_open;
	aw->input->close = aw8624_close;
	input_set_drvdata(aw->input, aw);
	input_set_capability(aw->input, EV_FF, FF_RUMBLE);

	error = input_ff_create_memless(aw->input, NULL, aw8624_play);
	if (error)
		return dev_err_probe(dev, error, "Failed to create force feedback\n");

	mutex_lock(&aw->lock);
	error = aw8624_init(aw);
	mutex_unlock(&aw->lock);
	if (error)
		return dev_err_probe(dev, error, "Failed to initialize hardware and RAM\n");

	/* All handler dependencies and the waveform exist before IRQ delivery. */
	error = devm_request_threaded_irq(dev, aw->irq, NULL, aw8624_irq,
					  IRQF_ONESHOT, dev_name(dev), aw);
	if (error)
		return dev_err_probe(dev, error, "Failed to request interrupt\n");

	mutex_lock(&aw->lock);
	error = aw8624_enable_irqs(aw);
	mutex_unlock(&aw->lock);
	if (error)
		return dev_err_probe(dev, error, "Failed to enable protection interrupts\n");

	error = input_register_device(aw->input);
	if (error)
		return dev_err_probe(dev, error, "Failed to register input device\n");

	return 0;
}

static const struct of_device_id aw8624_of_match[] = {
	{ .compatible = "awinic,aw8624" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw8624_of_match);

static const struct i2c_device_id aw8624_id[] = {
	{ "aw8624" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw8624_id);

static struct i2c_driver aw8624_driver = {
	.driver = {
		.name = "aw8624",
		.of_match_table = aw8624_of_match,
		.pm = pm_sleep_ptr(&aw8624_pm_ops),
	},
	.probe = aw8624_probe,
	.remove = aw8624_remove,
	.shutdown = aw8624_remove,
	.id_table = aw8624_id,
};
module_i2c_driver(aw8624_driver);

MODULE_DESCRIPTION("Awinic AW8624 RAM-mode rumble driver");
MODULE_LICENSE("GPL");
