#include "i2c_slave.h"

#include "main.h"
#include "display.h"
#include "rgb.h"
#include "temp.h"
#include "app.h"

// ===== Fixed 4-byte frame: [CMD, B1, B2, B3] =====
#define CMD_SET_PRICE  0x01
#define CMD_SET_RGB    0x02
#define CMD_GET_TEMP   0x03

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

// ========= This is the missing function you asked for =========
// Non-blocking. If a TX is already running, it will be ignored (or you can overwrite policy later).
void i2c_slave_send_reply(const uint8_t *data, uint8_t len)
{
    if (!data || len == 0) return;

    if (len > sizeof(tx_buf)) len = sizeof(tx_buf);

    // If currently transmitting, don't start another (simple policy)
    if (tx_busy) return;

    for (uint8_t i = 0; i < len; i++) tx_buf[i] = data[i];
    tx_len  = len;
    tx_busy = 1;

    // Master must issue a READ to clock these bytes out.
    HAL_I2C_Slave_Transmit_IT(&hi2c1, tx_buf, tx_len);
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

        } break;

        case CMD_SET_RGB:
        {
            rgb_set(rx4[1], rx4[2], rx4[3]);

        } break;

        case CMD_GET_TEMP:
        {
            // Reply with temperature * 100 as int16 (2 bytes)
            float t = temp_read_c();
            int16_t t100 = (int16_t)(t * 100.0f);
            uint8_t out[2] = { (uint8_t)(t100 >> 8), (uint8_t)(t100 & 0xFF) };
            i2c_slave_send_reply(out, 2);
        } break;

        default:
        {
            uint8_t err = 0xEE;
            i2c_slave_send_reply(&err, 1);
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
