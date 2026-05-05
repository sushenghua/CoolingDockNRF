/* Unit tests for src/control_logic.c — sensor hysteresis bands and
 * cycle phase logic, including regression for pwm_pct=0 in cycle mode. */

#include "../../test_harness.h"

#include "control_logic.h"

/* ============================================================
 *                    sensor_mode_pwm
 * ============================================================ */

TEST(sensor_below_thr1_returns_zero)
{
	ASSERT_EQ(sensor_mode_pwm(2400, 25, 32), 0);
	ASSERT_EQ(sensor_mode_pwm(2500, 25, 32), 0);
	ASSERT_EQ(sensor_mode_pwm(-1000, 25, 32), 0);
}

TEST(sensor_dead_band_returns_low)
{
	/* thr1=25, dead band runs 25.01 .. 26.99 (thr1 + 2 °C exclusive) */
	ASSERT_EQ(sensor_mode_pwm(2501, 25, 32), FAN_LOW_PCT);
	ASSERT_EQ(sensor_mode_pwm(2600, 25, 32), FAN_LOW_PCT);
	ASSERT_EQ(sensor_mode_pwm(2699, 25, 32), FAN_LOW_PCT);
}

TEST(sensor_ramp_interpolates)
{
	/* Ramp FAN_LOW at 27.00 °C → FAN_HIGH at 32.00 °C. Halfway is
	 * 29.50 °C → about 65 %. */
	uint8_t pct = sensor_mode_pwm(2950, 25, 32);
	ASSERT_WITHIN(pct, 65, 1, "got %u", pct);
}

TEST(sensor_at_or_above_thr2_returns_high)
{
	ASSERT_EQ(sensor_mode_pwm(3200, 25, 32), FAN_HIGH_PCT);
	ASSERT_EQ(sensor_mode_pwm(5000, 25, 32), FAN_HIGH_PCT);
}

TEST(sensor_ramp_endpoints_are_consistent)
{
	uint8_t low = sensor_mode_pwm(2700, 25, 32);
	ASSERT_EQ(low, FAN_LOW_PCT);

	uint8_t high = sensor_mode_pwm(3199, 25, 32);
	ASSERT_WITHIN(high, FAN_HIGH_PCT, 1);
}

/* Degenerate: thr2 sits inside the dead band → ramp would have zero
 * span. Logic clamps to HIGH. */
TEST(sensor_narrow_thresholds_collapse_to_high)
{
	ASSERT_EQ(sensor_mode_pwm(2700, 25, 26), FAN_HIGH_PCT);
	ASSERT_EQ(sensor_mode_pwm(2700, 25, 27), FAN_HIGH_PCT);
}

/* ============================================================
 *                    cycle_mode_pwm
 * ============================================================ */

static struct prf_cfg cycle_cfg(uint16_t con, uint16_t coff, uint8_t pct)
{
	return (struct prf_cfg){
		.mode = PRF_MODE_CYCLE,
		.con_sec = con, .coff_sec = coff,
		.pwm_pct = pct,
	};
}

TEST(cycle_on_phase_returns_pwm_pct)
{
	struct prf_cfg c = cycle_cfg(10, 5, 60);
	ASSERT_EQ(cycle_mode_pwm(&c,    0), 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 5000), 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 9999), 60);
}

TEST(cycle_off_phase_returns_zero)
{
	struct prf_cfg c = cycle_cfg(10, 5, 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 10000), 0);
	ASSERT_EQ(cycle_mode_pwm(&c, 14999), 0);
}

TEST(cycle_phase_wraps_modulo_total)
{
	struct prf_cfg c = cycle_cfg(10, 5, 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 15000), 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 24999), 60);
	ASSERT_EQ(cycle_mode_pwm(&c, 25000), 0);
}

TEST(cycle_zero_durations_returns_zero)
{
	struct prf_cfg c = cycle_cfg(0, 0, 100);
	ASSERT_EQ(cycle_mode_pwm(&c, 0), 0);
	ASSERT_EQ(cycle_mode_pwm(&c, 999999), 0);
}

/* Regression for fix #19: pwm_pct=0 used to fall through to default
 * 100. Fix honors literal 0. */
TEST(cycle_pwm_zero_honored_literally)
{
	struct prf_cfg c = cycle_cfg(10, 5, 0);
	ASSERT_EQ(cycle_mode_pwm(&c, 1000), 0);
	ASSERT_EQ(cycle_mode_pwm(&c, 9999), 0);
}
