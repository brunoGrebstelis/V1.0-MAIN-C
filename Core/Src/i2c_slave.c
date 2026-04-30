#include "i2c_slave.h"

#include "main.h"
#include "display.h"
#include "rgb.h"
#include "temp.h"
#include "app.h"
#include "lighting_mode.h"

// ===== Fixed 4-byte request frame from master: [CMD, B1, B2, B3] =====
#define CMD_SET_PRICE  0x01U
#define CMD_SET_RGB    0x02U
#define CMD_GET_TEMP   0x03U
#define CMD_SET_LIGHT_MODE 0x04U

#define CMD_GET_ALL    0x05U

// ===== Fixed 4-byte reply frame to master: [CMD, LOCKER_ID, D1, D2] =====
#define REPLY_TEMP      0x01U
#define REPLY_ALL_DATA  0x02U

static uint8_t rx4[4];

extern I2C_HandleTypeDef hi2c1;

// TX buffer for replies
static uint8_t  tx_buf[8];
static volatile uint8_t tx_len = 0;
static volatile uint8_t tx_busy = 0;

static void arm_rx(I2C_HandleTypeDef *hi2c)
{
    // Always receive exactly 4 bytes
    HAL_I2C_Slave_Receive_IT(hi2c, rx4, sizeof(rx4));
}

void i2c_slave_init(void)
{
    tx_busy = 0;
    tx_len  = 0;
    arm_rx(&hi2c1);
}

// Non-blocking. If a TX is already running, new reply is ignored.
// Reply is always fixed 4 bytes: [CMD, LOCKER_ID, D1, D2]
void i2c_slave_send_reply(const uint8_t *data, uint8_t len)
{
    if (!data || len == 0) return;

    // If currently transmitting, don't start another (simple policy)
    if (tx_busy) return;

    for (uint8_t i = 0; i < 4; i++) {
        tx_buf[i] = (i < len) ? data[i] : 0x00;
    }

    tx_len  = 4;
    tx_busy = 1;

    // Master must issue a READ to clock these bytes out.
    HAL_I2C_Slave_Transmit_IT(&hi2c1, tx_buf, tx_len);
}

static void send_reply_frame(uint8_t reply_cmd, uint16_t data16)
{
    uint8_t out[4] = {
        reply_cmd,
        app_get_locker_id(),
        (uint8_t)(data16 >> 8),
        (uint8_t)(data16 & 0xFF)
    };
    i2c_slave_send_reply(out, sizeof(out));
}

// ================= HAL callbacks =================

// Called when 4 bytes have been received into rx4[]
void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;

    uint8_t cmd = rx4[0];

    switch (cmd)
    {
        case CMD_SET_PRICE:
        {
            uint16_t price = (uint16_t)((rx4[1] << 8) | rx4[2]);
            app_set_price(price);
            display_set_number(price);

            // "all data" currently uses current price as 16-bit payload
            send_reply_frame(REPLY_ALL_DATA, app_get_price());

        } break;

        case CMD_SET_RGB:
        {
            rgb_set(rx4[1], rx4[2], rx4[3]);
            lighting_mode_set_base_color(rx4[1], rx4[2], rx4[3]);

            // acknowledge with current "all data" payload
            send_reply_frame(REPLY_ALL_DATA, app_get_price());

        } break;

        case CMD_SET_LIGHT_MODE:
        {
            // Lighting mode command payload in B1
            // 0   -> keep current color (no change)
            // 255 -> green for 500 ms, then restore previous/base color
            // else-> run selected light show mode
            run_light_show(rx4[1]);

            // acknowledge with current "all data" payload
            send_reply_frame(REPLY_ALL_DATA, app_get_price());

        } break;

        case CMD_GET_TEMP:
        {
            // Reply with temperature * 100 in D1:D2 (int16, big-endian)
            float t = temp_read_c();
            int16_t t100 = (int16_t)(t * 100.0f);
            send_reply_frame(REPLY_TEMP, (uint16_t)t100);
        } break;

        case CMD_GET_ALL:
        {
            send_reply_frame(REPLY_ALL_DATA, app_get_price());
        } break;

        default:
        {
            // fixed 4-byte reply, marker 0xFFFF means unsupported request
            send_reply_frame(REPLY_ALL_DATA, 0xFFFFU);
        } break;
    }

    // Keep listening for next frame
    arm_rx(hi2c);
}

// Called when slave finished transmitting reply bytes
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;
    tx_busy = 0;
}

// Called on NACK / bus error etc.
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;

    tx_busy = 0;
    tx_len  = 0;

    arm_rx(hi2c);
}
