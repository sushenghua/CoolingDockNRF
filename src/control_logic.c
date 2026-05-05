#include "control_logic.h"

uint8_t sensor_mode_pwm(int16_t temp_centi_c, int16_t thr1, int16_t thr2)
{
	int32_t t1 = (int32_t)thr1 * 100;
	int32_t t2 = (int32_t)thr2 * 100;
	int32_t db = t1 + SENSOR_DEAD_BAND_C * 100;

	if (temp_centi_c <= t1) return 0;
	if (temp_centi_c >= t2) return FAN_HIGH_PCT;
	if (temp_centi_c <  db) return FAN_LOW_PCT;
	if (t2 <= db)           return FAN_HIGH_PCT;

	int32_t span = t2 - db;
	int32_t over = temp_centi_c - db;
	int32_t pct  = FAN_LOW_PCT + (FAN_HIGH_PCT - FAN_LOW_PCT) * over / span;
	if (pct < 0)   pct = 0;
	if (pct > 100) pct = 100;
	return (uint8_t)pct;
}

uint8_t cycle_mode_pwm(const struct prf_cfg *cfg, uint32_t cycle_elapsed_ms)
{
	uint32_t on_ms  = (uint32_t)cfg->con_sec  * 1000;
	uint32_t off_ms = (uint32_t)cfg->coff_sec * 1000;
	if (on_ms + off_ms == 0) return 0;

	uint32_t phase = cycle_elapsed_ms % (on_ms + off_ms);
	return (phase < on_ms) ? cfg->pwm_pct : 0;
}
