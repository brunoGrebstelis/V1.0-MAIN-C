#include "i2c_slave.h"

#include "main.h"
#include "temp.h"
#include "app.h"
#include "terminal.h"

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
static volatile uint8_t rx_armed = 0;
static volatile uint8_t tx_pending = 0;

// Debug counters to verify if I2C callbacks/IRQ path are alive at runtime
static volatile uint32_t dbg_rx_cplt = 0;
static volatile uint32_t dbg_tx_cplt = 0;
static volatile uint32_t dbg_err_cplt = 0;
static volatile uint32_t dbg_arm_rx_ok = 0;
static volatile uint32_t dbg_arm_rx_busy = 0;
static volatile uint32_t dbg_tx_start_ok = 0;
static volatile uint32_t dbg_tx_start_busy = 0;
static volatile uint8_t  dbg_last_cmd = 0;

static void arm_rx(I2C_HandleTypeDef *hi2c)
{
    // Always receive exactly 4 bytes
    if (HAL_I2C_Slave_Receive_IT(hi2c, rx4, sizeof(rx4)) == HAL_OK) {
        rx_armed = 1;
        dbg_arm_rx_ok++;
    } else {
        dbg_arm_rx_busy++;
    }
}

void i2c_slave_init(void)
{
    rx_armed = 0;
    tx_pending = 0;
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

    tx_len = 4;

    // Master must issue a READ to clock these bytes out.
    if (HAL_I2C_Slave_Transmit_IT(&hi2c1, tx_buf, tx_len) == HAL_OK) {
        tx_busy = 1;
        tx_pending = 1;
        rx_armed = 0;
        dbg_tx_start_ok++;
    } else {
        tx_busy = 0;
        tx_pending = 0;
        dbg_tx_start_busy++;
        arm_rx(&hi2c1);
    }
}

void i2c_slave_debug_task(void)
{
    static uint32_t last_ms = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_ms) < 1000U) return;
    last_ms = now;

    terminal_printf("I2C dbg rx=%lu tx=%lu err=%lu arm_ok=%lu arm_busy=%lu tx_ok=%lu tx_busy=%lu last_cmd=0x%02X rx_armed=%u tx_busy=%u\r\n",
                    (unsigned long)dbg_rx_cplt,
                    (unsigned long)dbg_tx_cplt,
                    (unsigned long)dbg_err_cplt,
                    (unsigned long)dbg_arm_rx_ok,
                    (unsigned long)dbg_arm_rx_busy,
                    (unsigned long)dbg_tx_start_ok,
                    (unsigned long)dbg_tx_start_busy,
                    (unsigned int)dbg_last_cmd,
                    (unsigned int)rx_armed,
                    (unsigned int)tx_busy);
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

    dbg_rx_cplt++;
    rx_armed = 0;

    // A new write transaction from master means bus is active again.
    // If a previous TX reply was never read by master, do not stay stuck.
    tx_busy = 0;
    tx_pending = 0;

    uint8_t cmd = rx4[0];
    dbg_last_cmd = cmd;

    switch (cmd)
    {
        case CMD_SET_PRICE:
        {
            uint16_t price = (uint16_t)((rx4[1] << 8) | rx4[2]);
            app_set_price(price);

        } break;

        case CMD_SET_RGB:
        {
            app_set_rgb(rx4[1], rx4[2], rx4[3]);

        } break;

        case CMD_SET_LIGHT_MODE:
        {
            // Lighting mode command payload in B1
            // 0   -> keep current color (no change)
            // 255 -> green for 500 ms, then restore previous/base color
            // else-> run selected light show mode
            app_set_light_mode(rx4[1]);

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

    // Always re-arm RX for the next write command.
    // This avoids lockup if master sends write-only commands and never reads reply bytes.
    arm_rx(hi2c);
}

// Called when slave finished transmitting reply bytes
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;
    dbg_tx_cplt++;
    tx_busy = 0;
    tx_pending = 0;

    if (!rx_armed) {
        arm_rx(hi2c);
    }
}

// Called on NACK / bus error etc.
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;

    dbg_err_cplt++;
    tx_busy = 0;
    tx_pending = 0;
    tx_len  = 0;
    rx_armed = 0;

    arm_rx(hi2c);
}
