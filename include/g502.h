// SPDX-License-Identifier: GPL-2.0-only

#ifndef G502_H
#define G502_H

typedef struct {
    u8 r;
    u8 g;
    u8 b;
} RGB;

static inline RGB rgb_to_struct_rgb(unsigned int hrgb)
{
    RGB rgbcolor;

    rgbcolor.r = ((hrgb >> 16) & 0xFF);  // Extract the RR byte
    rgbcolor.g = ((hrgb >> 8) & 0xFF);   // Extract the GG byte
    rgbcolor.b = ((hrgb) & 0xFF);        // Extract the BB byte
    return rgbcolor;
}

/* This functions matches a vendor defined report rate hex
 * values to human-readable values, to be used with sysfs.
*/
static __always_inline unsigned int report_rate_htd(const u8 report_rate)
{
    switch (report_rate)
    {
    case 0x1U:      return 125U;
    case 0x2U:      return 250U;
    case 0x4U:      return 500U;
    case 0x8U:      return 1000U;
    default:        return 0U;
    }
}

/* This functions matches the vendor defined hex values that
 * represent the report rate of the device, to be used
 * with hidpp_report structure (params).
*/
static __always_inline u8 report_rate_dth(unsigned int report_rate)
{
    switch (report_rate)
    {
    case 125U:    return 0x1U;
    case 250U:    return 0x2U;
    case 500U:    return 0x4U;
    case 1000U:   return 0x8U;
    default:      return 0U;
    }
}

/* @list_next_entry_circular was only added in v5.19 */
#ifndef list_next_entry_circular
#define list_next_entry_circular(pos, head, member) \
	(list_is_last(&(pos)->member, head) ? \
	list_first_entry(head, typeof(*(pos)), member) : list_next_entry(pos, member))
#endif

/* struct usb_device */
#define	hid_to_usb_dev(hid_dev)                              \
	to_usb_device(hid_dev->dev.parent->parent)

#define LOGITECH_VENDOR_ID          0x046d
#define G502_HERO_DEVICE_ID         0xc08b
#define LINUX_KERNEL_SW_ID			0x1

/* Quirk. should be checked against firmware version */
#define G502_ON_BOARD_MEM_5_PROF_QUIRK          0x400

/* Generic informiaton */
#define G502_MAX_RESOLUTION_DPI                 25600
#define G502_MAX_PROFILES                       5

/* The first byte is the Report ID and goes for requests (output)
    and responses (input). */
#define G502_COMMAND_SHORT_REPORT_ID       		0x10U
#define G502_COMMAND_LONG_REPORT_ID             0x11U
#define G502_COMMAND_VERY_LONG_REPORT_ID        0x12U   /* Unused */
#define G502_COMMAND_VERY_LONG_SIZE             64      /* Unused */
#define G502_COMMAND_LONG_SIZE		            20
#define G502_COMMAND_SHORT_SIZE				    7

#define G502_DEVICE_INDEX_RECEIVER              0xffU

/* In the future use those to get feature as according to doc */
#define HIDPP_PAGE_ROOT_IDX                     0x00U
#define CMD_ROOT_GET_FEATURE                    0x00U

/* Features index and their functions
 * Note: These are device specific (Only index).
 * FIXME: Too long names and it bothers me, shorten them.
*/
#define G502_FEATURE_REPORT_RATE            0x0bU /* 0x8060 */
#   define G502_GET_REPORT_RATE             0x10U
#   define G502_SET_REPORT_RATE             0x20U

#define G502_MAX_DPI_VALUE           25600U
#define G502_FEATURE_DPI             0x0aU /* 0x2201 */
#   define G502_GET_DPI              0x20U
#   define G502_SET_DPI              0x03U

/* Currently we don't support mutltiple profiles,
 * so we disable it on device's probe. On/Off should be
 * used with params (index 0) */
#define G502_FEATURE_ON_BOARD_PROFILES       0x0cU
#   define G502_CONTROL_ON_BOARD_PROFILES    0x10U
#   define G502_ON_BOARD_PROFILES_ON         0x01U
#   define G502_ON_BOARD_PROFILES_OFF        0x02U

/* Control led modes, e.g.
 * 0x02 (feature index)
 * 0x03 (function index, see below)
 * led_type (set color mode to)
 * led mode described below
*/
#define G502_FEATURE_COLOR_LED_EFFECTS            0x02U /* 0x8070 */
#   define G502_CHANGE_LED_MODE                         0x30U


/* Firmware information. Firmware entity should be passed after
 * the function index, as a parameter.. We always pass 1. LMK for issues.
*/
#define G502_FEATURE_DEVICE_FW                    0x03U /* 0x0003 */
#   define G502_GET_FW_INFO                             0x10U

enum firmware_type {
    FW_MAIN_APP         = 0,
    FW_BOOTLOADER,
    FW_HARDWARE,
    FW_OPT_SENSOR       = 4,
};

/*
 * Though GHub has 6 LED Modes in it, G502 Hero only offers 4 LED modes,
 * as the other two - Screen Sampler and Audio Visualizer, just use the already defined mods,
 * and simply updates (SET_REPORT) the attributes very fast.
 *
 * As for now, Screen Sampler and Audio Visualizer aren't going to be supported.
*/
enum g502_led_mode {
	G_LED_OFF,
	G_LED_FIXED,
	G_LED_BREATHING,
	G_LED_CYCLE
};

/* We are able to set a different LED configuration for each LED of our device.
 * TODO: Implement a sync option that will set the same config for both LEDs.
*/
enum g502_led_type {
	G_LED_PRIMARY, /* That is the "DPI" Level LED */
	G_LED_LOGO    /* Logitech Icon */
};

struct g502_profile {
	struct list_head entry;
	unsigned int dev_rgb;
    u16 dev_report_rate;
	u16 dev_dpi;
	int index;
};

/* FIXME:
 * The hid-logitech-hidpp documentation mentions that @fap works only
 * with G502_COMMAND_LONG_SIZE, though it works also with
 * G502_COMMAND_SHORT_SIZE, at least in my case.
 * Though it might be undefined behaviour, so verify it.
*/
struct hidpp_report {
    u8 report_id;
    u8 device_index;
    u8 feature_index;
    u8 funcindex_clientid;
    union {
        u8 params_s[G502_COMMAND_SHORT_SIZE - 4U];
        u8 params_l[G502_COMMAND_LONG_SIZE - 4U];
    };
} __packed;

/* Struct used by work_struct API */
struct private_work_struct {
	struct hid_device *hdev;
	struct hidpp_report report;
	struct work_struct work;
};

/* This struct defines a firmware to be used with quirks */
struct gfirmware {
    enum firmware_type ftype;
    unsigned nr_entities;
    char *fwversion;
};

static void g502_hero_remove(struct hid_device *hdev);
static int g502_hero_probe(struct hid_device *dev, const struct hid_device_id *id);

#endif /* G502_H */