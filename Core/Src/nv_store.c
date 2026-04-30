#include "nv_store.h"

#include "main.h"

#include <stddef.h>
#include <string.h>

#define NV_MAGIC               0x4C4B5231UL  // "LKR1"
#define NV_PAGE_INDEX          (FLASH_PAGE_NB - 1U)
#define NV_PAGE_ADDR           (FLASH_BASE + (NV_PAGE_INDEX * FLASH_PAGE_SIZE))

typedef struct {
    uint32_t magic;
    uint16_t price;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  mode;
    uint16_t seq;
    uint16_t crc;
    uint16_t reserved;
} nv_record_t;

#define NV_RECORD_COUNT        (FLASH_PAGE_SIZE / (uint32_t)sizeof(nv_record_t))

static volatile uint8_t s_dirty = 0U;
static nv_store_state_t s_pending_state = {0};
static uint16_t s_next_seq = 0U;

static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static uint16_t nv_record_crc(const nv_record_t *rec)
{
    return crc16_ccitt((const uint8_t *)rec, (uint32_t)offsetof(nv_record_t, crc));
}

static uint8_t nv_slot_is_blank(const nv_record_t *slot)
{
    const uint8_t *p = (const uint8_t *)slot;
    for (uint32_t i = 0; i < (uint32_t)sizeof(nv_record_t); i++) {
        if (p[i] != 0xFFU) {
            return 0U;
        }
    }
    return 1U;
}

static uint8_t nv_record_is_valid(const nv_record_t *rec)
{
    if (rec->magic != NV_MAGIC) {
        return 0U;
    }

    if (rec->crc != nv_record_crc(rec)) {
        return 0U;
    }

    return 1U;
}

static uint32_t nv_find_free_slot(void)
{
    const nv_record_t *page = (const nv_record_t *)NV_PAGE_ADDR;

    for (uint32_t i = 0; i < NV_RECORD_COUNT; i++) {
        if (nv_slot_is_blank(&page[i])) {
            return i;
        }
    }

    return NV_RECORD_COUNT;
}

static HAL_StatusTypeDef nv_erase_page(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.Page = NV_PAGE_INDEX;
    erase.NbPages = 1;

    return HAL_FLASHEx_Erase(&erase, &page_error);
}

static HAL_StatusTypeDef nv_program_record(uint32_t slot_index, const nv_record_t *rec)
{
    const uint32_t addr = NV_PAGE_ADDR + slot_index * (uint32_t)sizeof(nv_record_t);
    uint64_t dword = 0ULL;

    memcpy(&dword, rec, sizeof(uint64_t));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dword) != HAL_OK) {
        return HAL_ERROR;
    }

    dword = 0ULL;
    memcpy(&dword, ((const uint8_t *)rec) + sizeof(uint64_t), sizeof(uint64_t));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + 8U, dword) != HAL_OK) {
        return HAL_ERROR;
    }

    return HAL_OK;
}

static uint8_t nv_take_snapshot_if_dirty(nv_store_state_t *out)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (s_dirty == 0U) {
        __set_PRIMASK(primask);
        return 0U;
    }

    *out = s_pending_state;
    s_dirty = 0U;

    __set_PRIMASK(primask);
    return 1U;
}

void nv_store_init(void)
{
    s_dirty = 0U;
    s_next_seq = 0U;
}

void nv_store_load(nv_store_state_t *out, const nv_store_state_t *defaults)
{
    if (out == NULL) {
        return;
    }

    if (defaults != NULL) {
        *out = *defaults;
    } else {
        memset(out, 0, sizeof(*out));
    }

    const nv_record_t *page = (const nv_record_t *)NV_PAGE_ADDR;
    const nv_record_t *latest = NULL;

    s_next_seq = 0U;

    for (uint32_t i = 0; i < NV_RECORD_COUNT; i++) {
        const nv_record_t *rec = &page[i];
        if (nv_record_is_valid(rec)) {
            latest = rec;
        }
    }

    if (latest != NULL) {
        out->price = latest->price;
        out->r = latest->r;
        out->g = latest->g;
        out->b = latest->b;
        out->mode = latest->mode;
        s_next_seq = (uint16_t)(latest->seq + 1U);
    }

    s_pending_state = *out;
}

void nv_store_request_save(const nv_store_state_t *state)
{
    if (state == NULL) {
        return;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_pending_state = *state;
    s_dirty = 1U;
    __set_PRIMASK(primask);
}

void nv_store_task(void)
{
    nv_store_state_t snapshot = {0};

    if (!nv_take_snapshot_if_dirty(&snapshot)) {
        return;
    }

    nv_record_t rec = {0};
    rec.magic = NV_MAGIC;
    rec.price = snapshot.price;
    rec.r = snapshot.r;
    rec.g = snapshot.g;
    rec.b = snapshot.b;
    rec.mode = snapshot.mode;
    rec.seq = s_next_seq;
    rec.reserved = 0U;
    rec.crc = nv_record_crc(&rec);

    uint32_t slot_index = nv_find_free_slot();
    HAL_StatusTypeDef st = HAL_OK;

    HAL_FLASH_Unlock();

    if (slot_index >= NV_RECORD_COUNT) {
        st = nv_erase_page();
        slot_index = 0U;
    }

    if (st == HAL_OK) {
        st = nv_program_record(slot_index, &rec);
    }

    HAL_FLASH_Lock();

    if (st == HAL_OK) {
        s_next_seq = (uint16_t)(s_next_seq + 1U);
    } else {
        nv_store_request_save(&snapshot);
    }
}

