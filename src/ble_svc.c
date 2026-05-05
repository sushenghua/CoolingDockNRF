#include "ble_svc.h"
#include "cmd_interpreter.h"
#include "json_io.h"
#include "sys_data.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_svc, LOG_LEVEL_INF);

/* ----------------------------------------------------------- UUIDs
 *
 * Wire-compatible with the React frontend at react_projects/CoolingDock.
 *   Service:  59462f12-9543-9999-12c8-58b459a2712d
 *   Status:   33333333-2222-2222-1111-111100000001  R + N (encrypted)
 *   Command:  33333333-2222-2222-1111-111100000002  W     (encrypted)
 *   DevInfo:  33333333-2222-2222-1111-111100000004  R     (encrypted)
 *   PrpConf:  33333333-2222-2222-1111-111100000005  R     (encrypted)
 *
 * The frontend never subscribes to 0x...0003; UpdateRet replies are sent
 * via the STATUS notify channel instead (the client demuxes by `cmd`).
 */

#define UUID_SVC_VAL \
	BT_UUID_128_ENCODE(0x59462f12, 0x9543, 0x9999, 0x12c8, 0x58b459a2712dULL)
#define UUID_STATUS_VAL \
	BT_UUID_128_ENCODE(0x33333333, 0x2222, 0x2222, 0x1111, 0x111100000001ULL)
#define UUID_CMD_VAL \
	BT_UUID_128_ENCODE(0x33333333, 0x2222, 0x2222, 0x1111, 0x111100000002ULL)
#define UUID_DEVINFO_VAL \
	BT_UUID_128_ENCODE(0x33333333, 0x2222, 0x2222, 0x1111, 0x111100000004ULL)
#define UUID_PRPCONF_VAL \
	BT_UUID_128_ENCODE(0x33333333, 0x2222, 0x2222, 0x1111, 0x111100000005ULL)

static const struct bt_uuid_128 uuid_svc     = BT_UUID_INIT_128(UUID_SVC_VAL);
static const struct bt_uuid_128 uuid_status  = BT_UUID_INIT_128(UUID_STATUS_VAL);
static const struct bt_uuid_128 uuid_cmd     = BT_UUID_INIT_128(UUID_CMD_VAL);
static const struct bt_uuid_128 uuid_devinfo = BT_UUID_INIT_128(UUID_DEVINFO_VAL);
static const struct bt_uuid_128 uuid_prpconf = BT_UUID_INIT_128(UUID_PRPCONF_VAL);

/* ------------------------------------------------------- adv data */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID_SVC_VAL),
};

/* Connectable, accepts any peer — used during the open pairing window. */
#define ADV_OPEN_PARAM       BT_LE_ADV_CONN_FAST_1

/* Connectable, but Filter Accept List restricts which peers can connect —
 * used after the open window expires (or after a successful pairing). */
#define ADV_BONDED_ONLY_PARAM \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_FILTER_CONN, \
			BT_GAP_ADV_FAST_INT_MIN_2, \
			BT_GAP_ADV_FAST_INT_MAX_2, NULL)

/* ----------------------------------------------------- runtime state */

#define CMD_BUF_SIZE         512
#define RESP_BUF_SIZE        512
#define STATUS_BUF_SIZE      384
#define NOTIFY_PERIOD_MS     500
#define NOTIFY_STACK_SIZE    2048
#define CMD_STACK_SIZE       2048

#define PAIRING_WINDOW_MS    (120 * 1000)
#define PAIRING_BTN_HOLD_MS  5000

enum adv_mode { ADV_OPEN = 0, ADV_BONDED_ONLY = 1 };

static atomic_t adv_mode_v    = ATOMIC_INIT(ADV_OPEN);
static atomic_t is_connected   = ATOMIC_INIT(0);
static atomic_t notify_enabled = ATOMIC_INIT(0);
static struct bt_conn *current_conn;

static const struct bt_gatt_attr *attr_status_value;

/* ------------------------------------------------ command-dispatch queue */

struct cmd_msg {
	uint16_t len;
	char     buf[CMD_BUF_SIZE];
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd_msg), 4, 4);

static struct {
	char     buf[CMD_BUF_SIZE];
	size_t   pos;
	int      depth;
	bool     in_string;
	bool     escape_next;
} acc;

static void acc_reset(void)
{
	acc.pos = 0;
	acc.depth = 0;
	acc.in_string = false;
	acc.escape_next = false;
}

/* ============================================================
 *                     Pairing window
 * ============================================================
 *
 * Boot         → mode = OPEN, schedule 120 s timer
 * Pair OK      → mode = BONDED_ONLY, cancel timer (locked in)
 * Timer fires  → mode = BONDED_ONLY (no bond yet → no one can connect
 *                until BUTTON3 is held to reopen the window)
 * BUTTON3 held → mode = OPEN, reschedule 120 s timer, re-arm adv
 */

static void pairing_window_expired(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(pairing_window_work, pairing_window_expired);

/* -------------------------------------- Filter Accept List helpers */

static void fal_add_one(const struct bt_bond_info *info, void *user_data)
{
	int *count = user_data;
	int rc = bt_le_filter_accept_list_add(&info->addr);
	if (rc) {
		LOG_WRN("FAL add: %d", rc);
	} else {
		(*count)++;
	}
}

/* Returns number of bonded peers added to the FAL. */
static int fal_repopulate(void)
{
	int rc = bt_le_filter_accept_list_clear();
	if (rc) {
		LOG_WRN("FAL clear: %d", rc);
	}

	int count = 0;
	bt_foreach_bond(BT_ID_DEFAULT, fal_add_one, &count);
	return count;
}

/* ---------------------------------------- start advertising
 *
 * Stops first (idempotent), then starts in whichever mode adv_mode_v
 * currently indicates. Safe to call from any thread.
 */
static int start_advertising(void)
{
	(void)bt_le_adv_stop();   /* best effort; -EALREADY is fine */

	if (atomic_get(&adv_mode_v) == ADV_BONDED_ONLY) {
		int n = fal_repopulate();
		if (n == 0) {
			LOG_WRN("BONDED_ONLY: no bonds → not advertising. "
				"Hold BUTTON3 for 5 s to reopen pairing window.");
			return -ENOENT;
		}
		int rc = bt_le_adv_start(ADV_BONDED_ONLY_PARAM,
					 ad, ARRAY_SIZE(ad),
					 sd, ARRAY_SIZE(sd));
		if (rc) {
			LOG_ERR("adv start (bonded-only): %d", rc);
		} else {
			LOG_INF("adv: BONDED_ONLY (%d bond%s in FAL)",
				n, n == 1 ? "" : "s");
		}
		return rc;
	}

	int rc = bt_le_adv_start(ADV_OPEN_PARAM,
				 ad, ARRAY_SIZE(ad),
				 sd, ARRAY_SIZE(sd));
	if (rc) {
		LOG_ERR("adv start (open): %d", rc);
	} else {
		LOG_INF("adv: OPEN (any client may pair, %u s window)",
			PAIRING_WINDOW_MS / 1000);
	}
	return rc;
}

/* ---------------------------------------- mode transitions */

static void enter_open_window(void)
{
	atomic_set(&adv_mode_v, ADV_OPEN);
	(void)k_work_reschedule(&pairing_window_work, K_MSEC(PAIRING_WINDOW_MS));

	/* If currently connected, the new mode takes effect on disconnect.
	 * Otherwise re-arm adv right now. */
	if (!atomic_get(&is_connected)) {
		(void)start_advertising();
	} else {
		LOG_INF("pairing window reopened; new mode applies after current peer disconnects");
	}
}

static void enter_bonded_only(void)
{
	atomic_set(&adv_mode_v, ADV_BONDED_ONLY);
	(void)k_work_cancel_delayable(&pairing_window_work);

	if (!atomic_get(&is_connected)) {
		(void)start_advertising();
	}
}

static void pairing_window_expired(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("pairing window expired");
	enter_bonded_only();
}

/* ---------------------------------------- pairing-complete callback */

static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	if (bonded) {
		LOG_INF("client bonded → closing pairing window");
		enter_bonded_only();
	}
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete_cb,
};

/* ============================================================
 *                  BUTTON3 long-press detector
 * ============================================================ */

static const struct gpio_dt_spec pairing_btn =
	GPIO_DT_SPEC_GET(DT_ALIAS(pairing_btn), gpios);
static struct gpio_callback btn_cb_data;

static void btn_long_press(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(btn_hold_work, btn_long_press);

static void btn_long_press(struct k_work *w)
{
	ARG_UNUSED(w);
	/* Re-check that the button is still held; defends against bounce. */
	if (gpio_pin_get_dt(&pairing_btn) != 1) {
		return;
	}
	LOG_INF("BUTTON3 held %d ms → reopening pairing window",
		PAIRING_BTN_HOLD_MS);
	enter_open_window();
}

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);

	int v = gpio_pin_get_dt(&pairing_btn);
	if (v == 1) {
		/* Pressed (active-low pin reads 0; logical 1 = active). */
		(void)k_work_reschedule(&btn_hold_work,
					K_MSEC(PAIRING_BTN_HOLD_MS));
	} else {
		(void)k_work_cancel_delayable(&btn_hold_work);
	}
}

static int pairing_btn_init(void)
{
	if (!gpio_is_ready_dt(&pairing_btn)) {
		LOG_ERR("pairing button GPIO not ready");
		return -ENODEV;
	}
	int rc = gpio_pin_configure_dt(&pairing_btn, GPIO_INPUT);
	if (rc) return rc;
	rc = gpio_pin_interrupt_configure_dt(&pairing_btn, GPIO_INT_EDGE_BOTH);
	if (rc) return rc;
	gpio_init_callback(&btn_cb_data, btn_isr, BIT(pairing_btn.pin));
	rc = gpio_add_callback(pairing_btn.port, &btn_cb_data);
	return rc;
}

/* ============================================================
 *               GATT handlers (unchanged from before)
 * ============================================================ */

static int status_notify(const char *payload, size_t len)
{
	if (!atomic_get(&is_connected) || !atomic_get(&notify_enabled)) {
		return -ENOTCONN;
	}
	return bt_gatt_notify(NULL, attr_status_value, payload, len);
}

static ssize_t read_devinfo(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	char tmp[256];
	ssize_t n = cmd_interpreter_dispatch("{\"cmd\":\"GetDeviceInfo\"}",
					sizeof("{\"cmd\":\"GetDeviceInfo\"}") - 1,
					tmp, sizeof(tmp));
	if (n < 0) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, tmp, (size_t)n);
}

static ssize_t read_prpconf(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	char tmp[256];
	ssize_t n = cmd_interpreter_dispatch("{\"cmd\":\"GetPeripheralSetsConfig\"}",
					sizeof("{\"cmd\":\"GetPeripheralSetsConfig\"}") - 1,
					tmp, sizeof(tmp));
	if (n < 0) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, tmp, (size_t)n);
}

static ssize_t read_status(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	char tmp[STATUS_BUF_SIZE];
	ssize_t n = cmd_interpreter_dispatch("{\"cmd\":\"GetStatusData\"}",
					sizeof("{\"cmd\":\"GetStatusData\"}") - 1,
					tmp, sizeof(tmp));
	if (n < 0) return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, tmp, (size_t)n);
}

static ssize_t write_cmd(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len,
			 uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

	const char *data = buf;

	for (uint16_t i = 0; i < len; i++) {
		if (acc.pos >= sizeof(acc.buf) - 1) {
			LOG_ERR("cmd accumulator overflow; resetting");
			acc_reset();
			return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
		}

		char c = data[i];
		acc.buf[acc.pos++] = c;

		if (acc.escape_next) { acc.escape_next = false; continue; }
		if (acc.in_string) {
			if      (c == '\\') acc.escape_next = true;
			else if (c == '"')  acc.in_string   = false;
			continue;
		}
		if (c == '"') { acc.in_string = true;  continue; }
		if (c == '{') acc.depth++;
		else if (c == '}') {
			acc.depth--;
			if (acc.depth == 0 && acc.pos > 0) {
				acc.buf[acc.pos] = '\0';
				struct cmd_msg msg;
				msg.len = (uint16_t)acc.pos;
				memcpy(msg.buf, acc.buf, acc.pos + 1);
				if (k_msgq_put(&cmd_q, &msg, K_NO_WAIT) != 0) {
					LOG_WRN("cmd_q full; dropping");
				}
				acc_reset();
			}
		}
	}
	return len;
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&notify_enabled, value == BT_GATT_CCC_NOTIFY ? 1 : 0);
	LOG_INF("status notify %s",
		value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(coolingdock_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),

	BT_GATT_CHARACTERISTIC(&uuid_status.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ_ENCRYPT,
		read_status, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),

	BT_GATT_CHARACTERISTIC(&uuid_cmd.uuid,
		BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE_ENCRYPT,
		NULL, write_cmd, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_devinfo.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ_ENCRYPT,
		read_devinfo, NULL, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_prpconf.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ_ENCRYPT,
		read_prpconf, NULL, NULL),
);

#define ATTR_STATUS_VALUE_IDX  2

/* ============================================================
 *                  Connection lifecycle
 * ============================================================ */

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("connect failed: 0x%02x", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	atomic_set(&is_connected, 1);
	LOG_INF("connected (mode=%s)",
		atomic_get(&adv_mode_v) == ADV_OPEN ? "OPEN" : "BONDED_ONLY");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_set(&is_connected, 0);
	atomic_set(&notify_enabled, 0);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	acc_reset();
	LOG_INF("disconnected: 0x%02x", reason);

	(void)start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected_cb,
	.disconnected = disconnected_cb,
};

/* ============================================================
 *           Status push thread + command worker thread
 * ============================================================ */

static K_THREAD_STACK_DEFINE(notify_stack, NOTIFY_STACK_SIZE);
static struct k_thread notify_tcb;

static void notify_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	char buf[STATUS_BUF_SIZE];

	while (1) {
		k_msleep(NOTIFY_PERIOD_MS);

		if (!atomic_get(&is_connected) || !atomic_get(&notify_enabled)) continue;

		ssize_t n = cmd_interpreter_dispatch("{\"cmd\":\"GetStatusData\"}",
						sizeof("{\"cmd\":\"GetStatusData\"}") - 1,
						buf, sizeof(buf));
		if (n > 0) (void)status_notify(buf, (size_t)n);
	}
}

static K_THREAD_STACK_DEFINE(cmd_stack, CMD_STACK_SIZE);
static struct k_thread cmd_tcb;

static void cmd_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct cmd_msg msg;
	char           resp[RESP_BUF_SIZE];

	while (1) {
		k_msgq_get(&cmd_q, &msg, K_FOREVER);
		ssize_t n = cmd_interpreter_dispatch(msg.buf, msg.len, resp, sizeof(resp));
		if (n > 0) (void)status_notify(resp, (size_t)n);
	}
}

/* ============================================================
 *                       public start
 * ============================================================ */

int ble_svc_start(void)
{
	int rc = bt_enable(NULL);
	if (rc) {
		LOG_ERR("bt_enable: %d", rc);
		return rc;
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load();
	}

	rc = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (rc) {
		LOG_ERR("auth_info_cb register: %d", rc);
		return rc;
	}

	rc = pairing_btn_init();
	if (rc) {
		LOG_WRN("pairing button init failed (%d) — manual reopen unavailable", rc);
	}

	attr_status_value = &coolingdock_svc.attrs[ATTR_STATUS_VALUE_IDX];

	/* Always boot into the open pairing window (per spec). */
	atomic_set(&adv_mode_v, ADV_OPEN);
	(void)k_work_schedule(&pairing_window_work, K_MSEC(PAIRING_WINDOW_MS));

	rc = start_advertising();
	if (rc) {
		LOG_ERR("initial adv start: %d", rc);
		return rc;
	}

	k_thread_create(&notify_tcb, notify_stack, K_THREAD_STACK_SIZEOF(notify_stack),
			notify_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_name_set(&notify_tcb, "ble_notify");

	k_thread_create(&cmd_tcb, cmd_stack, K_THREAD_STACK_SIZEOF(cmd_stack),
			cmd_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_name_set(&cmd_tcb, "ble_cmd");

	return 0;
}
