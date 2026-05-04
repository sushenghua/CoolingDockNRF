#include "sys_data.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sys_data, LOG_LEVEL_INF);

#define KEY_MASTER  "ms"
#define KEY_NAME    "name"
#define KEY_PRF0    "p0"

static K_MUTEX_DEFINE(state_mu);

static struct {
	bool            master;
	char            name[SYS_DEVICE_NAME_MAX];
	struct prf_cfg  prf[SYS_PRF_COUNT];
} state = {
	.master = true,
	.name   = CONFIG_BT_DEVICE_NAME,
	.prf = { {
		.mode    = PRF_MODE_SENSOR,
		.pwr     = false,
		.thr1_c  = 25,
		.thr2_c  = 32,
		.pwm_pct = 50,
		.con_sec = 10,
		.coff_sec = 10,
		.ont_sec = 0,
		.offt_sec = 0,
	} },
};

/* ----------------------------------------------------- settings handler */

static int sys_set_cb(const char *key, size_t len,
		      settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int name_len = settings_name_next(key, &next);

	if (name_len == 0) {
		return -ENOENT;
	}

	k_mutex_lock(&state_mu, K_FOREVER);
	int rc = -ENOENT;

	if (!strncmp(key, KEY_MASTER, name_len)) {
		rc = read_cb(cb_arg, &state.master, sizeof(state.master));
	} else if (!strncmp(key, KEY_NAME, name_len)) {
		size_t take = MIN(len, sizeof(state.name) - 1);
		rc = read_cb(cb_arg, state.name, take);
		if (rc >= 0) {
			state.name[take] = '\0';
		}
	} else if (!strncmp(key, KEY_PRF0, name_len)) {
		rc = read_cb(cb_arg, &state.prf[0], sizeof(state.prf[0]));
	}

	k_mutex_unlock(&state_mu);
	return (rc >= 0) ? 0 : rc;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_settings, "app", NULL, sys_set_cb, NULL, NULL);

/* ---------------------------------------------------------------- init */

int sys_data_init(void)
{
	int rc = settings_load_subtree("app");
	if (rc) {
		LOG_WRN("settings_load_subtree(app) -> %d (using defaults)", rc);
	}
	LOG_INF("loaded: master=%d name=\"%s\" prf0.mode=%u thr=(%d,%d)",
		state.master, state.name,
		state.prf[0].mode, state.prf[0].thr1_c, state.prf[0].thr2_c);
	return rc;
}

/* ----------------------------------------------------- master switch */

bool sys_data_get_master(void)
{
	bool v;
	k_mutex_lock(&state_mu, K_FOREVER);
	v = state.master;
	k_mutex_unlock(&state_mu);
	return v;
}

int sys_data_set_master(bool on)
{
	k_mutex_lock(&state_mu, K_FOREVER);
	state.master = on;
	int rc = settings_save_one("app/" KEY_MASTER, &state.master, sizeof(state.master));
	k_mutex_unlock(&state_mu);
	if (rc) LOG_ERR("save master: %d", rc);
	return rc;
}

/* --------------------------------------------------------- device name */

const char *sys_data_get_name(void)
{
	return state.name;   /* writes are atomic enough for read-mostly use */
}

int sys_data_set_name(const char *name)
{
	if (!name) return -EINVAL;
	size_t len = strnlen(name, SYS_DEVICE_NAME_MAX);
	if (len == 0 || len >= SYS_DEVICE_NAME_MAX) return -EINVAL;

	k_mutex_lock(&state_mu, K_FOREVER);
	memcpy(state.name, name, len);
	state.name[len] = '\0';
	int rc = settings_save_one("app/" KEY_NAME, state.name, len);
	k_mutex_unlock(&state_mu);
	if (rc) LOG_ERR("save name: %d", rc);
	return rc;
}

/* ---------------------------------------------------- peripheral cfg */

int sys_data_get_prf(uint8_t idx, struct prf_cfg *out)
{
	if (idx >= SYS_PRF_COUNT || !out) return -EINVAL;
	k_mutex_lock(&state_mu, K_FOREVER);
	*out = state.prf[idx];
	k_mutex_unlock(&state_mu);
	return 0;
}

int sys_data_set_prf(uint8_t idx, const struct prf_cfg *cfg)
{
	if (idx >= SYS_PRF_COUNT || !cfg) return -EINVAL;

	k_mutex_lock(&state_mu, K_FOREVER);
	state.prf[idx] = *cfg;
	int rc = settings_save_one("app/" KEY_PRF0, &state.prf[idx], sizeof(*cfg));
	k_mutex_unlock(&state_mu);
	if (rc) LOG_ERR("save prf%u: %d", idx, rc);
	return rc;
}

/* --------------------------------------------------------- factory reset */

int sys_data_factory_reset(void)
{
	int rc1 = settings_delete("app/" KEY_MASTER);
	int rc2 = settings_delete("app/" KEY_NAME);
	int rc3 = settings_delete("app/" KEY_PRF0);

	k_mutex_lock(&state_mu, K_FOREVER);
	state.master = true;
	strncpy(state.name, CONFIG_BT_DEVICE_NAME, sizeof(state.name) - 1);
	state.name[sizeof(state.name) - 1] = '\0';
	state.prf[0] = (struct prf_cfg){
		.mode = PRF_MODE_SENSOR, .pwr = false,
		.thr1_c = 25, .thr2_c = 32, .pwm_pct = 50,
		.con_sec = 10, .coff_sec = 10,
	};
	k_mutex_unlock(&state_mu);

	return (rc1 || rc2 || rc3) ? -EIO : 0;
}
