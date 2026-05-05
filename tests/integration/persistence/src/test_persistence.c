/* Integration test for the cmd → sys_data → settings → reload pipeline.
 *
 * Compiled directly against the production sys_data.c, json_io.c, and
 * cmd_interpreter.c — the small slice of Zephyr API they consume is
 * shimmed in tests/integration/persistence/fakes/. The shimmed
 * settings backend is an in-memory key→bytes map; everything else
 * (the cmd dispatch, validation, JSON, sys_data wrapping) is the real
 * production code under test.
 */

#include "test_harness.h"

#include <string.h>

#include "sys_data.h"
#include "cmd_interpreter.h"
#include "zephyr/settings/settings.h"   /* for _shim_settings_reset */

extern const char *test_last_applied_name(void);

/* Most TESTs need a clean store. Easiest is to call the reset helper
 * at the top of each test. */
#define RESET_STATE() \
	do { \
		_shim_settings_reset(); \
		(void)sys_data_factory_reset(); \
		(void)sys_data_init(); \
	} while (0)

TEST(persistence_set_peripheral_sensor_persists_thresholds)
{
	RESET_STATE();
	const char *cmd =
		"{\"cmd\":\"SetPeripheralConfig\",\"index\":0,\"mode\":\"sensor\","
		"\"thr1\":20,\"thr2\":30,\"retfmt\":\"json\"}";
	char resp[128];
	ssize_t n = cmd_interpreter_dispatch(cmd, strlen(cmd), resp, sizeof(resp));
	ASSERT_TRUE(n > 0, "dispatch returned %zd", n);
	ASSERT_NOT_NULL(strstr(resp, "\"code\":0"), "resp=%s", resp);

	struct prf_cfg cfg;
	ASSERT_EQ(sys_data_get_prf(0, &cfg), 0);
	ASSERT_EQ(cfg.mode, PRF_MODE_SENSOR);
	ASSERT_EQ(cfg.thr1_c, 20);
	ASSERT_EQ(cfg.thr2_c, 30);
}

TEST(persistence_set_peripheral_survives_reboot)
{
	RESET_STATE();

	/* Apply config */
	const char *cmd =
		"{\"cmd\":\"SetPeripheralConfig\",\"index\":0,\"mode\":\"sensor\","
		"\"thr1\":18,\"thr2\":28}";
	char resp[128];
	(void)cmd_interpreter_dispatch(cmd, strlen(cmd), resp, sizeof(resp));

	/* Override with different values and persist */
	const char *override =
		"{\"cmd\":\"SetPeripheralConfig\",\"index\":0,\"mode\":\"power\","
		"\"pwr\":true}";
	(void)cmd_interpreter_dispatch(override, strlen(override), resp, sizeof(resp));

	/* "Reboot" — re-init sys_data from the persisted store */
	(void)sys_data_init();

	struct prf_cfg cfg;
	(void)sys_data_get_prf(0, &cfg);
	ASSERT_EQ(cfg.mode, PRF_MODE_POWER);
	ASSERT_TRUE(cfg.pwr);
}

TEST(persistence_master_control_round_trip)
{
	RESET_STATE();

	const char *off = "{\"cmd\":\"SetMasterControl\",\"on\":false}";
	char resp[64];
	(void)cmd_interpreter_dispatch(off, strlen(off), resp, sizeof(resp));
	ASSERT_FALSE(sys_data_get_master());

	const char *on = "{\"cmd\":\"SetMasterControl\",\"on\":true}";
	(void)cmd_interpreter_dispatch(on, strlen(on), resp, sizeof(resp));
	ASSERT_TRUE(sys_data_get_master());
}

TEST(persistence_set_device_name_persists_and_propagates)
{
	RESET_STATE();

	const char *cmd = "{\"cmd\":\"SetDeviceName\",\"name\":\"CoolingDock_X\"}";
	char resp[64];
	ssize_t n = cmd_interpreter_dispatch(cmd, strlen(cmd), resp, sizeof(resp));
	ASSERT_TRUE(n > 0);
	ASSERT_NOT_NULL(strstr(resp, "\"code\":0"));

	char name[24];
	ASSERT_EQ(sys_data_get_name(name, sizeof(name)), 0);
	ASSERT_STR_EQ(name, "CoolingDock_X");

	/* SetDeviceName must also push to BLE host via ble_svc_apply_name. */
	ASSERT_STR_EQ(test_last_applied_name(), "CoolingDock_X");
}

TEST(persistence_bad_mode_rejected_without_mutation)
{
	RESET_STATE();
	struct prf_cfg before;
	(void)sys_data_get_prf(0, &before);

	const char *bad =
		"{\"cmd\":\"SetPeripheralConfig\",\"index\":0,\"mode\":\"bogus\"}";
	char resp[64];
	(void)cmd_interpreter_dispatch(bad, strlen(bad), resp, sizeof(resp));
	ASSERT_NOT_NULL(strstr(resp, "\"code\":3"));
	ASSERT_NOT_NULL(strstr(resp, "\"val\":\"bad mode\""));

	struct prf_cfg after;
	(void)sys_data_get_prf(0, &after);
	ASSERT_EQ(memcmp(&before, &after, sizeof(before)), 0);
}

TEST(persistence_sensor_mode_thr1_ge_thr2_rejected)
{
	RESET_STATE();
	struct prf_cfg before;
	(void)sys_data_get_prf(0, &before);

	const char *bad =
		"{\"cmd\":\"SetPeripheralConfig\",\"index\":0,\"mode\":\"sensor\","
		"\"thr1\":40,\"thr2\":40}";
	char resp[64];
	(void)cmd_interpreter_dispatch(bad, strlen(bad), resp, sizeof(resp));
	ASSERT_NOT_NULL(strstr(resp, "\"val\":\"thr2<=thr1\""));

	struct prf_cfg after;
	(void)sys_data_get_prf(0, &after);
	ASSERT_EQ(memcmp(&before, &after, sizeof(before)), 0);
}

TEST(persistence_unknown_cmd_returns_unsupp)
{
	RESET_STATE();

	const char *bad = "{\"cmd\":\"FlyToTheMoon\"}";
	char resp[64];
	(void)cmd_interpreter_dispatch(bad, strlen(bad), resp, sizeof(resp));
	ASSERT_NOT_NULL(strstr(resp, "\"code\":4"));
}

TEST(persistence_get_status_does_not_crash_without_sample)
{
	RESET_STATE();

	const char *cmd = "{\"cmd\":\"GetStatusData\"}";
	char resp[256];
	ssize_t n = cmd_interpreter_dispatch(cmd, strlen(cmd), resp, sizeof(resp));
	ASSERT_TRUE(n > 0);
	/* sensor stub returns -EAGAIN, so jsf_status emits null fields. */
	ASSERT_NOT_NULL(strstr(resp, "\"temp\":null"));
	ASSERT_NOT_NULL(strstr(resp, "\"humid\":null"));
}
