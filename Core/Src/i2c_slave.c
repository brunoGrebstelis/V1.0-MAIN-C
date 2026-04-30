#include "i2c_slave.h"

#include "main.h"
#include "temp.h"
#include "app.h"
#include "terminal.h"

#include <string.h>

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

// Time tracking for reliability watchdog
static volatile uint32_t i2c_last_activity_ms = 0;
static volatile uint32_t i2c_last_recover_ms = 0;

// Latching error info
static volatile uint32_t i2c_error_flags = 0;
static volatile uint32_t i2c_recover_count = 0;

// Debug counters to verify if I2C callbacks/IRQ path are alive at runtime
static volatile uint32_t dbg_rx_cplt = 0;
static volatile uint32_t dbg_tx_cplt = 0;
static volatile uint32_t dbg_err_cplt = 0;
static volatile uint32_t dbg_arm_rx_ok = 0;
static volatile uint32_t dbg_arm_rx_busy = 0;
static volatile uint32_t dbg_tx_start_ok = 0;
static volatile uint32_t dbg_tx_start_busy = 0;
static volatile uint8_t  dbg_last_cmd = 0;
static volatile uint32_t dbg_recover = 0;

#define I2C_RECOVER_COOLDOWN_MS      20U
#define I2C_STUCK_TIMEOUT_MS         40U
#define I2C_RECOVER_MAX_TRIES        2U

static void arm_rx(I2C_HandleTypeDef *hi2c);

static void i2c_note_activity(void)
{
    i2c_last_activity_ms = HAL_GetTick();
}

static void i2c_force_ready(I2C_HandleTypeDef *hi2c)
{
    if (hi2c == NULL) {
        return;
    }

    // Abort possible pending transfer state in HAL
    (void)HAL_I2C_Master_Abort_IT(hi2c, 0);

    // Reset peripheral state machine and clear flags
    (void)HAL_I2C_DeInit(hi2c);
    (void)HAL_I2C_Init(hi2c);
    (void)HAL_I2CEx_ConfigAnalogFilter(hi2c, I2C_ANALOGFILTER_ENABLE);
    (void)HAL_I2CEx_ConfigDigitalFilter(hi2c, 0);

    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_BERR);
    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_ARLO);
    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_OVR);
    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_AF);
    __HAL_I2C_CLEAR_FLAG(hi2c, I2C_FLAG_STOPF);

    tx_busy = 0;
    tx_pending = 0;
    tx_len = 0;
    rx_armed = 0;

    dbg_recover++;
    i2c_recover_count++;
    i2c_note_activity();

    arm_rx(hi2c);
}

static void i2c_try_recover_if_stuck(I2C_HandleTypeDef *hi2c)
{
    const uint32_t now = HAL_GetTick();
    const uint32_t dt_since_activity = now - i2c_last_activity_ms;
    const uint32_t dt_since_recover = now - i2c_last_recover_ms;

    if (dt_since_recover < I2C_RECOVER_COOLDOWN_MS) {
        return;
    }

    if (dt_since_activity < I2C_STUCK_TIMEOUT_MS) {
        return;
    }

    uint32_t isr = hi2c->Instance->ISR;
    uint8_t stuck = 0;

    if ((hi2c->State != HAL_I2C_STATE_READY) && !rx_armed) {
        stuck = 1;
    }

    if (isr & (I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR)) {
        stuck = 1;
    }

    if (!stuck) {
        return;
    }

    i2c_last_recover_ms = now;
    for (uint8_t i = 0; i < I2C_RECOVER_MAX_TRIES; i++) {
        i2c_force_ready(hi2c);
        if ((hi2c->State == HAL_I2C_STATE_READY) || rx_armed) {
            break;
        }
    }
}

static void arm_rx(I2C_HandleTypeDef *hi2c)
{
    // Always receive exactly 4 bytes
    if (HAL_I2C_Slave_Receive_IT(hi2c, rx4, sizeof(rx4)) == HAL_OK) {
        rx_armed = 1;
        dbg_arm_rx_ok++;
        i2c_note_activity();
    } else {
        dbg_arm_rx_busy++;

        // If RX re-arm fails repeatedly, force recovery to keep callbacks alive
        i2c_try_recover_if_stuck(hi2c);
    }
}

void i2c_slave_init(void)
{
    memset((void *)rx4, 0, sizeof(rx4));
    memset((void *)tx_buf, 0, sizeof(tx_buf));

    rx_armed = 0;
    tx_pending = 0;
    tx_busy = 0;
    tx_len  = 0;

    i2c_last_activity_ms = HAL_GetTick();
    i2c_last_recover_ms = 0;
    i2c_error_flags = 0;
    i2c_recover_count = 0;

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
        i2c_note_activity();
    } else {
        tx_busy = 0;
        tx_pending = 0;
        dbg_tx_start_busy++;
        i2c_try_recover_if_stuck(&hi2c1);
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

    terminal_printf("I2C state=0x%02X err=0x%08lX recov=%lu lastAct=%lu ms\r\n",
                    (unsigned int)hi2c1.State,
                    (unsigned long)i2c_error_flags,
                    (unsigned long)dbg_recover,
                    (unsigned long)(now - i2c_last_activity_ms));
}

void i2c_slave_watchdog_task(void)
{
    // keep RX callback armed whenever possible
    if (!rx_armed && !tx_busy && hi2c1.State == HAL_I2C_STATE_READY) {
        arm_rx(&hi2c1);
    }

    i2c_try_recover_if_stuck(&hi2c1);
}

uint32_t i2c_slave_take_error_flags(void)
{
    uint32_t flags = i2c_error_flags;
    i2c_error_flags = 0;
    return flags;
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
    i2c_note_activity();
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
    i2c_note_activity();
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
    i2c_note_activity();
    i2c_error_flags |= HAL_I2C_GetError(hi2c);
    tx_busy = 0;
    tx_pending = 0;
    tx_len  = 0;
    rx_armed = 0;

    // Fast path: try re-arm first
    arm_rx(hi2c);

    // If still not armed or driver not ready, force full recovery
    if (!rx_armed || hi2c->State != HAL_I2C_STATE_READY) {
        i2c_force_ready(hi2c);
    }
}
