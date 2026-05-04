#ifndef APP_CONTROL_H_
#define APP_CONTROL_H_

#include <stdint.h>

/* Spawn the temperature-control thread. Call after fan_init() and
 * sensor_start(). */
void control_start(void);

/* Snapshot of the actual fan output the control loop last commanded.
 * Status notifications report this so the frontend sees the truth, not the
 * configured setpoint. */
uint8_t control_get_actual_pwm(void);

/* "running" / "stopped" / "unknown" — string consumed by status JSON. */
const char *control_get_status(void);

#endif /* APP_CONTROL_H_ */
