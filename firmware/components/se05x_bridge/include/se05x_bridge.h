/* Lab bench — SE05x bridge (I2C target <-> SE050Sim over TCP). See se05x_bridge.c. */
#ifndef SE05X_BRIDGE_H
#define SE05X_BRIDGE_H

/* Joins Wi-Fi, starts the I2C target and connects to the simulator. NVS must be initialised. */
void se05x_bridge_start(void);

#endif
