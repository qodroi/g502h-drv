/*
 *   Copyright (c) 2024 Roi

 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.

 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.

 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "g502.h"

struct logi_g502_data {
	struct list_head profiles_list;
	struct g502_profile *profiles[G502_MAX_PROFILES];
	struct g502_profile *current_prof; /* Pointed by index in @profiles */
	struct hid_input *hid_input;
	struct hidpp_report report;
	struct input_dev *dev_input;
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

static __always_inline void
initialize_profile(struct g502_profile *prof_ptr,
		u16 report_rate, unsigned int rgb, u16 dpi, int index)
{
	prof_ptr->dev_report_rate = report_rate;
	prof_ptr->dev_rgb = rgb;
	prof_ptr->dev_dpi = dpi;
	prof_ptr->index = index;
}

static int g502_send_report(struct hid_device *hdev,
			struct hidpp_report *report)
{
	int ret;
	size_t report_length;
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 0)
		return 0;

	ret = -ENOMEM;
	report_length = (report->report_id == G502_COMMAND_SHORT_REPORT_ID
								? G502_COMMAND_SHORT_SIZE
								: G502_COMMAND_LONG_SIZE);

	/* There may be underlying issues with the protocol itself, that
	 * are yield back in the response packet, so we should be able to
	 * examine packets in the future
	 */
	mutex_trylock(&gdv->mutex_dev);
	ret = hid_hw_raw_request(hdev, report->report_id, (u8 *)report,
			report_length, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0) {
		hid_err(hdev,
			"%s: cannot issue hid raw request (%d)\n", __func__, ret);
		goto out;
	}

	ret = 0;

out:
	mutex_unlock(&gdv->mutex_dev);
	return ret;
}

static __always_inline void refresh_report_rate(struct hid_device *hdev)
{
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);

	__do_fill_report(&gdv->report, G502_COMMAND_SHORT_REPORT_ID,
		G502_FEATURE_REPORT_RATE, G502_GET_REPORT_RATE,
		G502_COMMAND_SHORT_SIZE, NULL);
	g502_send_report(hdev, &gdv->report);
}

static __always_inline void refresh_dpi(struct hid_device *hdev)
{
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);

	__do_fill_report(&gdv->report, G502_COMMAND_SHORT_REPORT_ID,
		G502_FEATURE_DPI, G502_GET_DPI, G502_COMMAND_SHORT_SIZE, NULL);
	g502_send_report(hdev, &gdv->report);
}

/* Generic function to set current device's config to values passed as arguments.
 * Pass 0 as one of the arguments if you don't desire to change that argument.  */
static int g502_update_device_config(struct hid_device *hdev, u16 report_rate,
			u16 dpi, unsigned int rgb)
{
	u8 __maybe_unused params[G502_COMMAND_LONG_SIZE - 4U] = { 0 };
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	// RGB unused rgb_s = rgb_to_struct_rgb(rgb);

	if (report_rate)
	{
		params[0] = report_rate;
		__do_fill_report(&gdv->report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_REPORT_RATE, G502_SET_REPORT_RATE,
							G502_COMMAND_SHORT_SIZE, params);
		g502_send_report(hdev, &gdv->report);
		refresh_report_rate(hdev);
	}

	if (dpi)
	{
		params[0] = 0; /* Sensor idx */
		params[1] = (u8)((dpi >> 8) & 0xFF);
		params[2] = (u8)((dpi) & 0xFF);
		__do_fill_report(&gdv->report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_DPI, G502_SET_DPI,
							G502_COMMAND_SHORT_SIZE, params);
		g502_send_report(hdev, &gdv->report);
		refresh_dpi(hdev);
	}

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

	return 0;
}

/* Handle the regular mouse events */
static int g502_handle_regular_event(struct hid_device *hdev,
			struct input_dev *input, u8 *data)
{
	return 0;
}

// see commit fe73965
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(6, 12, 0))
static __u8 *g502_report_fixup(struct hid_device *hdev,
				__u8 *rdesc, unsigned int *rsize)
#else
static const __u8 *g502_report_fixup(struct hid_device *hdev,
				__u8 *rdesc, unsigned int *rsize)
#endif
{
	if (!hdev || !rdesc)
		return NULL;

	/* rdesc[15] = Usage Maximum, rdesc[21] = Report Count */
	if (*rsize == 67 && rdesc[15] == 16 && rdesc[21] == 16) {
		hid_info(hdev, "fixing up g502 hero report descriptor\n");
		rdesc[15] = 0x0b;

		/* hid_irq_in breaks when rdesc[21] is changed,
		 * with "input irq status -75 (E-OVERFLOW) value". I _literally_ have no clue why.
		 * For now leave it as is.
		 */

		// rdesc[21] = 0x09;
	}

	return rdesc;
}

// #define G502_USAGE_MAX			0x10
// #define DPI_SHIFT_G6			0x0a
// #define g502_map_key_clear(c)  	hid_map_usage_clear(hi, usage, bit, max, EV_KEY, (c))

// /* All buttons must be mapped correctly. We only map G6,
//  * to BTN_9 for it to be used with profile changes at @switch_profile
//  * whenever BTN_9 is pressed.
//  */
// static int g502_input_mapping(struct hid_device *hdev, struct hid_input *hi,
// 		struct hid_field *field, struct hid_usage *usage,
// 		unsigned long **bit, int *max)
// {

// 	// if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON && (usage->hid & 0xFF) == DPI_SHIFT_G6) {
// 	// 	g502_map_key_clear(BTN_TRIGGER_HAPPY1);
// 	// 	hid_info(hdev, "Mapped G6 to BTN_TRIGGER_HAPPY1\n");
// 	// 	return 1;
// 	// }

// 	// if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
// 	// 	if (usage->hid && 0xFF == 4)
// 	// 		g502_map_key_clear(KEY_POWER);
//     //         pr_info("Mapped G502 side button (Button %u) to KEY_POWER\n", usage->hid && 0xFF);
// 	//     return 1; // Handled by generic mapping
// 	// }

// 	return 0;
// }

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
	u8 function_idx;

	/* Regular events */
	if (size == 8)
		return g502_handle_regular_event(hdev, gdv->dev_input, data);

	/* We always need a LONG report */
	if (response->report_id !=
		G502_COMMAND_LONG_REPORT_ID ||
		 	size != G502_COMMAND_LONG_SIZE)
		return 1;

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

	return 0;
}

/* We define a macro to handle the attributes' show operations.
 * as it's simply the same for all. */
#define G502_ATTR_SHOW(name, feature_uppercase)			       						\
	static ssize_t name##_show(struct device *dev,			       					\
			struct device_attribute *attr, char *buf)								\
	{								       											\
		struct logi_g502_data *gdv = dev_get_drvdata(dev);							\
		int ret;																	\
																					\
		ret = sysfs_emit(buf, "%u\n", READ_ONCE(gdv->current_prof->dev_##name));	\
		return ret;																	\
	}

static ssize_t report_rate_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	unsigned int __report_rate;
	u8 report_rate;

	if (kstrtouint(buf, 0, &__report_rate))
		return -EINVAL;
	if ((report_rate = report_rate_dth(__report_rate)) == 0)
		return -EINVAL;

	g502_update_device_config(hdev, report_rate, 0, 0);
	refresh_report_rate(hdev);

	return count;
}

static ssize_t dpi_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	u16 dpi_requested;

	if (kstrtou16(buf, 0, &dpi_requested))
		return -EINVAL;
	if (0 > dpi_requested || dpi_requested > G502_MAX_RESOLUTION_DPI)
		return -EINVAL;

	g502_update_device_config(hdev, 0, dpi_requested, 0);
	refresh_dpi(hdev);

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
	u8 params[G502_COMMAND_LONG_SIZE - 4U] = { 0 };
	struct logi_g502_data *gdv = hid_get_drvdata(hdev);
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	int profile_params[G502_MAX_PROFILES][2] = {
        {1, 800},
        {2, 1600},
        {4, 2400},
        {8, 3200},
        {8, 6000}
    };

	mutex_init(&gdv->mutex_dev);
	INIT_LIST_HEAD(&gdv->profiles_list);

	gdv->hid_input = list_first_entry(&hdev->inputs, struct hid_input, list);
	gdv->dev_input = gdv->hid_input->input;

	input_set_capability(gdv->dev_input, EV_KEY, BTN_TRIGGER_HAPPY1);

	params[0] = G502_ON_BOARD_PROFILES_OFF;
	__do_fill_report(&gdv->report, G502_COMMAND_SHORT_REPORT_ID,
							G502_FEATURE_ON_BOARD_PROFILES, G502_CONTROL_ON_BOARD_PROFILES,
							G502_COMMAND_SHORT_SIZE, params);
	g502_send_report(hdev, &gdv->report);

	/* Currently, all profiles must be allocated and setup correctly. */
	for (i = 0; i < G502_MAX_PROFILES; i++) {
		gdv->profiles[i] = devm_kzalloc(&hdev->dev, sizeof(*gdv->current_prof), GFP_KERNEL | GFP_ATOMIC);
		if (!gdv->profiles[i])
			return -ENOMEM;
		list_add(&gdv->profiles[i]->entry, &gdv->profiles_list);
	}

	if (list_empty(&gdv->profiles_list))
		return -EFAULT;

	/* entry; dev_rgb; dev_report_rate; dev_dpi; index; */
	for (i = 0; i < G502_MAX_PROFILES; i++) {
		initialize_profile(gdv->profiles[i],
			profile_params[i][0], 0, profile_params[i][1], i);
	}

	gdv->current_prof = list_first_entry(&gdv->profiles_list,
							struct g502_profile, entry);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
		if (sysfs_create_group(&hdev->dev.kobj, &g502_group))
		{
			hid_err(hdev, "%s: failed to create sysfs attrs\n", __func__);
			return -EFAULT;
		}
	}

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
	// .input_mapping = g502_input_mapping,
	.report_fixup = g502_report_fixup,
	.raw_event = g502_raw_event,
};
module_hid_driver(g502_hid_driver);

/* This device registers two inputs/interfaces, one is of type Mouse and the other is Keyboard.
	G502 supports both, mouse and keyboard interface protocols,
	which enables it to not only play mouse movement and button events, but keycodes as well.
*/
static int g502_hero_probe(struct hid_device *hdev,
			const struct hid_device_id *id)
{
	int retval;
	struct logi_g502_data *gdv;

	/* Allocate memory for @hdev internal structure and assign @gdv as its private structure. */
	gdv = devm_kzalloc(&hdev->dev, sizeof(*gdv), GFP_KERNEL);
	if (!gdv) {
		hid_err(hdev, "%s: couldn't allocate memory for internal structure\n",
					__func__);
		return -ENOMEM;
	}

	hid_set_drvdata(hdev, gdv);

	/* Start parsing HW reports */
	retval = hid_parse(hdev);
	if (retval) {
		hid_err(hdev, "%s: failed to parse HID\n", __func__);
		return retval;
	}

	/* Start the device, make it work. */
	retval = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (retval) {
		hid_err(hdev, "%s: failed to start hw\n", __func__);
		return retval;
	}

	retval = g502_init_drvdata(hdev);
	if (retval < 0) {
		hid_err(hdev, "%s: device's driver data initalization failed\n",
					__func__);
		goto out_hw_stop;
	}

	hid_info(hdev, "%s: Module successfully loaded.\n", __func__);
	return 0;

out_hw_stop:
	hid_hw_stop(hdev);
	return retval;
}

static void g502_hero_remove(struct hid_device *hdev)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1)
		sysfs_remove_group(&hdev->dev.kobj, &g502_group);
	hid_hw_stop(hdev);
}

MODULE_AUTHOR("Roi L");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("hid:logitech-g502-hero");
MODULE_DESCRIPTION("HID Logitech G502 Hero Driver");
