// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "g502.h"

struct logi_g502_data {
	struct list_head profiles_list;
	struct g502_profile *profiles[G502_MAX_PROFILES];
	struct g502_profile *current_prof;
	struct private_work_struct priv_ws;
	struct input_dev *input_dev;
	struct mutex mutex_dev; /* Protect this struct's shared fields */
	struct gfirmware gfw;
};

static __always_inline void
__do_fill_report(struct hidpp_report *report_ptr,
			u8 id, u8 feature_index, u8 function_index, size_t report_length, u8 *params)
{
	memset(report_ptr, 0, sizeof(*report_ptr));
	report_ptr->report_id = id;
	report_ptr->device_index = G502_DEVICE_INDEX_RECEIVER;
	report_ptr->feature_index = feature_index;
	report_ptr->funcindex_clientid = function_index | LINUX_KERNEL_SW_ID;
	if (params)
		memcpy(id == G502_COMMAND_SHORT_REPORT_ID ?
				report_ptr->params_s : report_ptr->params_l,
					params, report_length - 4U);
}

static __always_inline void echo_current_profile_config(struct g502_profile *curr_prof)
{
    pr_info("current profile\tindex: %d\n\treport rate: %u\n\trgb: %u\n\tdpi: %u\n\t",
                 curr_prof->index, curr_prof->dev_report_rate, curr_prof->dev_rgb, curr_prof->dev_dpi);
}

static __always_inline void
initalize_profile_struct(struct g502_profile *prof_ptr,
		u16 report_rate, unsigned int rgb, u16 dpi, int index)
{
	prof_ptr->dev_report_rate = report_rate;
	prof_ptr->dev_rgb = rgb;
	prof_ptr->dev_dpi = dpi;
	prof_ptr->index	= index;
}

static int g502_send_report(struct hid_device *hdev,
			struct hidpp_report *report)
{
	int ret = -ENOMEM;
	const size_t report_length = (report->report_id == G502_COMMAND_SHORT_REPORT_ID
								? G502_COMMAND_SHORT_SIZE
								: G502_COMMAND_LONG_SIZE);

	/* There may be underlying issues with the protocol itself, that
	 * are yield back in the response packet, so we should be able to
	 * examine packets in the future
	 */
	ret = hid_hw_raw_request(hdev, report->report_id, (u8 *)report,
			report_length, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_err(hdev,
			"%s: cannot issue hid raw request (%d)\n", __func__, ret);
		goto out;
	}

	ret = 0;

out:
	return ret;
}

/* Callback called by schedule_work, to be run in a work context.
 * and executes hid_hw_raw_request which _can_ schedule and sleep.
 */
static void g502_work_send_report(struct work_struct *work)
{
	struct private_work_struct *priv_ws = container_of(work,
				struct private_work_struct, work);

	g502_send_report(priv_ws->hdev, &priv_ws->report);
}

/* Sends out command to get the current device's config to be caught
 * in raw_event and be stored in our device's private structure.
 */
static void g502_refresh_gdv_config(struct hid_device *hdev)
{
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 0)
		return;

	__do_fill_report(&gdv->priv_ws.report, G502_COMMAND_SHORT_REPORT_ID,
		G502_FEATURE_REPORT_RATE, G502_GET_REPORT_RATE,
		G502_COMMAND_SHORT_SIZE, NULL);
	schedule_work(&gdv->priv_ws.work);

	__do_fill_report(&gdv->priv_ws.report, G502_COMMAND_SHORT_REPORT_ID,
		G502_FEATURE_DPI, G502_GET_DPI, G502_COMMAND_SHORT_SIZE, NULL);
	schedule_work(&gdv->priv_ws.work);

	/* TODO: Add RGB */
}

/* Generic function to set current device's config to values passed as arguments.
 * Pass 0 as one of the arguments if you don't desire to change that argument.
 */
static int g502_update_device_config(struct hid_device *hdev, u16 report_rate,
			u16 dpi, unsigned int rgb)
{
	u8 __maybe_unused params[G502_COMMAND_LONG_SIZE - 4U] = { 0 };
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	// RGB unused rgb_s = rgb_to_struct_rgb(rgb);

	if (report_rate)
	{
		params[0] = report_rate;
		__do_fill_report(&gdv->priv_ws.report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_REPORT_RATE, G502_SET_REPORT_RATE,
							G502_COMMAND_SHORT_SIZE, params);
		schedule_work(&gdv->priv_ws.work);
	}

	if (dpi)
	{
		params[0] = 0; /* Sensor idx */
		params[1] = (u8)((dpi >> 8) & 0xFF);
		params[2] = (u8)((dpi) & 0xFF);
		__do_fill_report(&gdv->priv_ws.report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_DPI, G502_SET_DPI,
							G502_COMMAND_SHORT_SIZE, params);
		schedule_work(&gdv->priv_ws.work);
	}

	/* Fetch in back the values we just submitted
	 * for them to be in gdv */
	g502_refresh_gdv_config(hdev);

	return 0;
}

/* Switch profiles on BTN_9 Click interrupt. */
static int g502_switch_profile(struct hid_device *hdev)
{
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);

	if (unlikely(list_empty(&gdv->profiles_list)))
		return -EINVAL;


	/* Circular means it returns the _first_ element if
		 gdv->current_prof->entry is the last one. */
	gdv->current_prof = list_next_entry_circular(gdv->current_prof,
							&gdv->profiles_list, entry);
	g502_update_device_config(hdev, gdv->current_prof->dev_report_rate,
			gdv->current_prof->dev_dpi, gdv->current_prof->dev_rgb);

	echo_current_profile_config(gdv->current_prof);
	return 0;
}

/* Handle the regular mouse events, e.g.
 * support horizontal scrolling (tilt wheel left/right) */
static int g502_handle_regular_event(struct hid_device *hdev,
			struct input_dev *input, u8 *data)
{
	if (input == NULL)
		return 1;

	/* Wheel cmds is one byte after buttons, except middle-click. */
	if (data[1] & 0x2) { // LEFT
		input_report_rel(input, REL_HWHEEL, -1);
		input_report_rel(input, REL_HWHEEL_HI_RES,
				-120);
	} else if (data[1] & 0x4) { // RIGHT
		input_report_rel(input, REL_HWHEEL, 1);
		input_report_rel(input, REL_HWHEEL_HI_RES,
				120);
	} else if (data[1] & 0x1) { // G6
		return g502_switch_profile(hdev);
	}

// sync_out:
	input_sync(input);
	return 0;
}

/* Only 9 programmable buttons, instead of 16 */
static __u8 *g502_report_fixup(struct hid_device *hdev,
				__u8 *rdesc, unsigned int *rsize)
{
	/* rdesc[15] = Usage Maximum, rdesc[21] = Report Count */
	if (*rsize == 67 && rdesc[15] == 16 && rdesc[21] == 16) {
		hid_info(hdev, "fixing up g502 hero report descriptor\n");
		rdesc[15] = 0x09;

		/* Seriously, hid_irq_in breaks when rdesc[21] is changed,
		 * with "input irq status -75 (E-OVERFLOW) value". I _literally_ have no clue why.
		 * Figure it out, but for now leave it as is.
		 */

		// rdesc[21] = 0x09;
	}

	return rdesc;
}

#define g502_map_key_clear(c)  hid_map_usage_clear(hi, usage, bit, max, EV_KEY, (c))

/* All buttons must be mapped correctly. We only map G6,
 * to BTN_9 for it to be used with profile changes at @switch_profile
 * whenever BTN_9 is pressed.
 */
static int g502_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_BUTTON)
		return 0;
	if (intf->cur_altsetting->desc.bInterfaceNumber == 1)
		return 0;

	switch (usage->hid & HID_USAGE)
	{
	case 9: g502_map_key_clear(BTN_9); break;
	default: return 0;
	}

	return 1;
}

/*
 * Parse the hidpp_report contents we received.
 *
 * SET_REPORT response is empty, probably just a handshake,
 * because that's how the protocol works. Now, the actual data
 * comes as USB_INTERRUPT, meaning that HID I/O are independent
 * from each other. We validate it and store the contents.
 */
static int g502_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct hidpp_report *response = (struct hidpp_report *)data;
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	struct input_dev *input = gdv->input_dev;
	u8 function_idx;

	/* Regular events */
	if (size == 8)
		return g502_handle_regular_event(hdev, input, data);

	/* We always need a LONG report, save resources. */
	if (response->report_id !=
		G502_COMMAND_LONG_REPORT_ID ||
		 	size != G502_COMMAND_LONG_SIZE)
		return 1;

	/* We use our mutex do indicate whether a new report can be passed */
	if (mutex_is_locked(&gdv->mutex_dev))
		return 1;

	mutex_lock(&gdv->mutex_dev);
	function_idx = response->funcindex_clientid & ~LINUX_KERNEL_SW_ID;
	switch (response->feature_index)
	{
	case G502_FEATURE_REPORT_RATE:
		if (function_idx == G502_GET_REPORT_RATE)
			gdv->current_prof->dev_report_rate =
					report_rate_htd(response->params_l[0]);
		break;
	case G502_FEATURE_DPI:
		if (function_idx == G502_GET_DPI)
			gdv->current_prof->dev_dpi = (u16)(response->params_l[1] << 8) |
					(response->params_l[2]);
		break;
	case G502_FEATURE_DEVICE_FW:
		break;
	}
	mutex_unlock(&gdv->mutex_dev);

	return 0;
}

/* We define a macro to handle the attributes' show operations.
 * as it's simply the same for all. */
#define G502_ATTR_SHOW(name, feature_uppercase)			       				\
	static ssize_t name##_show(struct device *dev,			       			\
			struct device_attribute *attr, char *buf)						\
	{								       									\
		struct logi_g502_data *gdv = dev_get_drvdata(dev);					\
		int ret;															\
																			\
		mutex_lock(&gdv->mutex_dev);										\
		ret = sysfs_emit(buf, "%u\n", gdv->current_prof->dev_##name);		\
		mutex_unlock(&gdv->mutex_dev);										\
		return ret;															\
	}

static ssize_t report_rate_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	unsigned __report_rate;
	u8 report_rate;

	if (kstrtouint(buf, 0, &__report_rate))
		return -EINVAL;
	if ((report_rate = report_rate_dth(__report_rate)) == 0)
		return -EINVAL;

	g502_update_device_config(hdev, report_rate, 0, 0);

	return count;
}

/* TBD */
static ssize_t dpi_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	// struct hid_device *hdev = to_hid_device(dev);
	// u16 dpi_requested;

	// if (kstrtou16(buf, 0, &dpi_requested))
	// 	return -EINVAL;
	// if (0 > dpi_requested || dpi_requested > G502_MAX_RESOLUTION_DPI)
	// 	return -EINVAL;

	// hid_info(hdev, "%u\n", dpi_requested);

	// g502_update_device_config(hdev, 0, dpi_requested, 0);

	// return count;
	return count;
}

G502_ATTR_SHOW(report_rate, REPORT_RATE);
G502_ATTR_SHOW(dpi, DPI);

static DEVICE_ATTR_RW(report_rate);
static DEVICE_ATTR_RW(dpi);

static const struct attribute_group g502_group = {
	.attrs = (struct attribute *[]) {
		&dev_attr_report_rate.attr,
		&dev_attr_dpi.attr,
		NULL,
	}
};

static int __init g502_init_drvdata(struct hid_device *hdev)
{
	int i;
	struct hid_input *hidinput;
	u8 params[G502_COMMAND_LONG_SIZE - 4U] = { 0 };
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if (list_empty(&hdev->inputs))
		return -EINVAL;

	mutex_init(&gdv->mutex_dev);
	INIT_LIST_HEAD(&gdv->profiles_list);
	INIT_WORK(&gdv->priv_ws.work, g502_work_send_report);
	gdv->priv_ws.hdev = hdev;

	hidinput = list_first_entry(&hdev->inputs, struct hid_input, list);
	gdv->input_dev = hidinput->input;

	/* Currently, all profiles must be allocated and setup correctly. */
	for (i = 0; i < G502_MAX_PROFILES; i++) {
		gdv->profiles[i] = devm_kzalloc(&hdev->dev, sizeof(*gdv->current_prof), GFP_KERNEL);
		if (!gdv->profiles[i])
			return -EFAULT;
		list_add(&gdv->profiles[i]->entry, &gdv->profiles_list);
	}

	initalize_profile_struct(gdv->profiles[0], 1, 0, 800, 0);
	initalize_profile_struct(gdv->profiles[1], 2, 0, 1600, 1);
	initalize_profile_struct(gdv->profiles[2], 4, 0, 2400, 2);
	initalize_profile_struct(gdv->profiles[3], 8, 0, 3200, 3);
	initalize_profile_struct(gdv->profiles[4], 8, 0, 6000, 4);

	gdv->current_prof = list_first_entry(&gdv->profiles_list,
							struct g502_profile, entry);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
		if (sysfs_create_group(&hdev->dev.kobj, &g502_group) < 0)
		{
			hid_err(hdev, "%s: failed to create sysfs attrs\n", __func__);
			return -EFAULT;
		}
	}

	/* Disable on-board profiles support on device entry */
	params[0] = G502_ON_BOARD_PROFILES_OFF;
	__do_fill_report(&gdv->priv_ws.report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_ON_BOARD_PROFILES, G502_CONTROL_ON_BOARD_PROFILES,
							G502_COMMAND_SHORT_SIZE, params);
	schedule_work(&gdv->priv_ws.work);

	g502_refresh_gdv_config(hdev);

	return 0;
}

static const struct hid_device_id logitech_g502[] = {
	{ HID_USB_DEVICE(LOGITECH_VENDOR_ID, G502_HERO_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, logitech_g502);

static struct hid_driver g502_hid_driver = {
	.name = "g502_hero_hid",
	.id_table = logitech_g502,
	.probe = g502_hero_probe,
	.remove = g502_hero_remove,
	.input_mapping = g502_input_mapping,
	.report_fixup = g502_report_fixup,
	.raw_event = g502_raw_event,
};
module_hid_driver(g502_hid_driver);

static int g502_hero_probe(struct hid_device *hdev,
			const struct hid_device_id *id)
{
	int retval;
	struct logi_g502_data *gdv;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	gdv = devm_kzalloc(&hdev->dev, sizeof(*gdv), GFP_KERNEL);
	if (!gdv) {
		hid_err(hdev, "%s: couldn't allocate memory for internal structure\n",
					__func__);
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, gdv);

	retval = hid_parse(hdev);
	if (retval) {
		hid_err(hdev, "%s: failed to parse HID\n", __func__);
		return retval;
	}

	retval = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (retval) {
		hid_err(hdev, "%s: failed to start hw\n", __func__);
		return retval;
	}

	retval = hid_hw_open(hdev);
	if (retval) {
		hid_err(hdev, "%s: failed to open hw\n", __func__);
		goto out_hw_stop;
	}

	hid_device_io_start(hdev);

	retval = g502_init_drvdata(hdev);
	if (retval < 0) {
		hid_err(hdev, "%s: device's driver data initalization failed\n",
					__func__);
		goto out_hw_stop;
	}

	return 0;

out_hw_stop:
	hid_hw_stop(hdev);
	return retval;
}

static void g502_hero_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

MODULE_AUTHOR("Roi L");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("hid:logitech-g502-hero");
MODULE_DESCRIPTION("HID Logitech G502 Hero Driver");