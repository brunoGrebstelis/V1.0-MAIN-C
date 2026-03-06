#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>

void i2c_slave_init(void);

// Called by protocol.c to transmit reply bytes
void i2c_slave_send_reply(const uint8_t *data, uint8_t len);

#endif
