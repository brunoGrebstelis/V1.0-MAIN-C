#include "i2c_slave.h"

#include "main.h"
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
static volatile uint32_t tx_started_ms = 0;

static volatile uint8_t pending_price_valid = 0;
static volatile uint16_t pending_price = 0;
static volatile uint8_t pending_rgb_valid = 0;
static volatile uint8_t pending_rgb_r = 0;
static volatile uint8_t pending_rgb_g = 0;
static volatile uint8_t pending_rgb_b = 0;
static volatile uint8_t pending_mode_valid = 0;
static volatile uint8_t pending_mode = 0;

// Time tracking for reliability watchdog
static volatile uint32_t i2c_last_activity_ms = 0;
static volatile uint32_t i2c_last_valid_frame_ms = 0;
static volatile uint32_t i2c_last_recover_ms = 0;
static volatile uint32_t i2c_bus_busy_since_ms = 0;
static volatile uint32_t i2c_line_low_since_ms = 0;
static volatile uint32_t i2c_recover_window_start_ms = 0;
static volatile uint8_t i2c_recover_window_count = 0;
static volatile uint8_t i2c_recovery_requested = 0;
static volatile uint8_t i2c_recovery_reason = 0;

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
static volatile uint8_t  dbg_last_rx_b0 = 0;
static volatile uint8_t  dbg_last_rx_b1 = 0;
static volatile uint8_t  dbg_last_rx_b2 = 0;
static volatile uint8_t  dbg_last_rx_b3 = 0;
static volatile uint8_t  dbg_last_action = 0;
static volatile uint8_t  dbg_last_reply_cmd = 0;
static volatile uint16_t dbg_last_reply_data = 0;
static volatile uint8_t  dbg_last_reply_valid = 0;
static volatile uint32_t dbg_last_hal_error = 0;
static volatile uint32_t dbg_last_isr = 0;
static volatile uint8_t  dbg_last_recover_reason = 0;
static volatile uint32_t dbg_no_valid_reset_armed = 0;

#define DBG_ACT_NONE            0U
#define DBG_ACT_SET_PRICE       1U
#define DBG_ACT_SET_RGB         2U
#define DBG_ACT_SET_LIGHT_MODE  3U
#define DBG_ACT_GET_TEMP        4U
#define DBG_ACT_GET_ALL         5U
#define DBG_ACT_UNKNOWN         6U

#define I2C_RECOVER_COOLDOWN_MS           20U
#define I2C_STUCK_TIMEOUT_MS              50U
#define I2C_LINE_STUCK_LOW_MS             50U
#define I2C_BUS_BUSY_STUCK_MS             50U
#define I2C_TX_STUCK_TIMEOUT_MS           50U
#define I2C_NO_VALID_FRAME_RECOVER_MS 360000U
#define I2C_NO_VALID_FRAME_RESET_MS   720000U
#define I2C_RECOVER_BURST_WINDOW_MS    10000U
#define I2C_RECOVER_BURST_RESET_LIMIT      6U
#define I2C_RECOVER_MAX_TRIES              2U

#define I2C_RECOVER_REASON_HAL_STATE      1U
#define I2C_RECOVER_REASON_HAL_FLAGS      2U
#define I2C_RECOVER_REASON_BUS_BUSY       3U
#define I2C_RECOVER_REASON_LINE_LOW       4U
#define I2C_RECOVER_REASON_TX_STUCK       5U
#define I2C_RECOVER_REASON_ARM_FAILED     6U
#define I2C_RECOVER_REASON_NO_VALID_FRAME 7U

static void arm_rx(I2C_HandleTypeDef *hi2c);

static void i2c_note_activity(void)
{
    i2c_last_activity_ms = HAL_GetTick();
}

static void i2c_note_valid_frame(void)
{
    const uint32_t now = HAL_GetTick();
    i2c_last_activity_ms = now;
    i2c_last_valid_frame_ms = now;
    dbg_no_valid_reset_armed = 0U;
}

static void i2c_request_recovery(uint8_t reason)
{
    i2c_recovery_requested = 1U;
    i2c_recovery_reason = reason;
}

static void i2c_track_recovery_burst(uint32_t now)
{
    if ((now - i2c_recover_window_start_ms) > I2C_RECOVER_BURST_WINDOW_MS) {
        i2c_recover_window_start_ms = now;
        i2c_recover_window_count = 0U;
    }

    if (i2c_recover_window_count < 255U) {
        i2c_recover_window_count++;
    }

    if (i2c_recover_window_count >= I2C_RECOVER_BURST_RESET_LIMIT) {
        NVIC_SystemReset();
    }
}

static void i2c_force_ready(I2C_HandleTypeDef *hi2c)
{
    if (hi2c == NULL) {
        return;
    }

    HAL_NVIC_DisableIRQ(I2C1_IRQn);

    // Reset peripheral state machine and clear flags
    (void)HAL_I2C_DeInit(hi2c);
    (void)HAL_I2C_Init(hi2c);
    HAL_NVIC_DisableIRQ(I2C1_IRQn);
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
    tx_started_ms = 0;
    i2c_bus_busy_since_ms = 0;
    i2c_line_low_since_ms = 0;

    dbg_recover++;
    i2c_recover_count++;
    i2c_note_activity();

    HAL_NVIC_ClearPendingIRQ(I2C1_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_IRQn);

    arm_rx(hi2c);
}

static void i2c_perform_recovery_if_requested(I2C_HandleTypeDef *hi2c)
{
    const uint32_t now = HAL_GetTick();
    const uint32_t dt_since_recover = now - i2c_last_recover_ms;

    if (!i2c_recovery_requested) {
        return;
    }

    if (dt_since_recover < I2C_RECOVER_COOLDOWN_MS) {
        return;
    }

    i2c_recovery_requested = 0U;
    i2c_last_recover_ms = now;
    dbg_last_recover_reason = i2c_recovery_reason;
    i2c_track_recovery_burst(now);

    for (uint8_t i = 0; i < I2C_RECOVER_MAX_TRIES; i++) {
        i2c_force_ready(hi2c);
        if (rx_armed) {
            break;
        }
    }
}

static void i2c_monitor_for_stuck_bus(I2C_HandleTypeDef *hi2c)
{
    const uint32_t now = HAL_GetTick();
    const uint32_t isr = hi2c->Instance->ISR;
    const GPIO_PinState scl = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8);
    const GPIO_PinState sda = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7);

    if (isr & (I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR)) {
        i2c_request_recovery(I2C_RECOVER_REASON_HAL_FLAGS);
        return;
    }

    if ((hi2c->State != HAL_I2C_STATE_READY) && !rx_armed && !tx_busy) {
        if ((now - i2c_last_activity_ms) >= I2C_STUCK_TIMEOUT_MS) {
            i2c_request_recovery(I2C_RECOVER_REASON_HAL_STATE);
        }
        return;
    }

    if (tx_busy && ((now - tx_started_ms) >= I2C_TX_STUCK_TIMEOUT_MS)) {
        i2c_request_recovery(I2C_RECOVER_REASON_TX_STUCK);
        return;
    }

    if ((isr & I2C_FLAG_BUSY) != 0U) {
        if (i2c_bus_busy_since_ms == 0U) {
            i2c_bus_busy_since_ms = now;
        } else if ((now - i2c_bus_busy_since_ms) >= I2C_BUS_BUSY_STUCK_MS) {
            i2c_request_recovery(I2C_RECOVER_REASON_BUS_BUSY);
        }
    } else {
        i2c_bus_busy_since_ms = 0U;
    }

    if ((scl == GPIO_PIN_RESET) || (sda == GPIO_PIN_RESET)) {
        if (i2c_line_low_since_ms == 0U) {
            i2c_line_low_since_ms = now;
        } else if ((now - i2c_line_low_since_ms) >= I2C_LINE_STUCK_LOW_MS) {
            i2c_request_recovery(I2C_RECOVER_REASON_LINE_LOW);
        }
    } else {
        i2c_line_low_since_ms = 0U;
    }

    const uint32_t valid_age = now - i2c_last_valid_frame_ms;
    if (valid_age >= I2C_NO_VALID_FRAME_RESET_MS) {
        NVIC_SystemReset();
    } else if (valid_age >= I2C_NO_VALID_FRAME_RECOVER_MS) {
        if (!dbg_no_valid_reset_armed) {
            dbg_no_valid_reset_armed = 1U;
            i2c_request_recovery(I2C_RECOVER_REASON_NO_VALID_FRAME);
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

        // If RX re-arm fails, recover from main-loop context.
        i2c_request_recovery(I2C_RECOVER_REASON_ARM_FAILED);
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
    tx_started_ms = 0;
    pending_price_valid = 0;
    pending_rgb_valid = 0;
    pending_mode_valid = 0;

    i2c_last_activity_ms = HAL_GetTick();
    i2c_last_valid_frame_ms = i2c_last_activity_ms;
    i2c_last_recover_ms = 0;
    i2c_bus_busy_since_ms = 0;
    i2c_line_low_since_ms = 0;
    i2c_recover_window_start_ms = i2c_last_activity_ms;
    i2c_recover_window_count = 0;
    i2c_recovery_requested = 0;
    i2c_recovery_reason = 0;
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
        tx_started_ms = HAL_GetTick();
        dbg_tx_start_ok++;
        i2c_note_activity();
    } else {
        tx_busy = 0;
        tx_pending = 0;
        tx_started_ms = 0;
        dbg_tx_start_busy++;
        i2c_request_recovery(I2C_RECOVER_REASON_TX_STUCK);
    }
}

static void i2c_apply_pending_commands(void)
{
    uint8_t have_price = 0U;
    uint16_t price = 0U;
    uint8_t have_rgb = 0U;
    uint8_t r = 0U;
    uint8_t g = 0U;
    uint8_t b = 0U;
    uint8_t have_mode = 0U;
    uint8_t mode = 0U;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    if (pending_price_valid) {
        have_price = 1U;
        price = pending_price;
        pending_price_valid = 0U;
    }

    if (pending_rgb_valid) {
        have_rgb = 1U;
        r = pending_rgb_r;
        g = pending_rgb_g;
        b = pending_rgb_b;
        pending_rgb_valid = 0U;
    }

    if (pending_mode_valid) {
        have_mode = 1U;
        mode = pending_mode;
        pending_mode_valid = 0U;
    }
    __set_PRIMASK(primask);

    if (have_price) {
        app_set_price(price);
    }

    if (have_rgb) {
        app_set_rgb(r, g, b);
    }

    if (have_mode) {
        app_set_light_mode(mode);
    }
}

void i2c_slave_debug_task(void)
{
    static uint32_t last_rx_seen = 0;
    static uint32_t last_err_seen = 0;

    // Print only when new RX message arrived or new I2C error occurred
    if (dbg_rx_cplt == last_rx_seen && dbg_err_cplt == last_err_seen) {
        return;
    }

    last_rx_seen = dbg_rx_cplt;
    last_err_seen = dbg_err_cplt;

    uint32_t now = HAL_GetTick();

    terminal_printf("I2C rx=%lu tx=%lu err=%lu arm=%lu/%lu txs=%lu/%lu cmd=%02X act=%u rxA=%u txB=%u\r\n",
                    (unsigned long)dbg_rx_cplt,
                    (unsigned long)dbg_tx_cplt,
                    (unsigned long)dbg_err_cplt,
                    (unsigned long)dbg_arm_rx_ok,
                    (unsigned long)dbg_arm_rx_busy,
                    (unsigned long)dbg_tx_start_ok,
                    (unsigned long)dbg_tx_start_busy,
                    (unsigned int)dbg_last_cmd,
                    (unsigned int)dbg_last_action,
                    (unsigned int)rx_armed,
                    (unsigned int)tx_busy);

    terminal_printf("I2C st=%02X rec=%lu act=%lu valid=%lu err=%08lX isr=%08lX why=%u\r\n",
                    (unsigned int)hi2c1.State,
                    (unsigned long)dbg_recover,
                    (unsigned long)(now - i2c_last_activity_ms),
                    (unsigned long)(now - i2c_last_valid_frame_ms),
                    (unsigned long)dbg_last_hal_error,
                    (unsigned long)dbg_last_isr,
                    (unsigned int)dbg_last_recover_reason);
}

void i2c_slave_watchdog_task(void)
{
    i2c_apply_pending_commands();

    // keep RX callback armed whenever possible
    if (!rx_armed && !tx_busy && hi2c1.State == HAL_I2C_STATE_READY) {
        arm_rx(&hi2c1);
    }

    i2c_monitor_for_stuck_bus(&hi2c1);
    i2c_perform_recovery_if_requested(&hi2c1);
}

uint32_t i2c_slave_take_error_flags(void)
{
    uint32_t flags = i2c_error_flags;
    i2c_error_flags = 0;
    return flags;
}

uint8_t i2c_slave_is_quiet(uint32_t min_quiet_ms)
{
    const uint32_t now = HAL_GetTick();

    if (tx_busy || i2c_recovery_requested) {
        return 0U;
    }

    if ((hi2c1.Instance->ISR & I2C_FLAG_BUSY) != 0U) {
        return 0U;
    }

    return ((now - i2c_last_activity_ms) >= min_quiet_ms) ? 1U : 0U;
}

static void send_reply_frame(uint8_t reply_cmd, uint16_t data16)
{
    dbg_last_reply_cmd = reply_cmd;
    dbg_last_reply_data = data16;
    dbg_last_reply_valid = 1U;

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
    dbg_last_rx_b0 = rx4[0];
    dbg_last_rx_b1 = rx4[1];
    dbg_last_rx_b2 = rx4[2];
    dbg_last_rx_b3 = rx4[3];
    dbg_last_reply_valid = 0U;
    dbg_last_action = DBG_ACT_NONE;

    switch (cmd)
    {
        case CMD_SET_PRICE:
        {
            dbg_last_action = DBG_ACT_SET_PRICE;
            pending_price = (uint16_t)((rx4[1] << 8) | rx4[2]);
            pending_price_valid = 1U;
            i2c_note_valid_frame();

        } break;

        case CMD_SET_RGB:
        {
            dbg_last_action = DBG_ACT_SET_RGB;
            pending_rgb_r = rx4[1];
            pending_rgb_g = rx4[2];
            pending_rgb_b = rx4[3];
            pending_rgb_valid = 1U;
            i2c_note_valid_frame();

        } break;

        case CMD_SET_LIGHT_MODE:
        {
            dbg_last_action = DBG_ACT_SET_LIGHT_MODE;
            // Lighting mode command payload in B1
            // 0   -> keep current color (no change)
            // 255 -> green for 500 ms, then restore previous/base color
            // else-> run selected light show mode
            pending_mode = rx4[1];
            pending_mode_valid = 1U;
            i2c_note_valid_frame();

        } break;

        case CMD_GET_TEMP:
        {
            dbg_last_action = DBG_ACT_GET_TEMP;
            // Reply with temperature * 100 in D1:D2 (int16, big-endian)
            int16_t t100 = app_get_temp100();
            i2c_note_valid_frame();
            send_reply_frame(REPLY_TEMP, (uint16_t)t100);
        } break;

        case CMD_GET_ALL:
        {
            dbg_last_action = DBG_ACT_GET_ALL;
            i2c_note_valid_frame();
            send_reply_frame(REPLY_ALL_DATA, app_get_price());
        } break;

        default:
        {
            dbg_last_action = DBG_ACT_UNKNOWN;

            // Accept any unknown 4-byte frame silently.
            // Intentionally do not send an error reply.
        } break;
    }

    // Re-arm immediately for write-only commands. Reply commands re-arm after
    // TX completes; if master never reads the reply, the TX timeout recovers.
    if (!tx_busy) {
        arm_rx(hi2c);
    }
}

// Called when slave finished transmitting reply bytes
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance != I2C1) return;
    dbg_tx_cplt++;
    i2c_note_activity();
    tx_busy = 0;
    tx_pending = 0;
    tx_started_ms = 0;

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
    dbg_last_hal_error = HAL_I2C_GetError(hi2c);
    dbg_last_isr = hi2c->Instance->ISR;
    i2c_error_flags |= dbg_last_hal_error;
    tx_busy = 0;
    tx_pending = 0;
    tx_len  = 0;
    tx_started_ms = 0;
    rx_armed = 0;

    // Fast path: try re-arm first
    arm_rx(hi2c);

    // If RX could not be re-armed, recover from main-loop context.
    if (!rx_armed) {
        i2c_request_recovery(I2C_RECOVER_REASON_HAL_STATE);
    }
}
