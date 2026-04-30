#ifndef NV_STORE_H
#define NV_STORE_H

#include <stdint.h>

typedef struct {
    uint16_t price;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  mode;
} nv_store_state_t;

void nv_store_init(void);

// Load latest valid state from flash log.
// If no valid record exists, *out is initialized from *defaults.
void nv_store_load(nv_store_state_t *out, const nv_store_state_t *defaults);

// Safe to call from ISR or main context.
void nv_store_request_save(const nv_store_state_t *state);

// Call periodically from main loop (non-ISR context) to perform flash writes.
void nv_store_task(void);

#endif
