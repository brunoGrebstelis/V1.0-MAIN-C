#include "lighting_mode.h"

#include "main.h"
#include "rgb.h"
#include "app.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

typedef struct {
    uint8_t active;
    uint8_t mode;
    uint8_t phase;
    uint8_t step;
    uint32_t last_tick;
} lighting_state_t;

static volatile uint8_t s_base_r = 150;
static volatile uint8_t s_base_g = 150;
static volatile uint8_t s_base_b = 150;

static lighting_state_t s_state = {0};
static lighting_state_t s_saved_state = {0};

static rgb_color_t s_palette[6] = {
    {255,   0,   0}, // red
    {255, 140,   0}, // orange
    {255, 255,   0}, // yellow
    {  0, 255,   0}, // green
    {  0,   0, 255}, // blue
    {180,   0, 255}  // violet
};

#define PSY_FADE_STEP_MS   20U
#define PSY_FADE_STEPS     90U

static uint8_t clamp_u8(int16_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, uint8_t step, uint8_t steps_total)
{
    if (steps_total == 0U) {
        return b;
    }

    const int16_t delta = (int16_t)b - (int16_t)a;
    const int32_t v = (int32_t)a +
                      (((int32_t)delta * (int32_t)step + (int32_t)(steps_total / 2U)) /
                       (int32_t)steps_total);
    return clamp_u8((int16_t)v);
}

static rgb_color_t scale_color(rgb_color_t c, uint8_t scale)
{
    rgb_color_t out;
    out.r = (uint8_t)(((uint16_t)c.r * scale) / 255U);
    out.g = (uint8_t)(((uint16_t)c.g * scale) / 255U);
    out.b = (uint8_t)(((uint16_t)c.b * scale) / 255U);
    return out;
}

static rgb_color_t shift_color(rgb_color_t c, int8_t shift)
{
    rgb_color_t out;
    out.r = clamp_u8((int16_t)c.r + shift);
    out.g = clamp_u8((int16_t)c.g + shift);
    out.b = clamp_u8((int16_t)c.b + shift);
    return out;
}

static uint8_t map_idx(uint8_t idx, uint8_t len, uint8_t start, uint8_t reverse)
{
    uint8_t pos = reverse ? (uint8_t)((len - 1U - idx) % len) : (uint8_t)(idx % len);
    return (uint8_t)((start + pos) % len);
}

static rgb_color_t locker_palette_color(uint8_t idx)
{
    const uint8_t locker = app_get_locker_id();
    const uint8_t start = (uint8_t)(locker % 6U);
    const uint8_t reverse = (uint8_t)((locker / 6U) & 0x01U);
    const uint8_t bri_sel = (uint8_t)((locker / 12U) % 3U);

    uint8_t scale = 255U;
    if (bri_sel == 1U) scale = 210U;
    if (bri_sel == 2U) scale = 170U;

    rgb_color_t c = s_palette[map_idx(idx, 6U, start, reverse)];
    return scale_color(c, scale);
}

static rgb_color_t locker_theme_color(uint8_t idx)
{
    // 5-color warm gradient family; locker id auto-selects order and brightness/shift
    static const rgb_color_t warm_theme[5] = {
        {255, 255,   0}, // yellow
        {255, 200,   0}, // amber
        {255, 150,   0}, // orange
        {255,  80,   0}, // deep orange
        {255,   0,   0}  // red
    };

    const uint8_t locker = app_get_locker_id();
    const uint8_t start = (uint8_t)((locker * 3U) % 5U);
    const uint8_t reverse = (uint8_t)((locker >> 1) & 0x01U);
    const int8_t shift = (int8_t)(((int16_t)(locker % 5U) - 2) * 10); // -20..+20

    rgb_color_t c = warm_theme[map_idx(idx, 5U, start, reverse)];
    return shift_color(c, shift);
}

static void apply_base_color(void)
{
    rgb_set(s_base_r, s_base_g, s_base_b);
}

static void stop_show_and_restore(void)
{
    s_state.active = 0;
    s_state.mode = 0;
    s_state.phase = 0;
    s_state.step = 0;
    s_state.last_tick = HAL_GetTick();
    apply_base_color();
}

static void restore_saved_mode_after_flash(void)
{
    if (s_saved_state.active && s_saved_state.mode != 0xFFU) {
        s_state = s_saved_state;
        s_state.last_tick = HAL_GetTick();
    } else {
        stop_show_and_restore();
    }

    s_saved_state.active = 0;
    s_saved_state.mode = 0;
    s_saved_state.phase = 0;
    s_saved_state.step = 0;
    s_saved_state.last_tick = 0;
}

void lighting_mode_init(void)
{
    s_state.active = 0;
    s_state.mode = 0;
    s_state.phase = 0;
    s_state.step = 0;
    s_state.last_tick = HAL_GetTick();
}

void lighting_mode_set_base_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_base_r = r;
    s_base_g = g;
    s_base_b = b;

    if (!s_state.active) {
        apply_base_color();
    }
}

void lighting_mode_set(uint8_t modeVal)
{
    // mode 0: keep current color (stop any running show)
    if (modeVal == 0U) {
        s_state.active = 0;
        s_state.mode = 0;
        s_state.phase = 0;
        s_state.step = 0;
        s_state.last_tick = HAL_GetTick();
        return;
    }

    // mode 255: green 500 ms, then back to previous/base color
    if (modeVal == 0xFFU) {
        s_saved_state = s_state;
        s_state.active = 1;
        s_state.mode = 0xFFU;
        s_state.phase = 0;
        s_state.step = 0;
        s_state.last_tick = HAL_GetTick();
        rgb_set(0, 255, 0);
        return;
    }

    // other mode numbers run light show
    s_state.active = 1;
    s_state.mode = modeVal;
    s_state.phase = 0;
    s_state.step = 0;
    s_state.last_tick = HAL_GetTick();
}

// kept for compatibility with the API name you requested
void run_light_show(uint8_t modeVal)
{
    lighting_mode_set(modeVal);
}

void lighting_mode_task(void)
{
    if (!s_state.active) return;

    const uint32_t now = HAL_GetTick();

    if (s_state.mode == 0xFFU) {
        if ((now - s_state.last_tick) >= 500U) {
            restore_saved_mode_after_flash();
        }
        return;
    }

    switch (s_state.mode)
    {
        case 1: // pink <-> red style (locker adjusted)
        {
            if ((now - s_state.last_tick) < 1000U) return;
            s_state.last_tick = now;

            const uint8_t locker = app_get_locker_id();
            const uint8_t pink_g = (uint8_t)(40U + (locker % 4U) * 10U);   // 40..70
            const uint8_t pink_b = (uint8_t)(90U + (locker % 5U) * 12U);   // 90..138

            if (s_state.phase == 0U) {
                rgb_set(255, pink_g, pink_b);
                s_state.phase = 1U;
            } else {
                rgb_set(255, 0, 0);
                s_state.phase = 0U;
            }
        } break;

        case 2: // disco palette with locker-based order/range
        {
            if ((now - s_state.last_tick) < 300U) return;
            s_state.last_tick = now;

            rgb_color_t c = locker_palette_color(s_state.step);
            rgb_set(c.r, c.g, c.b);
            s_state.step = (uint8_t)((s_state.step + 1U) % 6U);
        } break;

        case 3: // psychedelic smooth fade through warm theme (locker-adjusted)
        {
            if ((now - s_state.last_tick) < PSY_FADE_STEP_MS) return;
            s_state.last_tick = now;

            const uint8_t from_idx = (uint8_t)(s_state.phase % 5U);
            const uint8_t to_idx = (uint8_t)((from_idx + 1U) % 5U);
            const rgb_color_t from = locker_theme_color(from_idx);
            const rgb_color_t to = locker_theme_color(to_idx);

            rgb_color_t c;
            c.r = lerp_u8(from.r, to.r, s_state.step, PSY_FADE_STEPS);
            c.g = lerp_u8(from.g, to.g, s_state.step, PSY_FADE_STEPS);
            c.b = lerp_u8(from.b, to.b, s_state.step, PSY_FADE_STEPS);
            rgb_set(c.r, c.g, c.b);

            if (s_state.step >= PSY_FADE_STEPS) {
                s_state.step = 0U;
                s_state.phase = (uint8_t)((s_state.phase + 1U) % 5U);
            } else {
                s_state.step++;
            }
        } break;

        case 4: // welcome style white->green->white, timings slightly id dependent
        {
            const uint8_t locker = app_get_locker_id();
            const uint32_t t_white_long = 2400U + (uint32_t)(locker % 7U) * 100U;
            const uint32_t t_green = 700U + (uint32_t)(locker % 5U) * 60U;
            const uint32_t t_white_short = 700U + (uint32_t)(locker % 3U) * 80U;

            if (s_state.phase == 0U) {
                rgb_set(255, 255, 255);
                s_state.phase = 1U;
                s_state.last_tick = now;
                return;
            }

            if (s_state.phase == 1U && (now - s_state.last_tick) >= t_white_long) {
                rgb_set(0, 255, 0);
                s_state.phase = 2U;
                s_state.last_tick = now;
                return;
            }

            if (s_state.phase == 2U && (now - s_state.last_tick) >= t_green) {
                rgb_set(255, 255, 255);
                s_state.phase = 3U;
                s_state.last_tick = now;
                return;
            }

            if (s_state.phase == 3U && (now - s_state.last_tick) >= t_white_short) {
                s_state.phase = 1U;
                s_state.last_tick = now;
            }
        } break;

        case 5: // solid disco (locker-based order/range)
        {
            if ((now - s_state.last_tick) < 300U) return;
            s_state.last_tick = now;

            rgb_color_t c = locker_palette_color(s_state.step);
            c = shift_color(c, -20); // slightly deeper tones vs mode 2
            rgb_set(c.r, c.g, c.b);
            s_state.step = (uint8_t)((s_state.step + 1U) % 6U);
        } break;

        default:
            stop_show_and_restore();
            break;
    }
}
