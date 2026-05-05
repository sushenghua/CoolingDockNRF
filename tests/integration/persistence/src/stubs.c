/* Stubs for the headers cmd_interpreter pulls in but we can't link
 * against in this test (sensor needs SHT3x driver, control needs the
 * fan PWM hardware, ble_svc needs the BT host). The stubs return
 * deterministic values so cmd_interpreter behaves predictably.
 */

#include <errno.h>
#include <stddef.h>
#include "sensor.h"
#include "control.h"
#include "ble_svc.h"

/* sensor.h */
int sensor_get(struct sensor_reading *out)
{
	(void)out;
	return -EAGAIN;            /* "no sample yet" — status JSON shows null */
}

void sensor_start(void)        { /* no-op */ }

/* control.h */
uint8_t     control_get_actual_pwm(void) { return 0; }
const char *control_get_status(void)     { return "stopped"; }
void        control_start(void)          { /* no-op */ }

/* ble_svc.h — record name applications so tests can assert on them */
static char last_applied_name[64];

int ble_svc_apply_name(const char *name)
{
	if (!name) return -EINVAL;
	size_t i = 0;
	while (name[i] && i < sizeof(last_applied_name) - 1) {
		last_applied_name[i] = name[i];
		i++;
	}
	last_applied_name[i] = '\0';
	return 0;
}

const char *test_last_applied_name(void)
{
	return last_applied_name;
}

int ble_svc_start(void) { return 0; }
