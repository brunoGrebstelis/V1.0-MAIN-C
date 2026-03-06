#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    CMD_SET_PRICE = 0x01,   // payload: 2 bytes (MSB, LSB)
    CMD_SET_RGB   = 0x02,   // payload: 3 bytes (r,g,b)
    CMD_GET_TEMP  = 0x03    // payload: 0 bytes, reply: 2 bytes temp*100 int16
} protocol_cmd_t;

void protocol_on_frame(uint8_t cmd, const uint8_t *payload, uint8_t len);

// called by protocol when it wants to send a reply back
// implemented in i2c_slave.c
void i2c_slave_send_reply(const uint8_t *data, uint8_t len);

#endif
