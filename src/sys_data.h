#ifndef APP_SYS_DATA_H_
#define APP_SYS_DATA_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SYS_DEVICE_NAME_MAX  24
#define SYS_PRF_COUNT        1

/* Modes accepted from frontend "SetPeripheralConfig". String <-> enum
 * conversion lives in cmd_interpreter. */
enum prf_mode {
	PRF_MODE_POWER    = 0,
	PRF_MODE_FIXED    = 1,
	PRF_MODE_SENSOR   = 2,
	PRF_MODE_CYCLE    = 3,
	PRF_MODE_SCHEDULE = 4,   /* persisted but not executed (no RTC) */
};

/* One peripheral's persisted configuration. Only the fields the frontend
 * actually sends/reads are stored. */
struct prf_cfg {
	uint8_t  mode;        /* enum prf_mode */
	bool     pwr;         /* power-mode on/off */
	int16_t  thr1_c;      /* sensor-mode low threshold, °C  */
	int16_t  thr2_c;      /* sensor-mode high threshold, °C */
	uint8_t  pwm_pct;     /* fixed/cycle duty cycle, 0..100 */
	uint16_t con_sec;     /* cycle-mode on duration */
	uint16_t coff_sec;    /* cycle-mode off duration */
	uint32_t ont_sec;     /* schedule on time of day (sec) */
	uint32_t offt_sec;    /* schedule off time of day (sec) */
};

/* Initialise defaults + load any persisted overrides via the settings
 * subsystem. settings_subsys_init() must already have been called. */
int  sys_data_init(void);

/* Master power switch. */
bool sys_data_get_master(void);
int  sys_data_set_master(bool on);

/* Friendly device name. Copies into the caller's buffer under lock;
 * returns 0 on success or -EINVAL. `cap` must be ≥ 1. */
int sys_data_get_name(char *out, size_t cap);
int sys_data_set_name(const char *name);

/* Peripheral config. idx must be < SYS_PRF_COUNT (== 1 for now). */
int sys_data_get_prf(uint8_t idx, struct prf_cfg *out);
int sys_data_set_prf(uint8_t idx, const struct prf_cfg *cfg);

/* Reset all persisted state to defaults (does not reboot). */
int sys_data_factory_reset(void);

#endif /* APP_SYS_DATA_H_ */
