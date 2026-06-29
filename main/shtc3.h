#ifndef SHTC3_H
#define SHTC3_H

#include <stdbool.h>

/**
 * Initialize SHTC3 driver on the XiaoZhi I2C bus (GPIO13/14, addr 0x70).
 * Must be called after xiaozhi_init() (which builds the bus).
 * Returns true on success.
 */
bool shtc3_init(void);

/** Last temperature read (°C). Returns 0.0 if never read successfully. */
float shtc3_last_temperature(void);

/** Last humidity read (%RH). Returns 0.0 if never read successfully. */
float shtc3_last_humidity(void);

/**
 * Perform a synchronous measurement (~15ms).
 * Updates the cached values returned by the getters above.
 * Returns true if measurement was successful.
 */
bool shtc3_measure(void);

#endif // SHTC3_H
