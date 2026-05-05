#include "cmd_interpreter.h"
#include "sys_data.h"
#include "sensor.h"
#include "control.h"
#include "json_io.h"
#include "ble_svc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>           /* NRF_FICR */

LOG_MODULE_REGISTER(cmd_interpreter, LOG_LEVEL_INF);

#define FW_VERSION  "0.1.0"
#define BOARD_NAME  CONFIG_BOARD

/* UpdateRet `code` values that the frontend understands. The original
 * CoolingDock spec defines 0=success, 1=already-up-to-date, 12=progress.
 * We use a small disjoint set of failure codes that don't collide. */
#define URET_OK         0
#define URET_NOOP       1   /* already in requested state */
#define URET_FAIL       2   /* generic failure (I/O, write, …) */
#define URET_BAD_REQ    3   /* malformed args, missing fields, bad mode */
#define URET_UNSUPP     4   /* unknown command */

static char uid_str[17];   /* 16 hex + NUL */

/* ---------------------------------------------------------------- init */

void cmd_interpreter_init(void)
{
	uint32_t low  = NRF_FICR->DEVICEID[0];
	uint32_t high = NRF_FICR->DEVICEID[1];
	snprintf(uid_str, sizeof(uid_str), "%08x%08x",
		 (unsigned)high, (unsigned)low);
}

/* ----------------------------------------------------- mode parsing */

static int parse_mode(const char *s, uint8_t *out)
{
	if (!s) return -EINVAL;
	if (!strcmp(s, "power"))    { *out = PRF_MODE_POWER;    return 0; }
	if (!strcmp(s, "fixed"))    { *out = PRF_MODE_FIXED;    return 0; }
	if (!strcmp(s, "sensor"))   { *out = PRF_MODE_SENSOR;   return 0; }
	if (!strcmp(s, "cycle"))    { *out = PRF_MODE_CYCLE;    return 0; }
	if (!strcmp(s, "schedule")) { *out = PRF_MODE_SCHEDULE; return 0; }
	return -EINVAL;
}

/* ---------------------------------- delayed reboot for Restart commands */

static void reboot_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("rebooting now");
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

/* ------------------------------------------------ command handlers */

static ssize_t handle_get_status(char *out, size_t cap)
{
	struct prf_cfg cfg;
	struct sensor_reading r = { 0 };
	(void)sys_data_get_prf(0, &cfg);
	bool valid = (sensor_get(&r) == 0);

	return jsf_status(out, cap,
			  sys_data_get_master(), valid,
			  r.temp_centi_c, r.humid_centi_pct,
			  &cfg, control_get_actual_pwm(),
			  control_get_status());
}

static ssize_t handle_get_devinfo(char *out, size_t cap)
{
	char name[SYS_DEVICE_NAME_MAX];
	(void)sys_data_get_name(name, sizeof(name));
	return jsf_devinfo(out, cap, name, FW_VERSION, uid_str, BOARD_NAME);
}

static ssize_t handle_get_prpconf(char *out, size_t cap)
{
	struct prf_cfg cfg;
	(void)sys_data_get_prf(0, &cfg);
	return jsf_prpconf(out, cap, &cfg);
}

static ssize_t handle_set_peripheral(const char *json, char *out, size_t cap)
{
	int32_t idx = 0;
	(void)jsp_get_int(json, "index", &idx);
	if (idx < 0 || idx >= SYS_PRF_COUNT) {
		return jsf_updateret(out, cap, URET_BAD_REQ, "bad index");
	}

	struct prf_cfg cfg;
	if (sys_data_get_prf((uint8_t)idx, &cfg) != 0) {
		return jsf_updateret(out, cap, URET_FAIL, NULL);
	}

	char mode_s[16] = {0};
	if (jsp_get_str(json, "mode", mode_s, sizeof(mode_s)) != 0 ||
	    parse_mode(mode_s, &cfg.mode) != 0) {
		return jsf_updateret(out, cap, URET_BAD_REQ, "bad mode");
	}

	int32_t i32;
	bool    b;

	switch (cfg.mode) {
	case PRF_MODE_POWER:
		if (jsp_get_bool(json, "pwr", &b) == 0) cfg.pwr = b;
		break;

	case PRF_MODE_FIXED:
		if (jsp_get_int(json, "pwm", &i32) == 0) {
			if (i32 < 0)   i32 = 0;
			if (i32 > 100) i32 = 100;
			cfg.pwm_pct = (uint8_t)i32;
		}
		break;

	case PRF_MODE_SENSOR:
		if (jsp_get_int(json, "thr1", &i32) == 0) cfg.thr1_c = (int16_t)i32;
		if (jsp_get_int(json, "thr2", &i32) == 0) cfg.thr2_c = (int16_t)i32;
		if (cfg.thr2_c <= cfg.thr1_c) {
			return jsf_updateret(out, cap, URET_BAD_REQ, "thr2<=thr1");
		}
		break;

	case PRF_MODE_CYCLE:
		if (jsp_get_int(json, "pwm",  &i32) == 0) {
			if (i32 < 0)   i32 = 0;
			if (i32 > 100) i32 = 100;
			cfg.pwm_pct = (uint8_t)i32;
		}
		if (jsp_get_int(json, "con",  &i32) == 0 && i32 >= 0)
			cfg.con_sec  = (uint16_t)i32;
		if (jsp_get_int(json, "coff", &i32) == 0 && i32 >= 0)
			cfg.coff_sec = (uint16_t)i32;
		break;

	case PRF_MODE_SCHEDULE:
		if (jsp_get_int(json, "ont",  &i32) == 0 && i32 >= 0)
			cfg.ont_sec  = (uint32_t)i32;
		if (jsp_get_int(json, "offt", &i32) == 0 && i32 >= 0)
			cfg.offt_sec = (uint32_t)i32;
		LOG_WRN("schedule mode persisted but no RTC -> fan stays off");
		break;
	}

	int rc = sys_data_set_prf((uint8_t)idx, &cfg);
	return jsf_updateret(out, cap, rc == 0 ? URET_OK : URET_FAIL, NULL);
}

static ssize_t handle_set_master(const char *json, char *out, size_t cap)
{
	bool on;
	/* Frontend sends `{cmd: "SetMasterControl", on: <bool>}` (verified
	 * against react_projects/CoolingDock/src/App.tsx). */
	if (jsp_get_bool(json, "on", &on) != 0) {
		return jsf_updateret(out, cap, URET_BAD_REQ, "missing on");
	}
	int rc = sys_data_set_master(on);
	return jsf_updateret(out, cap, rc == 0 ? URET_OK : URET_FAIL, NULL);
}

static ssize_t handle_set_name(const char *json, char *out, size_t cap)
{
	char name[SYS_DEVICE_NAME_MAX];
	if (jsp_get_str(json, "name", name, sizeof(name)) != 0) {
		return jsf_updateret(out, cap, URET_BAD_REQ, "missing name");
	}
	int rc = sys_data_set_name(name);
	if (rc == 0) {
		/* Push the new name into the BLE host so adv + GAP reflect it. */
		(void)ble_svc_apply_name(name);
	}
	return jsf_updateret(out, cap, rc == 0 ? URET_OK : URET_FAIL, NULL);
}

static ssize_t handle_restart(char *out, size_t cap)
{
	k_work_schedule(&reboot_work, K_MSEC(500));
	return jsf_updateret(out, cap, URET_OK, "rebooting");
}

static ssize_t handle_factory(char *out, size_t cap)
{
	int rc = sys_data_factory_reset();
	if (rc == 0) k_work_schedule(&reboot_work, K_MSEC(500));
	return jsf_updateret(out, cap, rc == 0 ? URET_OK : URET_FAIL, "factory reset");
}

/* ----------------------------------------------------- entry point */

ssize_t cmd_interpreter_dispatch(const char *json_in, size_t in_len,
			    char *json_out, size_t out_cap)
{
	ARG_UNUSED(in_len);
	if (!json_in || !json_out || out_cap == 0) return -EINVAL;

	char cmd[40];
	if (jsp_get_str(json_in, "cmd", cmd, sizeof(cmd)) != 0) {
		return jsf_updateret(json_out, out_cap, URET_BAD_REQ, "no cmd");
	}

	LOG_DBG("dispatch cmd=%s", cmd);

	if (!strcmp(cmd, "GetStatusData"))            return handle_get_status(json_out, out_cap);
	if (!strcmp(cmd, "GetDeviceInfo"))            return handle_get_devinfo(json_out, out_cap);
	if (!strcmp(cmd, "GetPeripheralSetsConfig")) return handle_get_prpconf(json_out, out_cap);
	if (!strcmp(cmd, "SetPeripheralConfig"))     return handle_set_peripheral(json_in, json_out, out_cap);
	if (!strcmp(cmd, "SetMasterControl"))        return handle_set_master(json_in, json_out, out_cap);
	if (!strcmp(cmd, "SetDeviceName"))           return handle_set_name(json_in, json_out, out_cap);
	if (!strcmp(cmd, "Restart"))                 return handle_restart(json_out, out_cap);
	if (!strcmp(cmd, "RestoreFactory"))          return handle_factory(json_out, out_cap);

	LOG_WRN("unknown cmd: %s", cmd);
	return jsf_updateret(json_out, out_cap, URET_UNSUPP, "unknown cmd");
}
