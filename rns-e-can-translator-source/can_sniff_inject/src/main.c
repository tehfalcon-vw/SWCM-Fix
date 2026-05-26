#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#define RX_THREAD_STACK_SIZE 2048
#define RX_THREAD_PRIORITY 5
#define RX_QUEUE_DEPTH 128
#define POLL_INTERVAL K_SECONDS(2)

#define VW_MFSW_CAN_ID 0x5C1U
#define RNSE_MFSW_CAN_ID 0x5C3U

#define VW_BUTTON_MODE 0x01U
#define VW_BUTTON_PLAY_PAUSE 0x02U
#define VW_BUTTON_VOICE 0x04U
#define VW_BUTTON_MUTE 0x08U

#define RNSE_PREV 0x02U
#define RNSE_NEXT 0x03U
#define RNSE_VOLUME_UP 0x06U
#define RNSE_VOLUME_DOWN 0x07U

CAN_MSGQ_DEFINE(rx_msgq, RX_QUEUE_DEPTH);
K_THREAD_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_SIZE);

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static struct k_thread rx_thread_data;
static bool sniff_enabled;
static bool translate_enabled = true;
static int64_t last_translate_ms;
static uint32_t rx_count;
static uint32_t tx_count;
static uint32_t tx_fail_count;
static uint32_t translated_count;

#define TRANSLATE_COOLDOWN_MS 350
#define RNSE_PRESS_MS 120

static const char *state_name(enum can_state state)
{
	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		return "active";
	case CAN_STATE_ERROR_WARNING:
		return "warning";
	case CAN_STATE_ERROR_PASSIVE:
		return "passive";
	case CAN_STATE_BUS_OFF:
		return "bus-off";
	case CAN_STATE_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

static void print_frame(const struct can_frame *frame)
{
	if ((frame->flags & CAN_FRAME_IDE) != 0U) {
		printk("C:T%08X%X", frame->id, frame->dlc);
	} else {
		printk("C:t%03X%X", frame->id, frame->dlc);
	}

	for (uint8_t i = 0; i < frame->dlc; i++) {
		printk("%02X", frame->data[i]);
	}

	printk("\n");
}

static int send_std_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
	struct can_frame frame = {
		.id = id,
		.dlc = dlc,
		.flags = 0U,
	};
	int ret;

	memcpy(frame.data, data, dlc);
	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
	if (ret != 0) {
		tx_fail_count++;
		return ret;
	}

	tx_count++;
	return 0;
}

static bool is_5c1_press(const struct can_frame *frame, uint8_t button)
{
	if ((frame->flags & (CAN_FRAME_IDE | CAN_FRAME_RTR)) != 0U ||
	    frame->id != VW_MFSW_CAN_ID || frame->dlc != 4U) {
		return false;
	}

	return frame->data[0] == button && frame->data[1] == 0x00 &&
	       frame->data[2] == 0x00 && frame->data[3] == 0x01;
}

static void send_rnse_payload(const char *name, const uint8_t *press, const uint8_t *release)
{
	if (send_std_frame(RNSE_MFSW_CAN_ID, press, 2U) == 0) {
		translated_count++;
		printk("X:%s -> t5C32%02X%02X\n", name, press[0], press[1]);
		k_msleep(100);
		(void)send_std_frame(RNSE_MFSW_CAN_ID, press, 2U);
		k_msleep(RNSE_PRESS_MS);
		if (send_std_frame(RNSE_MFSW_CAN_ID, release, 2U) == 0) {
			printk("X:release -> t5C32%02X%02X\n", release[0], release[1]);
		}
	}
}

static void send_39_if_ready(const char *name, uint8_t code, int64_t now)
{
	uint8_t press[] = {0x39, code};
	uint8_t release[] = {0x39, 0x00};

	if (now - last_translate_ms < TRANSLATE_COOLDOWN_MS) {
		return;
	}

	last_translate_ms = now;
	send_rnse_payload(name, press, release);
}

static void translate_frame(const struct can_frame *frame)
{
	int64_t now;

	if (!translate_enabled || (frame->flags & CAN_FRAME_RTR) != 0U) {
		return;
	}

	now = k_uptime_get();

	if (is_5c1_press(frame, VW_BUTTON_MODE)) {
		send_39_if_ready("mode_volume_down", RNSE_VOLUME_DOWN, now);
		return;
	}

	if (is_5c1_press(frame, VW_BUTTON_VOICE)) {
		send_39_if_ready("voice_volume_up", RNSE_VOLUME_UP, now);
		return;
	}

	if (is_5c1_press(frame, VW_BUTTON_MUTE)) {
		send_39_if_ready("mute_track_prev", RNSE_PREV, now);
		return;
	}

	if (is_5c1_press(frame, VW_BUTTON_PLAY_PAUSE)) {
		send_39_if_ready("play_pause_track_next", RNSE_NEXT, now);
		return;
	}
}

static void rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct can_frame frame;

	while (true) {
		k_msgq_get(&rx_msgq, &frame, K_FOREVER);
		rx_count++;

		if ((frame.flags & CAN_FRAME_RTR) != 0U) {
			continue;
		}

		if (sniff_enabled) {
			print_frame(&frame);
		}

		translate_frame(&frame);
	}
}

static int parse_hex_byte(const char *text, uint8_t *out)
{
	char tmp[3] = {0};
	char *end = NULL;
	long value;

	if (strlen(text) != 2U) {
		return -EINVAL;
	}

	tmp[0] = text[0];
	tmp[1] = text[1];
	value = strtol(tmp, &end, 16);
	if (end == NULL || *end != '\0' || value < 0 || value > 0xff) {
		return -EINVAL;
	}

	*out = (uint8_t)value;
	return 0;
}

static int parse_payload(const char *hex, uint8_t *data, uint8_t *dlc)
{
	size_t len = strlen(hex);
	char byte_text[3] = {0};

	if ((len % 2U) != 0U || len > 16U) {
		return -EINVAL;
	}

	*dlc = (uint8_t)(len / 2U);
	for (uint8_t i = 0; i < *dlc; i++) {
		byte_text[0] = hex[i * 2U];
		byte_text[1] = hex[(i * 2U) + 1U];
		int ret = parse_hex_byte(byte_text, &data[i]);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int cmd_sniff(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2U) {
		shell_print(sh, "usage: sniff on|off");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		sniff_enabled = true;
	} else if (strcmp(argv[1], "off") == 0) {
		sniff_enabled = false;
	} else {
		shell_print(sh, "usage: sniff on|off");
		return -EINVAL;
	}

	shell_print(sh, "sniff %s", sniff_enabled ? "on" : "off");
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	enum can_state state;
	struct can_bus_err_cnt errors;
	int ret = can_get_state(can_dev, &state, &errors);

	if (ret != 0) {
		shell_error(sh, "can_get_state failed: %d", ret);
		return ret;
	}

	shell_print(sh, "bitrate=%d sniff=%s rx=%u tx=%u txfail=%u state=%s rxerr=%u txerr=%u",
		    CONFIG_CAN_DEFAULT_BITRATE, sniff_enabled ? "on" : "off", rx_count, tx_count,
		    tx_fail_count, state_name(state), errors.rx_err_cnt, errors.tx_err_cnt);
	shell_print(sh, "translate=%s translated=%u last_translate_ms=%lld",
		    translate_enabled ? "on" : "off", translated_count, last_translate_ms);
	return 0;
}

static int cmd_translate(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2U) {
		shell_print(sh, "usage: translate on|off");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		translate_enabled = true;
	} else if (strcmp(argv[1], "off") == 0) {
		translate_enabled = false;
	} else {
		shell_print(sh, "usage: translate on|off");
		return -EINVAL;
	}

	shell_print(sh, "translate %s", translate_enabled ? "on" : "off");
	return 0;
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	struct can_frame frame = {0};
	char *end = NULL;
	unsigned long id;
	int ret;

	if (argc != 3U) {
		shell_print(sh, "usage: send <std-id-hex> <data-hex>");
		shell_print(sh, "example: send 5C1 01020304");
		return -EINVAL;
	}

	id = strtoul(argv[1], &end, 16);
	if (end == NULL || *end != '\0' || id > CAN_STD_ID_MASK) {
		shell_error(sh, "standard CAN ID must be 0..7FF hex");
		return -EINVAL;
	}

	frame.id = (uint32_t)id;
	ret = parse_payload(argv[2], frame.data, &frame.dlc);
	if (ret != 0) {
		shell_error(sh, "payload must be 0..8 bytes as even-length hex");
		return ret;
	}

	ret = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
	if (ret != 0) {
		tx_fail_count++;
		shell_error(sh, "send failed: %d", ret);
		return ret;
	}

	tx_count++;
	shell_print(sh, "sent t%03X%X%s", frame.id, frame.dlc, argv[2]);
	return 0;
}

SHELL_CMD_ARG_REGISTER(sniff, NULL, "Enable/disable live CAN prints: sniff on|off", cmd_sniff, 2, 0);
SHELL_CMD_ARG_REGISTER(stats, NULL, "Show CAN counters/state", cmd_stats, 1, 0);
SHELL_CMD_ARG_REGISTER(send, NULL, "Send classic standard CAN frame: send <id> <hexdata>", cmd_send, 3, 0);
SHELL_CMD_ARG_REGISTER(translate, NULL, "Enable/disable button translation: translate on|off",
		       cmd_translate, 2, 0);

int main(void)
{
	const struct can_filter std_filter = {
		.flags = 0U,
		.id = 0U,
		.mask = 0U,
	};
	const struct can_filter ext_filter = {
		.flags = CAN_FILTER_IDE,
		.id = 0U,
		.mask = 0U,
	};
	int ret;

	printk("\nSTM32 RNS-E CAN translator 1.0, classic CAN %d bps\n", CONFIG_CAN_DEFAULT_BITRATE);

	if (!device_is_ready(can_dev)) {
		printk("CAN device %s is not ready\n", can_dev->name);
		return 0;
	}

	ret = can_set_bitrate(can_dev, CONFIG_CAN_DEFAULT_BITRATE);
	if (ret != 0) {
		printk("can_set_bitrate failed: %d\n", ret);
		return 0;
	}

	ret = can_add_rx_filter_msgq(can_dev, &rx_msgq, &std_filter);
	if (ret < 0) {
		printk("std rx filter failed: %d\n", ret);
		return 0;
	}

	ret = can_add_rx_filter_msgq(can_dev, &rx_msgq, &ext_filter);
	if (ret < 0) {
		printk("ext rx filter failed: %d\n", ret);
		return 0;
	}

	ret = can_start(can_dev);
	if (ret != 0) {
		printk("can_start failed: %d\n", ret);
		return 0;
	}

	k_thread_create(&rx_thread_data, rx_thread_stack, K_THREAD_STACK_SIZEOF(rx_thread_stack),
			rx_thread, NULL, NULL, NULL, RX_THREAD_PRIORITY, 0, K_NO_WAIT);

	printk("Ready. Commands: stats, sniff on, sniff off, translate on, translate off, send <id> <hexdata>\n");

	while (true) {
		enum can_state state;
		struct can_bus_err_cnt errors;

		ret = can_get_state(can_dev, &state, &errors);
		if (ret == 0 && state == CAN_STATE_BUS_OFF) {
			printk("CAN bus-off, waiting for auto-recovery; rxerr=%u txerr=%u\n",
			       errors.rx_err_cnt, errors.tx_err_cnt);
		}
		k_sleep(POLL_INTERVAL);
	}
}
