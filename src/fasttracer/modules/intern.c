#include "fasttracer.h"
#include <stdlib.h>
#include <string.h>

/* ── Pointer intern table (open-addressing, linear probing) ────────── */

int
intern_init(struct InternTable* table)
{
    table->capacity = FT_INTERN_INITIAL_CAP;
    table->count = 0;
    table->entries = calloc(table->capacity, sizeof(struct InternEntry));
    if (!table->entries) return -1;
    return 0;
}

void
intern_free(struct InternTable* table)
{
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

static inline uint32_t
hash_ptr(void* p, uint32_t mask)
{
    /* Fibonacci hashing on pointer value */
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
    return (uint32_t)v & mask;
}

uint32_t
intern_lookup(struct InternTable* table, void* key)
{
    uint32_t mask = table->capacity - 1;
    uint32_t idx = hash_ptr(key, mask);

    for (;;) {
        struct InternEntry* e = &table->entries[idx];
        if (e->key == NULL) return 0;        /* empty slot, not found */
        if (e->key == key) return e->func_id;
        idx = (idx + 1) & mask;
    }
}

static int
intern_grow(struct InternTable* table)
{
    uint32_t new_cap = table->capacity * 2;
    struct InternEntry* new_entries = calloc(new_cap, sizeof(struct InternEntry));
    if (!new_entries) return -1;

    uint32_t new_mask = new_cap - 1;
    for (uint32_t i = 0; i < table->capacity; i++) {
        struct InternEntry* e = &table->entries[i];
        if (e->key == NULL) continue;
        uint32_t idx = hash_ptr(e->key, new_mask);
        while (new_entries[idx].key != NULL) {
            idx = (idx + 1) & new_mask;
        }
        new_entries[idx] = *e;
    }

    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_cap;
    return 0;
}

int
intern_insert(struct InternTable* table, void* key, uint32_t func_id)
{
    /* Grow at 70% load */
    if (table->count * 10 >= table->capacity * 7) {
        if (intern_grow(table) < 0) return -1;
    }

    uint32_t mask = table->capacity - 1;
    uint32_t idx = hash_ptr(key, mask);

    for (;;) {
        struct InternEntry* e = &table->entries[idx];
        if (e->key == NULL) {
            e->key = key;
            e->func_id = func_id;
            table->count++;
            return 0;
        }
        if (e->key == key) {
            /* already exists */
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

/* ── String table ──────────────────────────────────────────────────── */

#define ST_INITIAL_CAP 4096

int
string_table_init(struct StringTable* st)
{
    st->len = 0;
    st->cap = ST_INITIAL_CAP;
    st->data = malloc(st->cap);
    if (!st->data) return -1;
    return 0;
}

void
string_table_free(struct StringTable* st)
{
    free(st->data);
    st->data = NULL;
    st->len = 0;
    st->cap = 0;
}

int
string_table_append(struct StringTable* st, const char* str, uint32_t len)
{
    size_t need = st->len + 4 + len;  /* 4 bytes for uint32 length prefix */
    if (need > st->cap) {
        size_t new_cap = st->cap;
        while (new_cap < need) new_cap *= 2;
        char* new_data = realloc(st->data, new_cap);
        if (!new_data) return -1;
        st->data = new_data;
        st->cap = new_cap;
    }

    /* Write [uint32_t len][char data[len]] */
    memcpy(st->data + st->len, &len, 4);
    memcpy(st->data + st->len + 4, str, len);
    st->len += 4 + len;
    return 0;
}
