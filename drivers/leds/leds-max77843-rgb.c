/*
 * RGB-led driver for Maxim MAX77843
 *
 * Copyright (C) 2013 Maxim Integrated Product
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 *	Each OCTA has different light transmittance.
 *	So assign differnt LED brightness to each OCTA.
 *	This code has dependency on ZERO.
 *
 *	octa_color (ZERO BASE)
 *	0000 : Black
 *	0001 : Other
 *	0010 : White
 *	0011 : Gold
 *	0101 : Red
 *
 *	octa_color (ZERO_F BASE)
 *	0000 : Black
 *	0001 : White
 *	0010 : Gold
 *	0011 : Blue
 *	0100 : Red
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/mfd/max77843.h>
#include <linux/mfd/max77843-private.h>
#include <linux/leds-max77843-rgb.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/sec_sysfs.h>
#include <linux/time.h>
#include <linux/syscalls.h>

#define SEC_LED_SPECIFIC

/* Registers */
/*defined max77843-private.h*//*
 max77843_led_reg {
	MAX77843_RGBLED_REG_LEDEN           = 0x30,
	MAX77843_RGBLED_REG_LED0BRT         = 0x31,
	MAX77843_RGBLED_REG_LED1BRT         = 0x32,
	MAX77843_RGBLED_REG_LED2BRT         = 0x33,
	MAX77843_RGBLED_REG_LED3BRT         = 0x34,
	MAX77843_RGBLED_REG_LEDRMP          = 0x36,
	MAX77843_RGBLED_REG_LEDBLNK         = 0x38,
	MAX77843_LED_REG_END,
};*/

/* MAX77843_REG_LED0BRT */
#define MAX77843_LED0BRT	0xFF

/* MAX77843_REG_LED1BRT */
#define MAX77843_LED1BRT	0xFF

/* MAX77843_REG_LED2BRT */
#define MAX77843_LED2BRT	0xFF

/* MAX77843_REG_LED3BRT */
#define MAX77843_LED3BRT	0xFF

/* MAX77843_REG_LEDBLNK */
#define MAX77843_LEDBLINKD	0xF0
#define MAX77843_LEDBLINKP	0x0F

/* MAX77843_REG_LEDRMP */
#define MAX77843_RAMPUP		0xF0
#define MAX77843_RAMPDN		0x0F

#define LED_R_MASK		0x00FF0000
#define LED_G_MASK		0x0000FF00
#define LED_B_MASK		0x000000FF
#define LED_MAX_CURRENT		0xFF

/* MAX77843_STATE*/
#define LED_DISABLE			0
#define LED_ALWAYS_ON			1
#define LED_BLINK			2

#define LEDBLNK_ON(time)	((time < 100) ? 0 :			\
				(time < 500) ? time/100-1 :		\
				(time < 3250) ? (time-500)/250+4 : 15)

#define LEDBLNK_OFF(time)	((time < 1) ? 0x00 :			\
				(time < 500) ? 0x01 :			\
				(time < 5000) ? time/500 :		\
				(time < 8000) ? (time-5000)/1000+10 :	 \
				(time < 12000) ? (time-8000)/2000+13 : 15)

extern unsigned int lcdtype;

static u8 led_dynamic_current = 0x14;

static u8 normal_powermode_current = 0x14;
static u8 low_powermode_current = 0x05;

static unsigned int device_type = 0;
static unsigned int brightness_ratio_r = 100;
static unsigned int brightness_ratio_g = 100;
static unsigned int brightness_ratio_b = 100;

static u8 led_lowpower_mode = 0x0;

unsigned int octa_color = 0x0;

// Enable Fading by default
unsigned int led_enable_fade = 1;
unsigned int led_fade_time_up = 800;
unsigned int led_fade_time_down = 800;
unsigned int led_always_disable = 0;
unsigned int led_debug_enable = 0;
int led_block_leds_time_start = -1;
int led_block_leds_time_stop = -1;
struct device *GBLdev = NULL;

static struct delayed_work check_led_time;
static bool is_work_active = false;

enum max77843_led_color {
	WHITE,
	RED,
	GREEN,
	BLUE,
};
enum max77843_led_pattern {
	PATTERN_OFF,
	CHARGING,
	CHARGING_ERR,
	MISSED_NOTI,
	LOW_BATTERY,
	FULLY_CHARGED,
	POWERING,
};

static struct device *led_dev;

struct max77843_rgb {
	struct led_classdev led[4];
	struct i2c_client *i2c;
	unsigned int delay_on_times_ms;
	unsigned int delay_off_times_ms;
};

#if defined(CONFIG_LEDS_USE_ED28) && defined(CONFIG_SEC_FACTORY)
extern bool jig_status;
#endif

static int max77843_rgb_number(struct led_classdev *led_cdev,
				struct max77843_rgb **p)
{
	const struct device *parent = led_cdev->dev->parent;
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(parent);
	int i;

	*p = max77843_rgb;

	for (i = 0; i < 4; i++) {
		if (led_cdev == &max77843_rgb->led[i]) {
			dev_dbg("leds-max77843-rgb: %s, %d\n", __func__, i);
			return i;
		}
	}

	return -ENODEV;
}

static void max77843_rgb_set(struct led_classdev *led_cdev,
				unsigned int brightness)
{
	const struct device *parent = led_cdev->dev->parent;
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(parent);
	struct device *dev;
	int n;
	int ret;

	ret = max77843_rgb_number(led_cdev, &max77843_rgb);
	if (IS_ERR_VALUE(ret)) {
		dev_err(led_cdev->dev,
			"max77843_rgb_number() returns %d.\n", ret);
		return;
	}

	dev = led_cdev->dev;
	n = ret;

	if (brightness == LED_OFF) {
		/* Flash OFF */
		ret = max77843_update_reg(max77843_rgb->i2c,
					MAX77843_RGBLED_REG_LEDEN, 0 , 3 << (2*n));
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "can't write LEDEN : %d\n", ret);
			return;
		}
	} else {
		/* Set current */
		ret = max77843_write_reg(max77843_rgb->i2c,
				MAX77843_RGBLED_REG_LED0BRT + n, brightness);
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "can't write LEDxBRT : %d\n", ret);
			return;
		}
		/* Flash ON */
		ret = max77843_update_reg(max77843_rgb->i2c,
				MAX77843_RGBLED_REG_LEDEN, 0x55, 3 << (2*n));
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "can't write FLASH_EN : %d\n", ret);
			return;
		}
	}
}

static void max77843_rgb_set_state(struct led_classdev *led_cdev,
				unsigned int brightness, unsigned int led_state)
{
	const struct device *parent = led_cdev->dev->parent;
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(parent);
	struct device *dev;
	int n;
	int ret;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	ret = max77843_rgb_number(led_cdev, &max77843_rgb);

	if (IS_ERR_VALUE(ret)) {
		dev_err(led_cdev->dev,
			"max77843_rgb_number() returns %d.\n", ret);
		return;
	}

	dev = led_cdev->dev;
	n = ret;

	if(brightness != 0) {
		/* apply brightness ratio for optimize each led brightness*/
		switch(n) {
		case RED:
			brightness = brightness * brightness_ratio_r / 100;
			break;
		case GREEN:
			brightness = brightness * brightness_ratio_g / 100;
			break;
		case BLUE:
			brightness = brightness * brightness_ratio_b / 100;
			break;
		}

		/*
			There is possibility that low_powermode_current is 0.
			ex) low_powermode_current is 1 & brightness_ratio_r is 90
			brightness = 1 * 90 / 100 = 0.9
			brightness is inteager, so brightness is 0.
			In this case, it is need to assign 1 of value.
		*/
		if(brightness == 0)
			brightness = 1;
	}

	max77843_rgb_set(led_cdev, brightness);

	pr_info("leds-max77843-rgb: %s, led_num = %d, brightness = %d\n", __func__, ret, brightness);

	ret = max77843_update_reg(max77843_rgb->i2c,
			MAX77843_RGBLED_REG_LEDEN, led_state << (2*n), 0x3 << 2*n);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "can't write FLASH_EN : %d\n", ret);
		return;
	}
}

static unsigned int max77843_rgb_get(struct led_classdev *led_cdev)
{
	const struct device *parent = led_cdev->dev->parent;
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(parent);
	struct device *dev;
	int n;
	int ret;
	u8 value;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	ret = max77843_rgb_number(led_cdev, &max77843_rgb);
	if (IS_ERR_VALUE(ret)) {
		dev_err(led_cdev->dev,
			"max77843_rgb_number() returns %d.\n", ret);
		return 0;
	}
	n = ret;

	dev = led_cdev->dev;

	/* Get status */
	ret = max77843_read_reg(max77843_rgb->i2c,
				MAX77843_RGBLED_REG_LEDEN, &value);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "can't read LEDEN : %d\n", ret);
		return 0;
	}
	if (!(value & (1 << n)))
		return LED_OFF;

	/* Get current */
	ret = max77843_read_reg(max77843_rgb->i2c,
				MAX77843_RGBLED_REG_LED0BRT + n, &value);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "can't read LED0BRT : %d\n", ret);
		return 0;
	}

	return value;
}

static int max77843_rgb_ramp(struct device *dev, int ramp_up, int ramp_down)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	int value;
	int ret;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	if (ramp_up <= led_fade_time_up) {
		ramp_up /= 100;
	} else {
		ramp_up = (ramp_up - led_fade_time_up) * 2 + led_fade_time_up;
		ramp_up /= 100;
	}

	if (ramp_down <= led_fade_time_down) {
		ramp_down /= 100;
	} else {
		ramp_down = (ramp_down - led_fade_time_down) * 2 + led_fade_time_down;
		ramp_down /= 100;
	}

	value = (ramp_down) | (ramp_up << 4);
	ret = max77843_write_reg(max77843_rgb->i2c,
					MAX77843_RGBLED_REG_LEDRMP, value);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "can't write REG_LEDRMP : %d\n", ret);
		return -ENODEV;
	}

	return 0;
}

static int max77843_rgb_blink(struct device *dev,
				unsigned int delay_on, unsigned int delay_off)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	int value;
	int ret = 0;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	value = (LEDBLNK_ON(delay_on) << 4) | LEDBLNK_OFF(delay_off);
	ret = max77843_write_reg(max77843_rgb->i2c,
					MAX77843_RGBLED_REG_LEDBLNK, value);
	if (IS_ERR_VALUE(ret)) {
		dev_err(dev, "can't write REG_LEDBLNK : %d\n", ret);
		return -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_OF
static struct max77843_rgb_platform_data
			*max77843_rgb_parse_dt(struct device *dev)
{
	struct max77843_rgb_platform_data *pdata;
	struct device_node *nproot = dev->parent->of_node;
	struct device_node *np;
	int ret;
	int i;
	int temp;
	char octa[4] = {0, };
	char br_ratio_r[23] = "br_ratio_r";
	char br_ratio_g[23] = "br_ratio_g";
	char br_ratio_b[23] = "br_ratio_b";
	char normal_po_cur[29] = "normal_powermode_current";
	char low_po_cur[26] = "low_powermode_current";

	pr_info("leds-max77843-rgb: %s\n", __func__);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (unlikely(pdata == NULL))
		return ERR_PTR(-ENOMEM);

	np = of_find_node_by_name(nproot, "rgb");
	if (unlikely(np == NULL)) {
		dev_err(dev, "rgb node not found\n");
		devm_kfree(dev, pdata);
		return ERR_PTR(-EINVAL);
	}

	for (i = 0; i < 4; i++)	{
		ret = of_property_read_string_index(np, "rgb-name", i,
						(const char **)&pdata->name[i]);

		pr_info("leds-max77843-rgb: %s, %s\n", __func__,pdata->name[i]);

		if (IS_ERR_VALUE(ret)) {
			devm_kfree(dev, pdata);
			return ERR_PTR(ret);
		}
	}

	/* get device_type value in dt */
	ret = of_property_read_u32(np, "device_type", &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing device_type in dt\n", __func__);
	}
	else {
		device_type = (u8)temp;
	}
	pr_info("leds-max77843-rgb: %s, device_type = %x\n", __func__, device_type);

	/* ZERO */
	if(device_type == 0) {
		pr_info("here0\n");

		switch(octa_color) {
		case 0:
			strcpy(octa, "_bk");
			break;
		case 2:
			strcpy(octa, "_wh");
			break;
		case 3:
			strcpy(octa, "_gd");
			break;
		case 4:
			strcpy(octa, "_gr");
			break;
		case 5:
			strcpy(octa, "_rd");
			break;
		default:
			break;
		}
	}
	/* ZEROF */
	else if(device_type == 1) {
		pr_info("here1\n");

		switch(octa_color) {
		case 0:
			strcpy(octa, "_bk");
			break;
		case 1:
			strcpy(octa, "_wh");
			break;
		case 2:
			strcpy(octa, "_gd");
			break;
		case 3:
			strcpy(octa, "_bl");
			break;
		case 4:
			strcpy(octa, "_rd");
			break;
		default:
			break;
		}
	}
	strcat(normal_po_cur, octa);
	strcat(low_po_cur, octa);
	strcat(br_ratio_r, octa);
	strcat(br_ratio_g, octa);
	strcat(br_ratio_b, octa);

	/* get normal_powermode_current value in dt */
	ret = of_property_read_u32(np, normal_po_cur, &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing normal_powermode_current in dt\n", __func__);
	}
	else {
		normal_powermode_current = (u8)temp;
	}
	pr_info("leds-max77843-rgb: %s, normal_powermode_current = %x\n", __func__, normal_powermode_current);

	/* get low_powermode_current value in dt */
	ret = of_property_read_u32(np, low_po_cur, &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing low_powermode_current in dt\n", __func__);
	}
	else
		low_powermode_current = (u8)temp;
	pr_info("leds-max77843-rgb: %s, low_powermode_current = %x\n", __func__, low_powermode_current);

	/* get led red brightness ratio */
	ret = of_property_read_u32(np, br_ratio_r, &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing brightness_ratio_r in dt\n", __func__);
	}
	else {
		brightness_ratio_r = (int)temp;
	}
	pr_info("leds-max77843-rgb: %s, brightness_ratio_r = %x\n", __func__, brightness_ratio_r);

	/* get led green brightness ratio */
	ret = of_property_read_u32(np, br_ratio_g, &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing brightness_ratio_g in dt\n", __func__);
	}
	else {
		brightness_ratio_g = (int)temp;
	}
	pr_info("leds-max77843-rgb: %s, brightness_ratio_g = %x\n", __func__, brightness_ratio_g);

	/* get led blue brightness ratio */
	ret = of_property_read_u32(np, br_ratio_b, &temp);
	if (IS_ERR_VALUE(ret)) {
		pr_info("leds-max77843-rgb: %s, can't parsing brightness_ratio_b in dt\n", __func__);
	}
	else {
		brightness_ratio_b = (int)temp;
	}
	pr_info("leds-max77843-rgb: %s, brightness_ratio_b = %x\n", __func__, brightness_ratio_b);

	return pdata;
}
#endif

static void max77843_rgb_reset(struct device *dev)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	max77843_rgb_set_state(&max77843_rgb->led[RED], LED_OFF, LED_DISABLE);
	max77843_rgb_set_state(&max77843_rgb->led[GREEN], LED_OFF, LED_DISABLE);
	max77843_rgb_set_state(&max77843_rgb->led[BLUE], LED_OFF, LED_DISABLE);
	max77843_rgb_ramp(dev, 0, 0);
}

static ssize_t store_max77843_rgb_lowpower(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int ret;
	u8 led_lowpower;

	ret = kstrtou8(buf, 0, &led_lowpower);
	if (ret != 0) {
		dev_err(dev, "fail to get led_lowpower.\n");
		return count;
	}

	led_lowpower_mode = led_lowpower;

	dev_dbg(dev, "led_lowpower mode set to %i\n", led_lowpower);

	return count;
}
static ssize_t store_max77843_rgb_brightness(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int ret;
	u8 brightness;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	ret = kstrtou8(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(dev, "fail to get led_brightness.\n");
		return count;
	}

	led_lowpower_mode = 0;

	if (brightness > LED_MAX_CURRENT)
		brightness = LED_MAX_CURRENT;

	led_dynamic_current = brightness;

	dev_dbg(dev, "led brightness set to %i\n", brightness);

	return count;
}

static bool check_restrictions(void)
{
	struct timeval curtime;
	struct tm tmv;
	int curhour;
	bool ret = true;

	if (led_always_disable)
	{
		ret = false;
		max77843_rgb_reset(GBLdev);
		goto skipitall;
	}
	if (led_block_leds_time_start != -1 && led_block_leds_time_stop != -1)
	{
		do_gettimeofday(&curtime);
		time_to_tm(curtime.tv_sec, 0, &tmv);

		curhour = tmv.tm_hour + ((sys_tz.tz_minuteswest / 60) * -1);
		if (curhour < 0)
			curhour = 24 + curhour;
		if (curhour > 23)
			curhour = curhour - 24;

		if (led_debug_enable) pr_alert("CHECK LED TIME RESTRICTION: %d:%d:%d:%ld -- %d -- %d -- %d\n", tmv.tm_hour, tmv.tm_min,
				         tmv.tm_sec, curtime.tv_usec, sys_tz.tz_minuteswest, sys_tz.tz_dsttime, curhour);
		if (led_block_leds_time_start > led_block_leds_time_stop)
		{
			if (curhour >= led_block_leds_time_start || curhour < led_block_leds_time_stop)
				ret = false;
		}
		else
		{
			if (curhour >= led_block_leds_time_start && curhour < led_block_leds_time_stop)
				ret = false;
		}
		/* Set all LEDs Off */
		if (!ret && GBLdev != NULL)
			max77843_rgb_reset(GBLdev);
	}
skipitall:
	return ret;
}

static ssize_t store_max77843_rgb_pattern(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	unsigned int mode = 0;
	int ret;
	pr_info("leds-max77843-rgb: %s\n", __func__);

	ret = sscanf(buf, "%1d", &mode);
	if (ret == 0) {
		dev_err(dev, "fail to get led_pattern mode.\n");
		return count;
	}
	GBLdev = dev;

	/* Set all LEDs Off */
	max77843_rgb_reset(dev);
	if (mode == PATTERN_OFF)
		return count;


	if (!check_restrictions())
		return count;

	/* Set to low power consumption mode */
	if (led_lowpower_mode == 1)
		led_dynamic_current = low_powermode_current;
	else
		led_dynamic_current = normal_powermode_current;

	switch (mode) {

	case CHARGING:
	{
		max77843_rgb_set_state(&max77843_rgb->led[RED], led_dynamic_current, LED_ALWAYS_ON);
		break;
	}
	case CHARGING_ERR:
		max77843_rgb_blink(dev, 500, 500);
		max77843_rgb_set_state(&max77843_rgb->led[RED], led_dynamic_current, LED_BLINK);
		break;
	case MISSED_NOTI:
		if (led_enable_fade)
		{
			max77843_rgb_ramp(dev, led_fade_time_up, led_fade_time_down);
			max77843_rgb_blink(dev, led_fade_time_up, 5000);
		}
		else
		{
			max77843_rgb_blink(dev, 500, 5000);
		}
		max77843_rgb_set_state(&max77843_rgb->led[BLUE], led_dynamic_current, LED_BLINK);
		break;
	case LOW_BATTERY:
		if (led_enable_fade)
		{
			max77843_rgb_ramp(dev, led_fade_time_up, led_fade_time_down);
			max77843_rgb_blink(dev, led_fade_time_up, 5000);
		}
		else
		{
			max77843_rgb_blink(dev, 500, 5000);
		}
		max77843_rgb_set_state(&max77843_rgb->led[RED], led_dynamic_current, LED_BLINK);
		break;
	case FULLY_CHARGED:
		max77843_rgb_set_state(&max77843_rgb->led[GREEN], led_dynamic_current, LED_ALWAYS_ON);
		break;
	case POWERING:
		max77843_rgb_ramp(dev, 800, 800);
		max77843_rgb_blink(dev, 200, 200);
		max77843_rgb_set_state(&max77843_rgb->led[BLUE], led_dynamic_current, LED_ALWAYS_ON);
		max77843_rgb_set_state(&max77843_rgb->led[GREEN], led_dynamic_current, LED_BLINK);
		break;
	default:
		break;
	}

	return count;
}

static ssize_t store_max77843_rgb_blink(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	int led_brightness = 0;
	int delay_on_time = 0;
	int delay_off_time = 0;
	u8 led_r_brightness = 0;
	u8 led_g_brightness = 0;
	u8 led_b_brightness = 0;
	unsigned int led_total_br = 0;
	unsigned int led_max_br = 0;
	int ret;

	ret = sscanf(buf, "0x%8x %5d %5d", &led_brightness,
					&delay_on_time, &delay_off_time);
	if (ret == 0) {
		dev_err(dev, "fail to get led_blink value.\n");
		return count;
	}

	/* Set to low power consumption mode */
	led_dynamic_current = normal_powermode_current;

	/*Reset led*/
	max77843_rgb_reset(dev);

	led_r_brightness = (led_brightness & LED_R_MASK) >> 16;
	led_g_brightness = (led_brightness & LED_G_MASK) >> 8;
	led_b_brightness = led_brightness & LED_B_MASK;

	/* In user case, LED current is restricted to less than tuning value */
	if (led_r_brightness != 0) {
		led_r_brightness = (led_r_brightness * led_dynamic_current) / LED_MAX_CURRENT;
		if (led_r_brightness == 0)
			led_r_brightness = 1;
	}
	if (led_g_brightness != 0) {
		led_g_brightness = (led_g_brightness * led_dynamic_current) / LED_MAX_CURRENT;
		if (led_g_brightness == 0)
			led_g_brightness = 1;
	}
	if (led_b_brightness != 0) {
		led_b_brightness = (led_b_brightness * led_dynamic_current) / LED_MAX_CURRENT;
		if (led_b_brightness == 0)
			led_b_brightness = 1;
	}

	led_total_br += led_r_brightness * brightness_ratio_r / 100;
	led_total_br += led_g_brightness * brightness_ratio_g / 100;
	led_total_br += led_b_brightness * brightness_ratio_b / 100;

	if (brightness_ratio_r >= brightness_ratio_g &&
		brightness_ratio_r >= brightness_ratio_b) {
		led_max_br = normal_powermode_current * brightness_ratio_r / 100;
	} else if (brightness_ratio_g >= brightness_ratio_r &&
		brightness_ratio_g >= brightness_ratio_b) {
		led_max_br = normal_powermode_current * brightness_ratio_g / 100;
	} else if (brightness_ratio_b >= brightness_ratio_r &&
		brightness_ratio_b >= brightness_ratio_g) {
		led_max_br = normal_powermode_current * brightness_ratio_b / 100;
	}

	/* Each color decreases according to the limit at the same rate. */
	if(device_type == 1 && octa_color == 1) {
		/* There is current consumption problem.
		   So, add workaround code in the case of zerof white octa device */
		if (led_total_br > led_max_br) {
			if (led_r_brightness != 0) {
				led_r_brightness = led_r_brightness * led_max_br / led_total_br * 8 / 10;
				if (led_r_brightness == 0)
					led_r_brightness = 1;
			}
			if (led_g_brightness != 0) {
				led_g_brightness = led_g_brightness * led_max_br / led_total_br * 8 / 10;
				if (led_g_brightness == 0)
					led_g_brightness = 1;
			}
			if (led_b_brightness != 0) {
				led_b_brightness = led_b_brightness * led_max_br / led_total_br;
				if (led_b_brightness == 0)
					led_b_brightness = 1;
			}
		}
	}
	else {
		if (led_total_br > led_max_br) {
			if (led_r_brightness != 0) {
				led_r_brightness = led_r_brightness * led_max_br / led_total_br;
				if (led_r_brightness == 0)
					led_r_brightness = 1;
			}
			if (led_g_brightness != 0) {
				led_g_brightness = led_g_brightness * led_max_br / led_total_br;
				if (led_g_brightness == 0)
					led_g_brightness = 1;
			}
			if (led_b_brightness != 0) {
				led_b_brightness = led_b_brightness * led_max_br / led_total_br;
				if (led_b_brightness == 0)
					led_b_brightness = 1;
			}
		}
	}
	

	if (led_r_brightness) {
		max77843_rgb_set_state(&max77843_rgb->led[RED], led_r_brightness, LED_BLINK);
	}
	if (led_g_brightness) {
		max77843_rgb_set_state(&max77843_rgb->led[GREEN], led_g_brightness, LED_BLINK);
	}
	if (led_b_brightness) {
		max77843_rgb_set_state(&max77843_rgb->led[BLUE], led_b_brightness, LED_BLINK);
	}
	/*Should we ramp?*/
	if (led_enable_fade && delay_on_time > 0)
	{
		max77843_rgb_ramp(dev, led_fade_time_up, led_fade_time_down);
	}
	/*Set LED blink mode*/
	max77843_rgb_blink(dev, delay_on_time, delay_off_time);

	pr_info("leds-max77843-rgb: %s, delay_on_time= %x, delay_off_time= %x\n", __func__, delay_on_time, delay_off_time);
	dev_dbg(dev, "led_blink is called, Color:0x%X Brightness:%i\n",
			led_brightness, led_dynamic_current);
	return count;
}

static ssize_t store_led_r(struct device *dev,
			struct device_attribute *devattr,
				const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	unsigned int brightness;
	int ret;

	ret = kstrtouint(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(dev, "fail to get brightness.\n");
		goto out;
	}
	if (brightness != 0) {
		max77843_rgb_set_state(&max77843_rgb->led[RED], brightness, LED_ALWAYS_ON);
	} else {
		max77843_rgb_set_state(&max77843_rgb->led[RED], LED_OFF, LED_DISABLE);
	}
out:
	pr_info("leds-max77843-rgb: %s\n", __func__);
	return count;
}
static ssize_t store_led_g(struct device *dev,
			struct device_attribute *devattr,
			const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	unsigned int brightness;
	int ret;

	ret = kstrtouint(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(dev, "fail to get brightness.\n");
		goto out;
	}
	if (brightness != 0) {
		max77843_rgb_set_state(&max77843_rgb->led[GREEN], brightness, LED_ALWAYS_ON);
	} else {
		max77843_rgb_set_state(&max77843_rgb->led[GREEN], LED_OFF, LED_DISABLE);
	}
out:
	pr_info("leds-max77843-rgb: %s\n", __func__);
	return count;
}
static ssize_t store_led_b(struct device *dev,
		struct device_attribute *devattr,
		const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	unsigned int brightness;
	int ret;

	ret = kstrtouint(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(dev, "fail to get brightness.\n");
		goto out;
	}
	if (brightness != 0) {
		max77843_rgb_set_state(&max77843_rgb->led[BLUE], brightness, LED_ALWAYS_ON);
	} else	{
		max77843_rgb_set_state(&max77843_rgb->led[BLUE], LED_OFF, LED_DISABLE);
	}
out:
	pr_info("leds-max77843-rgb: %s\n", __func__);
	return count;
}

/* Added for led common class */
static ssize_t led_delay_on_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev->parent);
	return sprintf(buf, "%u\n", max77843_rgb->delay_on_times_ms);
}

static ssize_t led_delay_on_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev->parent);
	unsigned int time;

	if (kstrtouint(buf, 0, &time)) {
		dev_err(dev, "can not write led_delay_on\n");
		return count;
	}

	max77843_rgb->delay_on_times_ms = time;

	return count;
}

static ssize_t led_delay_off_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%u\n", max77843_rgb->delay_off_times_ms);
}

static ssize_t led_delay_off_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev->parent);
	unsigned int time;

	if (kstrtouint(buf, 0, &time)) {
		dev_err(dev, "can not write led_delay_off\n");
		return count;
	}

	max77843_rgb->delay_off_times_ms = time;

	return count;
}

static ssize_t led_blink_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	const struct device *parent = dev->parent;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(parent);
	unsigned int blink_set;

	if (!sscanf(buf, "%1d", &blink_set)) {
		dev_err(dev, "can not write led_blink\n");
		return count;
	}

	if (!blink_set) {
		max77843_rgb->delay_on_times_ms = LED_OFF;
		max77843_rgb->delay_off_times_ms = LED_OFF;
	}

	max77843_rgb_blink(parent,
		max77843_rgb->delay_on_times_ms,
		max77843_rgb->delay_off_times_ms);
	max77843_rgb_set_state(led_cdev, led_dynamic_current, LED_BLINK);

	pr_info("leds-max77843-rgb: %s\n", __func__);
	return count;
}

static ssize_t led_fade_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_enable_fade);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_enable_fade);

	return ret;
}

static ssize_t led_fade_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int enabled = 0;
	retval = sscanf(buf, "%d", &enabled);
	if (retval != 0 && (enabled == 0 || enabled == 1))
		led_enable_fade = enabled;

	printk(KERN_DEBUG "led_fade is called\n");

	return count;
}

static ssize_t led_debug_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_debug_enable);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_debug_enable);

	return ret;
}

static ssize_t led_debug_enable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int enabled = 0;
	retval = sscanf(buf, "%d", &enabled);
	if (retval != 0 && (enabled == 0 || enabled == 1))
		led_debug_enable = enabled;

	printk(KERN_DEBUG "led_debug_enable is called\n");

	return count;
}

static ssize_t led_fade_time_up_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_fade_time_up);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_fade_time_up);

	return ret;
}

static ssize_t led_fade_time_up_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int val = 0;
	retval = sscanf(buf, "%d", &val);
	if (retval != 0 && val >= 100  &&  val <= 4000)
		led_fade_time_up = val;
	printk(KERN_DEBUG "led_time_on is called\n");

	return count;
}

static ssize_t led_fade_time_down_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_fade_time_down);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_fade_time_down);

	return ret;
}

static ssize_t led_fade_time_down_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int val = 0;
	retval = sscanf(buf, "%d", &val);
	if (retval != 0 && val >= 100  &&  val <= 4000)
		led_fade_time_down = val;
	printk(KERN_DEBUG "led_time_off is called\n");

	return count;
}

static ssize_t led_always_disable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_always_disable);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_always_disable);

	return ret;
}

static ssize_t led_always_disable_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int val = 0;
	retval = sscanf(buf, "%d", &val);
	if (retval != 0 && (val == 0 || val == 1))
		led_always_disable = val;
	printk(KERN_DEBUG "led_time_off is called\n");

	return count;
}

static ssize_t led_block_leds_time_start_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_block_leds_time_start);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_block_leds_time_start);

	return ret;
}

static ssize_t led_block_leds_time_start_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int val = 0;
	retval = sscanf(buf, "%d", &val);
	if (retval != 0 && (val == -1 || (val >= 0 && val <= 23))) {
		led_block_leds_time_start = val;
	}
	if (!is_work_active && led_block_leds_time_start != -1 && led_block_leds_time_stop != -1)
	{
		is_work_active = true;
		schedule_delayed_work_on(0, &check_led_time, msecs_to_jiffies(30000));
	}
	else if (led_block_leds_time_start == -1 || led_block_leds_time_stop == -1)
		is_work_active = false;

	return count;
}

static ssize_t led_block_leds_time_stop_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int ret;
	ret = sprintf(buf, "%d\n", led_block_leds_time_stop);
	pr_info("[LED] %s: led_fade=%d\n", __func__, led_block_leds_time_stop);

	return ret;
}

static ssize_t led_block_leds_time_stop_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int retval;
	int val = 0;
	retval = sscanf(buf, "%d", &val);
	if (retval != 0 && (val == -1 || (val >= 0 && val <= 23))) {
		led_block_leds_time_stop = val;
	}
	if (!is_work_active && led_block_leds_time_start != -1 && led_block_leds_time_stop != -1)
	{
		is_work_active = true;
		schedule_delayed_work_on(0, &check_led_time, msecs_to_jiffies(30000));
	}
	else if (led_block_leds_time_start == -1 || led_block_leds_time_stop == -1)
		is_work_active = false;

	return count;
}

/* permission for sysfs node */
static DEVICE_ATTR(delay_on, 0640, led_delay_on_show, led_delay_on_store);
static DEVICE_ATTR(delay_off, 0640, led_delay_off_show, led_delay_off_store);
static DEVICE_ATTR(blink, 0640, NULL, led_blink_store);

//Fade LED code
static DEVICE_ATTR(led_fade, 0664, led_fade_show, led_fade_store);
static DEVICE_ATTR(led_fade_time_up, 0664, led_fade_time_up_show, led_fade_time_up_store);
static DEVICE_ATTR(led_fade_time_down, 0664, led_fade_time_down_show, led_fade_time_down_store);
static DEVICE_ATTR(led_always_disable, 0664, led_always_disable_show, led_always_disable_store);
static DEVICE_ATTR(led_debug_enable, 0664, led_debug_enable_show, led_debug_enable_store);
static DEVICE_ATTR(led_block_leds_time_start, 0664, led_block_leds_time_start_show, led_block_leds_time_start_store);
static DEVICE_ATTR(led_block_leds_time_stop, 0664, led_block_leds_time_stop_show, led_block_leds_time_stop_store);

#ifdef SEC_LED_SPECIFIC
/* below nodes is SAMSUNG specific nodes */
static DEVICE_ATTR(led_r, 0660, NULL, store_led_r);
static DEVICE_ATTR(led_g, 0660, NULL, store_led_g);
static DEVICE_ATTR(led_b, 0660, NULL, store_led_b);
/* led_pattern node permission is 222 */
/* To access sysfs node from other groups */
static DEVICE_ATTR(led_pattern, 0660, NULL, store_max77843_rgb_pattern);
static DEVICE_ATTR(led_blink, 0660, NULL,  store_max77843_rgb_blink);
static DEVICE_ATTR(led_brightness, 0660, NULL, store_max77843_rgb_brightness);
static DEVICE_ATTR(led_lowpower, 0660, NULL,  store_max77843_rgb_lowpower);
#endif

static struct attribute *led_class_attrs[] = {
	&dev_attr_delay_on.attr,
	&dev_attr_delay_off.attr,
	&dev_attr_blink.attr,
	NULL,
};

static struct attribute_group common_led_attr_group = {
	.attrs = led_class_attrs,
};

#ifdef SEC_LED_SPECIFIC
static struct attribute *sec_led_attributes[] = {
	&dev_attr_led_r.attr,
	&dev_attr_led_g.attr,
	&dev_attr_led_b.attr,
	&dev_attr_led_pattern.attr,
	&dev_attr_led_blink.attr,
	&dev_attr_led_brightness.attr,
	&dev_attr_led_lowpower.attr,
	&dev_attr_led_fade.attr,
	&dev_attr_led_fade_time_up.attr,
	&dev_attr_led_fade_time_down.attr,
	&dev_attr_led_always_disable.attr,
	&dev_attr_led_debug_enable.attr,
	&dev_attr_led_block_leds_time_start.attr,
	&dev_attr_led_block_leds_time_stop.attr,
	NULL,
};

static struct attribute_group sec_led_attr_group = {
	.attrs = sec_led_attributes,
};
#endif
static int max77843_rgb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct max77843_rgb_platform_data *pdata;
	struct max77843_rgb *max77843_rgb;
	struct max77843_dev *max77843_dev = dev_get_drvdata(dev->parent);
	char temp_name[4][40] = {{0,},}, name[40] = {0,}, *p;
	int i, ret;

	pr_info("leds-max77843-rgb: %s\n", __func__);

	octa_color = (lcdtype >> 16) & 0x0000000f;

#ifdef CONFIG_OF
	pdata = max77843_rgb_parse_dt(dev);
	if (unlikely(IS_ERR(pdata)))
		return PTR_ERR(pdata);

	led_dynamic_current = normal_powermode_current;
#else
	pdata = dev_get_platdata(dev);
#endif

	pr_info("leds-max77843-rgb: %s : lcdtype=%d, octa_color=%x device_type=%x \n",
		__func__, lcdtype, octa_color, device_type);

	max77843_rgb = devm_kzalloc(dev, sizeof(struct max77843_rgb), GFP_KERNEL);
	if (unlikely(!max77843_rgb))
		return -ENOMEM;
	pr_info("leds-max77843-rgb: %s 1 \n", __func__);

	max77843_rgb->i2c = max77843_dev->i2c;

	for (i = 0; i < 4; i++) {
		ret = snprintf(name, 30, "%s", pdata->name[i])+1;
		if (1 > ret)
			goto alloc_err_flash;

		p = devm_kzalloc(dev, ret, GFP_KERNEL);
		if (unlikely(!p))
			goto alloc_err_flash;

		strcpy(p, name);
		strcpy(temp_name[i], name);
		max77843_rgb->led[i].name = p;
		max77843_rgb->led[i].brightness_set = max77843_rgb_set;
		max77843_rgb->led[i].brightness_get = max77843_rgb_get;
		max77843_rgb->led[i].max_brightness = LED_MAX_CURRENT;

		ret = led_classdev_register(dev, &max77843_rgb->led[i]);
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "unable to register RGB : %d\n", ret);
			goto alloc_err_flash_plus;
		}
		ret = sysfs_create_group(&max77843_rgb->led[i].dev->kobj,
						&common_led_attr_group);
		if (ret) {
			dev_err(dev, "can not register sysfs attribute\n");
			goto register_err_flash;
		}
	}

	led_dev = sec_device_create(max77843_rgb, "led");
	if (IS_ERR(led_dev)) {
		dev_err(dev, "Failed to create device for samsung specific led\n");
		goto create_err_flash;
	}


	ret = sysfs_create_group(&led_dev->kobj, &sec_led_attr_group);
	if (ret < 0) {
		dev_err(dev, "Failed to create sysfs group for samsung specific led\n");
		goto device_create_err;
	}

	platform_set_drvdata(pdev, max77843_rgb);
#if defined(CONFIG_LEDS_USE_ED28) && defined(CONFIG_SEC_FACTORY)
	if( lcdtype == 0 && jig_status == false) {
		max77843_rgb_set_state(&max77843_rgb->led[RED], led_dynamic_current, LED_ALWAYS_ON);
	}
#endif
	pr_info("leds-max77843-rgb: %s done\n", __func__);

	return 0;

device_create_err:
	sec_device_destroy(led_dev->devt);
create_err_flash:
    sysfs_remove_group(&led_dev->kobj, &common_led_attr_group);
register_err_flash:
	led_classdev_unregister(&max77843_rgb->led[i]);
alloc_err_flash_plus:
	devm_kfree(dev, temp_name[i]);
alloc_err_flash:
	while (i--) {
		led_classdev_unregister(&max77843_rgb->led[i]);
		devm_kfree(dev, temp_name[i]);
	}
	devm_kfree(dev, max77843_rgb);
	return -ENOMEM;
}

static int max77843_rgb_remove(struct platform_device *pdev)
{
	struct max77843_rgb *max77843_rgb = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < 4; i++)
		led_classdev_unregister(&max77843_rgb->led[i]);

	return 0;
}

static void max77843_rgb_shutdown(struct device *dev)
{
	struct max77843_rgb *max77843_rgb = dev_get_drvdata(dev);
	int i;

	if (!max77843_rgb->i2c)
		return;

	max77843_rgb_reset(dev);

	sysfs_remove_group(&led_dev->kobj, &sec_led_attr_group);

	for (i = 0; i < 4; i++){
		sysfs_remove_group(&max77843_rgb->led[i].dev->kobj,
						&common_led_attr_group);
		led_classdev_unregister(&max77843_rgb->led[i]);
	}
	devm_kfree(dev, max77843_rgb);
}
static struct platform_driver max77843_fled_driver = {
	.driver		= {
		.name	= "leds-max77843-rgb",
		.owner	= THIS_MODULE,
		.shutdown = max77843_rgb_shutdown,
	},
	.probe		= max77843_rgb_probe,
	.remove		= __devexit_p(max77843_rgb_remove),
};

static void check_led_timer(struct work_struct *work)
{
	check_restrictions();
	if (is_work_active && led_block_leds_time_start != -1 && led_block_leds_time_stop != -1)
		schedule_delayed_work_on(0, &check_led_time, msecs_to_jiffies(30000));
}

static int __init max77843_rgb_init(void)
{
	pr_info("leds-max77843-rgb: %s\n", __func__);
	INIT_DELAYED_WORK(&check_led_time, check_led_timer);
	return platform_driver_register(&max77843_fled_driver);
}
module_init(max77843_rgb_init);

static void __exit max77843_rgb_exit(void)
{
	platform_driver_unregister(&max77843_fled_driver);
}
module_exit(max77843_rgb_exit);

MODULE_ALIAS("platform:max77843-rgb");
MODULE_AUTHOR("Jeongwoong Lee<jell.lee@samsung.com>");
MODULE_DESCRIPTION("MAX77843 RGB driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
