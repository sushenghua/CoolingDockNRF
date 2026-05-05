#include "control.h"
#include "control_logic.h"
#include "sys_data.h"
#include "sensor.h"
#include "fan.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

#define CONTROL_STACK_SIZE  1024
#define CONTROL_PRIORITY    5
#define CONTROL_PERIOD_MS   1000

static K_THREAD_STACK_DEFINE(control_stack, CONTROL_STACK_SIZE);
static struct k_thread control_tcb;

/* Read by the BLE thread, written by the control thread. Use atomics so
 * the C model can't tear or reorder. */
enum status_v { STAT_UNKNOWN = 0, STAT_RUNNING, STAT_STOPPED };

static atomic_t actual_pwm_a = ATOMIC_INIT(0);
static atomic_t status_a     = ATOMIC_INIT(STAT_UNKNOWN);

uint8_t control_get_actual_pwm(void) { return (uint8_t)atomic_get(&actual_pwm_a); }

const char *control_get_status(void)
{
	switch (atomic_get(&status_a)) {
	case STAT_RUNNING: return "running";
	case STAT_STOPPED: return "stopped";
	default:           return "unknown";
	}
}

/* sensor_mode_pwm() and cycle_mode_pwm() now live in control_logic.{h,c}
 * so they can be unit-tested on the host without dragging in Zephyr. */

static void apply_cfg(const struct prf_cfg *cfg, int16_t temp_centi_c, bool temp_valid,
		      uint32_t cycle_elapsed_ms)
{
	uint8_t target = 0;

	switch (cfg->mode) {
	case PRF_MODE_POWER:
		target = cfg->pwr ? 100 : 0;
		break;
	case PRF_MODE_FIXED:
		target = cfg->pwm_pct;
		break;
	case PRF_MODE_SENSOR:
		if (!temp_valid) {
			target = 0;
			break;
		}
		target = sensor_mode_pwm(temp_centi_c, cfg->thr1_c, cfg->thr2_c);
		break;
	case PRF_MODE_CYCLE:
		target = cycle_mode_pwm(cfg, cycle_elapsed_ms);
		break;
	case PRF_MODE_SCHEDULE:
	default:
		target = 0;   /* persisted but no RTC; leave fan off */
		break;
	}

	bool power_on = target > 0;
	fan_power_set(power_on);
	fan_set_percent(target);

	atomic_set(&actual_pwm_a, target);
	atomic_set(&status_a, power_on ? STAT_RUNNING : STAT_STOPPED);
}

static void control_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	uint32_t cycle_elapsed_ms = 0;
	uint8_t  prev_mode = 0;
	bool     have_prev_mode = false;

	LOG_INF("control thread up (period %d ms)", CONTROL_PERIOD_MS);

	while (1) {
		struct prf_cfg cfg;
		struct sensor_reading r;

		(void)sys_data_get_prf(0, &cfg);
		bool temp_valid = (sensor_get(&r) == 0);
		bool master = sys_data_get_master();

		if (!have_prev_mode || cfg.mode != prev_mode) {
			cycle_elapsed_ms = 0;
			prev_mode = cfg.mode;
			have_prev_mode = true;
		}

		if (!master) {
			fan_power_set(false);
			fan_set_percent(0);
			atomic_set(&actual_pwm_a, 0);
			atomic_set(&status_a, STAT_STOPPED);
		} else {
			apply_cfg(&cfg, r.temp_centi_c, temp_valid, cycle_elapsed_ms);
		}

		k_msleep(CONTROL_PERIOD_MS);
		cycle_elapsed_ms += CONTROL_PERIOD_MS;
	}
}

void control_start(void)
{
	k_thread_create(&control_tcb, control_stack,
			K_THREAD_STACK_SIZEOF(control_stack),
			control_thread, NULL, NULL, NULL,
			CONTROL_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&control_tcb, "control");
}
