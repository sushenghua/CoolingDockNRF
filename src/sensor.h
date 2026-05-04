#ifndef APP_SENSOR_H_
#define APP_SENSOR_H_

#include <stdint.h>
#include <stdbool.h>

struct sensor_reading {
	int16_t  temp_centi_c;     /* hundredths of a degree C, e.g. 2543 = 25.43 */
	uint16_t humid_centi_pct;  /* hundredths of a percent,   e.g. 6512 = 65.12 */
	bool     valid;
};

/* Returns 0 on success, -EAGAIN if no sample has been taken yet. */
int sensor_get(struct sensor_reading *out);

/* Spawn the sampling thread. Call after sys_data_init(). */
void sensor_start(void);

#endif /* APP_SENSOR_H_ */
