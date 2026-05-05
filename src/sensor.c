#include "sensor.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

#define SENSOR_STACK_SIZE  1024
#define SENSOR_PRIORITY    5
#define SAMPLE_PERIOD_MS   500
#define SAMPLE_STALE_MS    5000   /* report -EAGAIN if last good sample older than this */

static const struct device *const sht = DEVICE_DT_GET(DT_ALIAS(sht3x));

static K_MUTEX_DEFINE(latest_mu);
static struct sensor_reading latest;
static int64_t latest_uptime_ms;

static K_THREAD_STACK_DEFINE(sensor_stack, SENSOR_STACK_SIZE);
static struct k_thread sensor_tcb;

int sensor_get(struct sensor_reading *out)
{
	int rc = -EAGAIN;
	k_mutex_lock(&latest_mu, K_FOREVER);
	if (latest.valid && (k_uptime_get() - latest_uptime_ms) < SAMPLE_STALE_MS) {
		*out = latest;
		rc = 0;
	}
	k_mutex_unlock(&latest_mu);
	return rc;
}

static void sensor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	if (!device_is_ready(sht)) {
		LOG_ERR("SHT3x not ready - check I2C wiring (P0.26/P0.27, addr 0x44)");
		return;
	}
	LOG_INF("sensor thread up on %s", sht->name);

	while (1) {
		struct sensor_value t, h;
		int rc = sensor_sample_fetch(sht);

		if (rc == 0) rc = sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t);
		if (rc == 0) rc = sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY,    &h);

		if (rc == 0) {
			int32_t tc = t.val1 * 100 + t.val2 / 10000;
			int32_t hc = h.val1 * 100 + h.val2 / 10000;
			if (hc < 0)     hc = 0;
			if (hc > 10000) hc = 10000;

			k_mutex_lock(&latest_mu, K_FOREVER);
			latest.temp_centi_c    = (int16_t)tc;
			latest.humid_centi_pct = (uint16_t)hc;
			latest.valid           = true;
			latest_uptime_ms       = k_uptime_get();
			k_mutex_unlock(&latest_mu);

			LOG_DBG("T=%d.%02d C  H=%u.%02u %%",
				tc / 100, (tc < 0 ? -tc : tc) % 100,
				(unsigned)hc / 100, (unsigned)hc % 100);
		} else {
			LOG_WRN("sht3x read failed: %d", rc);
		}

		k_msleep(SAMPLE_PERIOD_MS);
	}
}

void sensor_start(void)
{
	k_thread_create(&sensor_tcb, sensor_stack,
			K_THREAD_STACK_SIZEOF(sensor_stack),
			sensor_thread, NULL, NULL, NULL,
			SENSOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sensor_tcb, "sensor");
}
