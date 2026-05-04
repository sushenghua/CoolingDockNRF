#ifndef APP_FAN_H_
#define APP_FAN_H_

#include <stdint.h>
#include <stdbool.h>

/* Configure PWM + GPIO. Call once from main(). */
int  fan_init(void);

/* Set duty cycle 0..100 %. Idempotent; safe to call from any thread. */
int  fan_set_percent(uint8_t pct);

/* Drive the fan-power gate GPIO. */
int  fan_power_set(bool on);

/* Last value passed to fan_set_percent (best-effort, not under mutex). */
uint8_t fan_get_percent(void);

#endif /* APP_FAN_H_ */
