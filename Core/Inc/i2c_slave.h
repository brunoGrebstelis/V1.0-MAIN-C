#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>

void i2c_slave_init(void);

// Transmit reply bytes (driver pads/truncates to fixed 4-byte reply frame)
void i2c_slave_send_reply(const uint8_t *data, uint8_t len);

// Periodic debug print helper (optional runtime diagnostics)
void i2c_slave_debug_task(void);

// Periodic reliability monitor:
// - re-arms RX callback if it got lost
// - force-recovers I2C peripheral after prolonged inactivity in busy/error state
// - applies write-only commands outside interrupt context
void i2c_slave_watchdog_task(void);

// Retrieve and clear accumulated I2C error flags (bitmask of HAL_I2C_ERROR_* values)
uint32_t i2c_slave_take_error_flags(void);

// True when no I2C transaction/recovery has happened for at least min_quiet_ms.
uint8_t i2c_slave_is_quiet(uint32_t min_quiet_ms);

#endif
