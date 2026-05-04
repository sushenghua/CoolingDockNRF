#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include "sys_data.h"
#include "sensor.h"
#include "fan.h"
#include "control.h"
#include "cmd_interpreter.h"
#include "ble_svc.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	int rc;

	LOG_INF("CoolingDock-NRF starting");

	rc = settings_subsys_init();
	if (rc) LOG_ERR("settings_subsys_init: %d", rc);

	(void)sys_data_init();      /* loads persisted overrides */
	cmd_interpreter_init();          /* caches device UID */

	rc = fan_init();
	if (rc) LOG_ERR("fan_init: %d", rc);

	sensor_start();
	control_start();

	rc = ble_svc_start();
	if (rc) LOG_ERR("ble_svc_start: %d", rc);

	LOG_INF("init complete");
	return 0;
}
