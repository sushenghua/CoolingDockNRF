#include "fan.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(fan, LOG_LEVEL_INF);

static const struct pwm_dt_spec  fan_pwm   = PWM_DT_SPEC_GET(DT_ALIAS(fan_pwm));
static const struct gpio_dt_spec fan_power = GPIO_DT_SPEC_GET(DT_ALIAS(fan_power), gpios);

static uint8_t cur_pct;

int fan_init(void)
{
	if (!pwm_is_ready_dt(&fan_pwm)) {
		LOG_ERR("fan PWM device not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&fan_power)) {
		LOG_ERR("fan-power GPIO not ready");
		return -ENODEV;
	}

	int rc = gpio_pin_configure_dt(&fan_power, GPIO_OUTPUT_INACTIVE);
	if (rc) {
		LOG_ERR("gpio cfg: %d", rc);
		return rc;
	}

	rc = fan_set_percent(0);
	LOG_INF("fan ready (PWM period %u ns)", (unsigned)fan_pwm.period);
	return rc;
}

int fan_set_percent(uint8_t pct)
{
	if (pct > 100) pct = 100;
	uint64_t pulse = ((uint64_t)fan_pwm.period * pct) / 100U;
	int rc = pwm_set_pulse_dt(&fan_pwm, (uint32_t)pulse);
	if (rc == 0) {
		cur_pct = pct;
	} else {
		LOG_WRN("pwm set %u%% -> %d", pct, rc);
	}
	return rc;
}

int fan_power_set(bool on)
{
	return gpio_pin_set_dt(&fan_power, on ? 1 : 0);
}

uint8_t fan_get_percent(void)
{
	return cur_pct;
}
