#ifndef _SERIAL_CLI_H
#define _SERIAL_CLI_H

#include <Arduino.h>

/**
 * @brief Initialize and start the interactive USB Serial CLI background task.
 * Runs non-blocking on Core 0 at priority 2, allowing complete configuration
 * over USB UART (115200 baud) without requiring an SD card or captive portal.
 */
void setup_serialCli(void);

/**
 * @brief Process incoming UART characters (can be called periodically from loop
 * or run inside serialCliTask).
 */
void serialCliProcess(void);

#endif // _SERIAL_CLI_H
