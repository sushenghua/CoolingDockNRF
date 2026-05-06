#include "sensor.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor, LOG_LEVEL_INF);

/* ============================================================
 *           SHT3x soft-reset before driver init
 * ============================================================
 *
 * Zephyr's `sht3xd` driver doesn't soft-reset the chip at init —
 * it just sends START_PERIODIC_MEASUREMENT (or, in single-shot
 * mode, nothing at all) and considers itself done. If the chip
 * was left in some other state from the previous boot (different
 * mode, half-issued command, alert-pending, etc.), the new init
 * silently runs against stale chip state and reads can fail.
 *
 * We sidestep this by issuing the SHT3x's `0x30A2` soft-reset
 * command at SYS_INIT time, just before the sensor driver runs.
 * Per the datasheet, the reset takes < 1 ms; we wait 2 ms for
 * margin. From there the Zephyr driver init always sees a
 * freshly-reset chip and can pick whatever mode it wants
 * cleanly.
 *
 * Init priority math:
 *   - I2C driver runs at CONFIG_I2C_INIT_PRIORITY        (50)
 *   - We run at  POST_KERNEL                              (80)  <- here
 *   - sht3xd driver runs at CONFIG_SENSOR_INIT_PRIORITY  (90)
 *
 * If the SHT3x is unreachable (not wired, wrong address), the
 * I2C write fails and we log a warning but don't fail init —
 * the firmware should still boot so the user can flash a fix.
 */

#define SHT3X_CMD_SOFT_RESET_HI  0x30
#define SHT3X_CMD_SOFT_RESET_LO  0xA2

static int sht3x_soft_reset_init(void)
{
	const struct device *i2c =
		DEVICE_DT_GET(DT_BUS(DT_ALIAS(sht3x)));
	const uint16_t addr = DT_REG_ADDR(DT_ALIAS(sht3x));

	if (!device_is_ready(i2c)) {
		LOG_WRN("I2C bus not ready, skipping SHT3x soft-reset");
		return 0;
	}

	const uint8_t cmd[2] = { SHT3X_CMD_SOFT_RESET_HI, SHT3X_CMD_SOFT_RESET_LO };
	int rc = i2c_write(i2c, cmd, sizeof(cmd), addr);
	if (rc != 0) {
		/* Common case: sensor not wired. The sensor thread will
		 * detect this later via device_is_ready() and log clearly. */
		LOG_DBG("SHT3x soft-reset write at 0x%02x: %d (chip absent?)",
			addr, rc);
		return 0;
	}

	/* Datasheet: soft-reset completes in < 1 ms. */
	k_msleep(2);
	LOG_INF("SHT3x soft-reset issued at 0x%02x", addr);
	return 0;
}

SYS_INIT(sht3x_soft_reset_init, POST_KERNEL, 80);

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
		LOG_ERR("SHT3x not ready - check I2C wiring (P0.26/P0.27) and addr (overlay reg)");
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
