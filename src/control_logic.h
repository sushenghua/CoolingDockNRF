#ifndef APP_CONTROL_LOGIC_H_
#define APP_CONTROL_LOGIC_H_

/* Pure-logic helpers split out of control.c so they can be unit-tested
 * on the host (no Zephyr dependencies). The control thread links them
 * via #include "control_logic.h" and uses them from inside its loop. */

#include <stdint.h>
#include "sys_data.h"   /* struct prf_cfg */

#define SENSOR_DEAD_BAND_C  2     /* °C of pure-low band above thr1 */
#define FAN_LOW_PCT         30
#define FAN_HIGH_PCT        100

/* SENSOR mode: zero below thr1, FAN_LOW_PCT in the dead band, linear
 * ramp FAN_LOW_PCT → FAN_HIGH_PCT between (thr1+SENSOR_DEAD_BAND_C) and
 * thr2, FAN_HIGH_PCT at and above thr2. Inputs are centi-°C for the
 * temperature and whole °C for the thresholds. */
uint8_t sensor_mode_pwm(int16_t temp_centi_c, int16_t thr1, int16_t thr2);

/* CYCLE mode: alternates between cfg->pwm_pct (during cfg->con_sec) and
 * 0 (during cfg->coff_sec). cycle_elapsed_ms is the caller's running
 * counter modulo (con+off). Returns 0 when both durations are zero. */
uint8_t cycle_mode_pwm(const struct prf_cfg *cfg, uint32_t cycle_elapsed_ms);

#endif /* APP_CONTROL_LOGIC_H_ */
