#include "protocol.h"
#include "display.h"
#include "rgb.h"
#include "temp.h"
#include "terminal.h"
#include "app.h"

static void send_u16(uint16_t v)
{
    uint8_t out[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    i2c_slave_send_reply(out, 2);
}

void protocol_on_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd)
    {
        case CMD_SET_PRICE:
            if (len == 2) {
                uint16_t price = (uint16_t)((payload[0] << 8) | payload[1]);
                app_set_price(price);
                display_set_number(price);
                send_u16(price); // ACK by echo
            }
            break;

        case CMD_SET_RGB:
            if (len == 3) {
                rgb_set(payload[0], payload[1], payload[2]);
            }
            break;

        case CMD_GET_TEMP:
            if (len == 0) {
                float t = temp_read_c();
                int16_t t100 = (int16_t)(t * 100.0f);
                uint8_t out[2] = { (uint8_t)(t100 >> 8), (uint8_t)(t100 & 0xFF) };
                i2c_slave_send_reply(out, 2);
            }
            break;

        default:
        {
            uint8_t err = 0xEE;
            i2c_slave_send_reply(&err, 1);
        }
        break;
    }
}
