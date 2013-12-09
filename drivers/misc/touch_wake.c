/* drivers/misc/touch_wake.c
 *
 * Copyright 2013  Jean-Pierre Rasquin <yank555.lu@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * --------------------------------------------------------------------------------------
 *
 * Base idea by Ezekeel
 *
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/touch_wake.h>
#include <linux/workqueue.h>
#include <linux/powersuspend.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/input.h>

extern void touchscreen_enable(void);
extern void touchscreen_disable(void);

extern void touchscreen_pen_enable(void);
extern void touchscreen_pen_disable(void);

static bool touchwake_enabled = false;
static bool touch_disabled = false;
static bool device_suspended = false;
static bool timed_out = true;
static bool keep_awake = false;
static unsigned int touchoff_delay = 5000;

static void touchwake_touchoff(struct work_struct *touchoff_work);
static DECLARE_DELAYED_WORK(touchoff_work, touchwake_touchoff);
static void press_powerkey(struct work_struct *presspower_work);
static DECLARE_WORK(presspower_work, press_powerkey);
static DEFINE_MUTEX(lock);

static struct input_dev *powerkey_device;
static struct wake_lock touchwake_wake_lock;
static struct timeval last_powerkeypress;

#define TOUCHWAKE_VERSION "1.0 by Yank555.lu"
#define TIME_LONGPRESS 500
#define POWERPRESS_DELAY 60
#define POWERPRESS_TIMEOUT 1000

static void touchwake_disable_touch(void)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Disable touch controls\n");
	#endif
	touchscreen_disable();
	touchscreen_pen_disable();
	touch_disabled = true;

	return;
}

static void touchwake_enable_touch(void)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Enable touch controls\n");
	#endif
	touchscreen_enable();
	touchscreen_pen_enable();
	touch_disabled = false;
	return;
}

static void touchwake_suspend(struct power_suspend * h)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Enter suspend\n");
	#endif

	if (touchwake_enabled) {
		if (likely(touchoff_delay > 0))	{
			if (timed_out) {
				#ifdef TOUCHWAKE_DEBUG_PRINT
				pr_info("[TOUCHWAKE] Suspend - enable touch delay\n");
				#endif
				keep_awake = true; // Keep digitizers awake for now
				touchscreen_pen_enable(); // Wacom needs to be reactivated
				wake_lock(&touchwake_wake_lock);
				schedule_delayed_work(&touchoff_work, msecs_to_jiffies(touchoff_delay));
			} else {
				#ifdef TOUCHWAKE_DEBUG_PRINT
				pr_info("[TOUCHWAKE] Suspend - disable touch immediately\n");
				#endif
				keep_awake = false; // Digitizers can be disabled
				touchwake_disable_touch();
			}
		} else {
			if (timed_out) {
				#ifdef TOUCHWAKE_DEBUG_PRINT
				pr_info("[TOUCHWAKE] Suspend - keep touch enabled indefinately\n");
				#endif
				keep_awake = true; // Keep digitizers awake for now
				touchscreen_pen_enable(); // Wacom needs to be reactivated
				wake_lock(&touchwake_wake_lock);
			} else {
				#ifdef TOUCHWAKE_DEBUG_PRINT
				pr_info("[TOUCHWAKE] Suspend - disable touch immediately (indefinate mode)\n");
				#endif
				keep_awake = false; // Digitizers can be disabled
				touchwake_disable_touch();
			}
		}
	} else {
		#ifdef TOUCHWAKE_DEBUG_PRINT
		pr_info("[TOUCHWAKE] Suspend - disable touch immediately (TouchWake disabled)\n");
		#endif
		keep_awake = false; // Digitizers can be disabled
		touchwake_disable_touch();
	}

	device_suspended = true;

	return;
}

static void touchwake_resume(struct power_suspend * h)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Enter resume\n");
	#endif

	cancel_delayed_work(&touchoff_work);
	flush_scheduled_work();

	wake_unlock(&touchwake_wake_lock);

	if (touch_disabled)
		touchwake_enable_touch();

	keep_awake = false; // Digitizers can be disabled
	timed_out = true;
	device_suspended = false;

	return;
}

static struct power_suspend touchwake_suspend_data =
{
	.suspend = touchwake_suspend,
	.resume = touchwake_resume,
};

static void touchwake_touchoff(struct work_struct * touchoff_work)
{
	keep_awake = false; // Digitizers can be disabled
	touchwake_disable_touch();
	wake_unlock(&touchwake_wake_lock);

	return;
}

static void press_powerkey(struct work_struct * presspower_work)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Simulate Powerkey press on device %p.\n", powerkey_device);
	#endif

	input_event(powerkey_device, EV_KEY, KEY_POWER, 1);
	input_event(powerkey_device, EV_SYN, 0, 0);
	msleep(POWERPRESS_DELAY);

	input_event(powerkey_device, EV_KEY, KEY_POWER, 0);
	input_event(powerkey_device, EV_SYN, 0, 0);
	msleep(POWERPRESS_DELAY);

	msleep(POWERPRESS_TIMEOUT);

	mutex_unlock(&lock);

	return;
}

void powerkey_pressed(void)
{
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Powerkey pressed\n");
	#endif

	do_gettimeofday(&last_powerkeypress);
	timed_out = false; // Yank555 : consider user is indeed turning off the device

	return;
}

void powerkey_released(void)
{
	struct timeval now;
	int time_pressed;

	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Powerkey released\n");
	#endif

	do_gettimeofday(&now);

	time_pressed = (now.tv_sec - last_powerkeypress.tv_sec) * MSEC_PER_SEC +
	(now.tv_usec - last_powerkeypress.tv_usec) / USEC_PER_MSEC;

	if (unlikely(time_pressed > TIME_LONGPRESS || device_suspended)) {
		timed_out = true; // Yank555 : OK, user is not turning off device, but long-pressing Powerkey, or turing on device, so back to normal
		#ifdef TOUCHWAKE_DEBUG_PRINT
		pr_info("[TOUCHWAKE] Powerkey : Device being turned on or longpress release detected\n");
		#endif
	#ifdef TOUCHWAKE_DEBUG_PRINT
	} else {
		pr_info("[TOUCHWAKE] Powerkey : Device being turned off\n");
	#endif
	}

	return;
}

void touch_press(void)
{   
	#ifdef TOUCHWAKE_DEBUG_PRINT
	pr_info("[TOUCHWAKE] Touch press detected\n");
	#endif

	if (unlikely(device_suspended && touchwake_enabled && mutex_trylock(&lock)))
		schedule_work(&presspower_work);

	return;
}

bool device_is_suspended(void)
{
	return device_suspended;
}

bool touchwake_active(void)
{
	return keep_awake;
}

// Sysfs start ---------------------------------------------------------------------------------------

static ssize_t touchwake_status_read(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%u\n", (touchwake_enabled ? 1 : 0));
}

static ssize_t touchwake_status_write(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		pr_devel("%s: %u \n", __FUNCTION__, data);

		if (data == 1) {
			#ifdef TOUCHWAKE_DEBUG_PRINT
			pr_info("[TOUCHWAKE] %s: TOUCHWAKE function enabled\n", __FUNCTION__);
			#endif
			keep_awake = false; // Digitizers can be disabled
			touchwake_enabled = true;
		} else if (data == 0) {
			#ifdef TOUCHWAKE_DEBUG_PRINT
			pr_info("[TOUCHWAKE] %s: TOUCHWAKE function disabled\n", __FUNCTION__);
			#endif
			keep_awake = false; // Digitizers can be disabled
			touchwake_enabled = false;
		#ifdef TOUCHWAKE_DEBUG_PRINT
		} else {
			pr_info("[TOUCHWAKE] %s: invalid input range %u\n", __FUNCTION__, data);
		#endif
		}
	#ifdef TOUCHWAKE_DEBUG_PRINT
	} else 	{
		pr_info("[TOUCHWAKE] %s: invalid input\n", __FUNCTION__);
	#endif
	}

	return size;
}

static ssize_t touchwake_delay_read(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%u\n", touchoff_delay);
}

static ssize_t touchwake_delay_write(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int data;

	if(sscanf(buf, "%u\n", &data) == 1) {
		touchoff_delay = data;
		#ifdef TOUCHWAKE_DEBUG_PRINT
		pr_info("[TOUCHWAKE] Delay set to %u\n", touchoff_delay); 
		#endif
	#ifdef TOUCHWAKE_DEBUG_PRINT
	} else 	{
		pr_info("[TOUCHWAKE] %s: invalid input\n", __FUNCTION__);
	#endif
	}

	return size;
}

static ssize_t touchwake_version(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%s\n", TOUCHWAKE_VERSION);
}

#ifdef TOUCHWAKE_DEBUG_PRINT
static ssize_t touchwake_debug(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "timed_out : %u\n", (unsigned int) timed_out);
}
#endif

static DEVICE_ATTR(enabled, S_IRUGO | S_IWUGO, touchwake_status_read, touchwake_status_write);
static DEVICE_ATTR(delay, S_IRUGO | S_IWUGO, touchwake_delay_read, touchwake_delay_write);
static DEVICE_ATTR(version, S_IRUGO , touchwake_version, NULL);
#ifdef TOUCHWAKE_DEBUG_PRINT
static DEVICE_ATTR(debug, S_IRUGO , touchwake_debug, NULL);
#endif

static struct attribute *touchwake_notification_attributes[] =
{
	&dev_attr_enabled.attr,
	&dev_attr_delay.attr,
	&dev_attr_version.attr,
#ifdef TOUCHWAKE_DEBUG_PRINT
	&dev_attr_debug.attr,
#endif
	NULL
};

static struct attribute_group touchwake_notification_group =
{
	.attrs  = touchwake_notification_attributes,
};

static struct miscdevice touchwake_device =
{
	.minor = MISC_DYNAMIC_MINOR,
	.name = "touchwake",
};

// Sysfs end -----------------------------------------------------------------------------------------

static int __init touchwake_control_init(void)
{
	int ret;

	pr_info("%s misc_register(%s)\n", __FUNCTION__, touchwake_device.name);
	ret = misc_register(&touchwake_device);

	if (ret) {
		pr_err("[TOUCHWAKE] Failed to %s misc_register(%s)\n", __FUNCTION__, touchwake_device.name);
		return 1;
	}

	powerkey_device = input_allocate_device();
	input_set_capability(powerkey_device, EV_KEY, KEY_POWER);
	powerkey_device->name = "touchwake_powerkey";
	powerkey_device->phys = "touchwake_powerkey/input0";
	ret = input_register_device(powerkey_device);

	if (ret) {
		pr_err("[TOUCHWAKE] Failed to register Powerkey device (%s/%d)\n", __func__, ret);
		return 1;
	}

	if (sysfs_create_group(&touchwake_device.this_device->kobj, &touchwake_notification_group) < 0) {
		pr_err("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n", touchwake_device.name);
	}

	register_power_suspend(&touchwake_suspend_data);

	wake_lock_init(&touchwake_wake_lock, WAKE_LOCK_SUSPEND, "touchwake_wake");

	do_gettimeofday(&last_powerkeypress);

	return 0;
}

device_initcall(touchwake_control_init);
