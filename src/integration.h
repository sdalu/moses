#ifndef _INTEGRATION_H
#define _INTEGRATION_H

#define LCD_H_RES (80)
#define LCD_V_RES (160)

#include <math.h>

int integration_init(void);
int get_tph(float *temperature, float *pressure, float *humidity);

// Home altitude : 175m
static inline float
sea_level_pressure(float pressure, float temperature, float altitude) {
    return pressure * pow(1 - (0.0065 * altitude) /
			      (temperature + 0.0065 * altitude + 273.15),
			  -5.257);
}

#endif
