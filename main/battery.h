#ifndef BATTERY_H
#define BATTERY_H

/**
 * Initialize ADC for battery voltage reading (GPIO2, ADC1_CH3).
 */
void battery_init(void);

/**
 * Get battery voltage in millivolts.
 * Returns 0 on read error.
 */
int battery_get_voltage(void);

/**
 * Get battery level as percentage (0-100).
 * Range: 3.0V = 0%, 4.12V = 100%.
 */
int battery_get_level(void);

#endif // BATTERY_H
