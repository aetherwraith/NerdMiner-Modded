#ifndef _SERIAL_CLI_H
#define _SERIAL_CLI_H

#include <Arduino.h>

/**
 * @brief Initialize USB Serial CLI (spawns a lightweight boot task during setup()
 * so captive-portal first-boot provisioning works immediately, then hands off to
 * loop() via serialCliEnterLoopMode() to free the task stack during mining).
 */
void setup_serialCli(void);

/**
 * @brief Hand Serial CLI polling over to loop() and delete the boot task.
 */
void serialCliEnterLoopMode(void);

/**
 * @brief Process incoming UART characters non-blockingly.
 */
void serialCliProcess(void);

#endif // _SERIAL_CLI_H
