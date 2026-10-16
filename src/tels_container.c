/* tels_container.c — رجوع کن به tels_container.h */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tels_container.h"

#define TELS_SB_SIZE           4096u
#define TELS_SB_SLOT_A         0u
#define TELS_SB_SLOT_B         4096u
#define TELS_DATA_START        8192u
#define TELS_VERSION           1u
#define TELS_REC_MAGIC         0x4345524Cu   /* بایت‌ها: 4C 52 45 43 */
#define TELS_REC_HDR           40u
#define TELS_IDX_MAGIC         0x58444949u   /* "IIDX" */
#define TELS_IDX_VERSION       1u
#define TELS_MAX_INDEX         (256u * 1024u * 1024u)
#define TELS_CHUNK             65536u
#define TELS_MAX_EXT_PER_ENTRY 4000000u
#define TELS_EXT_BYTES         20u           /* off(8) + len(4) + pos(8) */

enum { REC_DATA = 1, REC_CREATE = 2, REC_DELETE = 3, REC_INDEX = 4 };

typedef struct {
    uint64_t off;     /* آفست منطقی داخل entry */
    uint32_t len;
    uint64_t pos;     /* آفست بایت‌ها در فایل */
} TelsExtent;

typedef struct {
    TelsId     id;
    TelsId     parent;
    uint8_t    type;
    uint8_t    name_len;
    char       name[TELS_MAX_NAME + 1];
    uint64_t   size;
    TelsExtent *ext;
    uint32_t   ext_count;
} TelsEntry;

struct TelsContainer {
    HANDLE           file;
    CRITICAL_SECTION lock;
    BOOL             writable;
    TelsEntry      **entries;      /* entries[id]؛ NULL یعنی خالی یا حذف‌شده */
    uint32_t         entry_cap;
    uint32_t         next_id;      /* شناسه‌ی بعدی؛ 0 برای ریشه رزرو است */
    uint64_t         append_pos;   /* انتهای رکوردهای معتبر */
    uint64_t         gen;          /* نسل آخرین commit */
};

/* ───── CRC32 (IEEE) ───── */

static uint32_t g_crc_table[256];
static volatile LONG g_crc_state = 0;   /* 0 خام، 1 در حال ساخت، 2 آماده */

static void crc_init(void) {
    if (g_crc_state == 2) return;
    if (InterlockedCompareExchange(&g_crc_state, 1, 0) == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crc_table[i] = c;
        }
        InterlockedExchange(&g_crc_state, 2);
    } else {
        while (g_crc_state != 2) Sleep(0);
    }
}

static uint32_t crc32_feed(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = g_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ───── کدگذاری little-endian ───── */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}
static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* ───── ورودی/خروجی فایل (زیر قفل صدا زده شود) ───── */

static BOOL read_at(TelsContainer *c, uint64_t pos, void *buf, uint32_t len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)pos;
    if (!SetFilePointerEx(c->file, li, NULL, FILE_BEGIN)) return FALSE;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        DWORD got = 0;
        if (!ReadFile(c->file, p + done, len - done, &got, NULL) || got == 0) return FALSE;
        done += got;
    }
    return TRUE;
}

static BOOL write_at(TelsContainer *c, uint64_t pos, const void *buf, uint32_t len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)pos;
    if (!SetFilePointerEx(c->file, li, NULL, FILE_BEGIN)) return FALSE;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        DWORD put = 0;
        if (!WriteFile(c->file, p + done, len - done, &put, NULL) || put == 0) return FALSE;
        done += put;
    }
    return TRUE;
}

static uint64_t file_size(TelsContainer *c) {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(c->file, &li)) return 0;
    return (uint64_t)li.QuadPart;
}

/* ───── نام‌ها و عمق ───── */

static BOOL name_valid(const char *name) {
    if (!name) return FALSE;
    size_t n = strlen(name);
    if (n == 0 || n > TELS_MAX_NAME) return FALSE;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return FALSE;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) return FALSE;
        if (strchr("/\\:*?\"<>|", (int)*p)) return FALSE;
    }
    return TRUE;
}

static TelsEntry *entry_get(TelsContainer *c, TelsId id) {
    if (id == TELS_ROOT || id >= c->next_id || id >= c->entry_cap) return NULL;
    return c->entries[id];
}

static TelsEntry *find_child(TelsContainer *c, TelsId parent, const char *name) {
    for (TelsId i = 1; i < c->next_id; i++) {
        TelsEntry *e = c->entries[i];
        if (e && e->parent == parent && _stricmp(e->name, name) == 0) return e;
    }
    return NULL;
}

/* عمق یک entry: فرزند مستقیم ریشه عمق 1 دارد؛ ریشه عمق 0 */
static int entry_depth(TelsContainer *c, TelsId id) {
    int d = 0;
    TelsId p = id;
    while (p != TELS_ROOT) {
        TelsEntry *e = entry_get(c, p);
        if (!e) return -1;
        p = e->parent;
        d++;
        if (d > (int)TELS_MAX_DEPTH + 1) return -1;
    }
    return d;
}

/* ───── entry در حافظه ───── */

static BOOL entry_slot(TelsContainer *c, TelsId id) {
    if (id >= TELS_MAX_ENTRIES) return FALSE;
    if (id < c->entry_cap) return TRUE;
    uint32_t ncap = c->entry_cap ? c->entry_cap : 64u;
    while (ncap <= id) ncap *= 2u;
    if (ncap > TELS_MAX_ENTRIES) ncap = TELS_MAX_ENTRIES;
    TelsEntry **n = (TelsEntry **)realloc(c->entries, (size_t)ncap * sizeof(*n));
    if (!n) return FALSE;
    memset(n + c->entry_cap, 0, (size_t)(ncap - c->entry_cap) * sizeof(*n));
    c->entries = n;
    c->entry_cap = ncap;
    return TRUE;
}

static TelsEntry *entry_new(TelsContainer *c, TelsId id, TelsId parent, uint8_t type,
                            const char *name, uint8_t name_len) {
    if (!entry_slot(c, id)) return NULL;
    TelsEntry *e = (TelsEntry *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->id = id;
    e->parent = parent;
    e->type = type;
    e->name_len = name_len;
    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    c->entries[id] = e;
    if (id >= c->next_id) c->next_id = id + 1u;
    return e;
}

static void entry_free(TelsEntry *e) {
    if (!e) return;
    free(e->ext);
    free(e);
}

static void entries_clear(TelsContainer *c) {
    for (uint32_t i = 0; i < c->entry_cap; i++) {
        entry_free(c->entries[i]);
        c->entries[i] = NULL;
    }
    c->next_id = 1;
}

/* افزودن یک extent و بریدن بخش‌های همپوشان قدیمی؛ extent ها همیشه مرتب و جدا هستند */
static BOOL entry_add_extent(TelsEntry *e, uint64_t off, uint32_t len, uint64_t pos) {
    uint64_t end = off + len;
    TelsExtent *n = (TelsExtent *)malloc(sizeof(TelsExtent) * ((size_t)e->ext_count + 2u));
    if (!n) return FALSE;
    uint32_t k = 0;
    for (uint32_t i = 0; i < e->ext_count; i++) {
        TelsExtent *x = &e->ext[i];
        uint64_t xend = x->off + x->len;
        if (xend <= off || x->off >= end) {
            n[k++] = *x;
            continue;
        }
        if (x->off < off) {
            n[k].off = x->off;
            n[k].len = (uint32_t)(off - x->off);
            n[k].pos = x->pos;
            k++;
        }
        if (xend > end) {
            n[k].off = end;
            n[k].len = (uint32_t)(xend - end);
            n[k].pos = x->pos + (end - x->off);
            k++;
        }
    }
    n[k].off = off;
    n[k].len = len;
    n[k].pos = pos;
    k++;
    for (uint32_t i = 1; i < k; i++) {       /* مرتب‌سازی درجی */
        TelsExtent t = n[i];
        uint32_t j = i;
        while (j > 0 && n[j - 1].off > t.off) {
            n[j] = n[j - 1];
            j--;
        }
        n[j] = t;
    }
    free(e->ext);
    e->ext = n;
    e->ext_count = k;
    if (end > e->size) e->size = end;
    return TRUE;
}

/* حذف entry و تمام زیرمجموعه‌ها؛ هر entry دقیقاً یک بار دیده می‌شود */
static void entry_delete_subtree(TelsContainer *c, TelsId root) {
    TelsId *stack = (TelsId *)malloc(sizeof(TelsId) * TELS_MAX_ENTRIES);
    if (!stack) return;
    uint32_t sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        TelsId x = stack[--sp];
        for (TelsId i = 1; i < c->next_id && sp < TELS_MAX_ENTRIES; i++) {
            TelsEntry *e = c->entries[i];
            if (e && e->parent == x) stack[sp++] = i;
        }
        if (x != TELS_ROOT && x < c->entry_cap) {
            entry_free(c->entries[x]);
            c->entries[x] = NULL;
        }
    }
    free(stack);
}

/* ───── رکوردها ───── */

static BOOL append_record(TelsContainer *c, uint8_t kind, TelsId id, uint64_t off,
                          const void *payload, uint32_t plen,
                          uint64_t *rec_pos, uint64_t *payload_pos) {
    uint8_t hdr[TELS_REC_HDR];
    memset(hdr, 0, sizeof(hdr));
    put_u32(hdr, TELS_REC_MAGIC);
    put_u32(hdr + 4, id);
    hdr[8] = kind;
    put_u64(hdr + 12, off);
    put_u32(hdr + 20, plen);
    uint32_t crc = crc32_feed(0, hdr, 32);
    if (plen) crc = crc32_feed(crc, (const uint8_t *)payload, plen);
    put_u32(hdr + 32, crc);

    uint64_t pos = c->append_pos;
    if (!write_at(c, pos, hdr, TELS_REC_HDR)) return FALSE;
    if (plen && !write_at(c, pos + TELS_REC_HDR, payload, plen)) return FALSE;
    if (rec_pos) *rec_pos = pos;
    if (payload_pos) *payload_pos = pos + TELS_REC_HDR;
    c->append_pos = pos + TELS_REC_HDR + plen;
    return TRUE;
}

/* ───── index ───── */

static BOOL serialize_index(TelsContainer *c, uint8_t **out, uint32_t *out_len) {
    uint32_t count = 0;
    uint64_t total = 16;
    for (TelsId i = 1; i < c->next_id; i++) {
        TelsEntry *e = c->entries[i];
        if (!e) continue;
        count++;
        total += 10u + e->name_len + 8u + 4u + (uint64_t)e->ext_count * TELS_EXT_BYTES;
    }
    if (total > TELS_MAX_INDEX) return FALSE;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return FALSE;
    uint8_t *p = buf;
    put_u32(p, TELS_IDX_MAGIC);     p += 4;
    put_u32(p, TELS_IDX_VERSION);   p += 4;
    put_u32(p, c->next_id);         p += 4;
    put_u32(p, count);              p += 4;
    for (TelsId i = 1; i < c->next_id; i++) {
        TelsEntry *e = c->entries[i];
        if (!e) continue;
        put_u32(p, e->id);          p += 4;
        put_u32(p, e->parent);      p += 4;
        *p++ = e->type;
        *p++ = e->name_len;
        memcpy(p, e->name, e->name_len);
        p += e->name_len;
        put_u64(p, e->size);        p += 8;
        put_u32(p, e->ext_count);   p += 4;
        for (uint32_t j = 0; j < e->ext_count; j++) {
            put_u64(p, e->ext[j].off);   p += 8;
            put_u32(p, e->ext[j].len);   p += 4;
            put_u64(p, e->ext[j].pos);   p += 8;
        }
    }
    *out = buf;
    *out_len = (uint32_t)total;
    return TRUE;
}

/* index را کامل می‌خواند و state را جایگزین می‌کند؛ در خطا state خالی می‌ماند */
static BOOL parse_index(TelsContainer *c, const uint8_t *buf, uint32_t len) {
    entries_clear(c);
    if (len < 16) return FALSE;
    if (get_u32(buf) != TELS_IDX_MAGIC || get_u32(buf + 4) != TELS_IDX_VERSION) return FALSE;
    uint32_t next_id = get_u32(buf + 8);
    uint32_t count   = get_u32(buf + 12);
    if (next_id < 1 || next_id > TELS_MAX_ENTRIES || count > next_id) return FALSE;

    const uint8_t *p = buf + 16;
    const uint8_t *end = buf + len;
    for (uint32_t k = 0; k < count; k++) {
        if ((uint64_t)(end - p) < 10u) goto bad;
        TelsId id = get_u32(p);
        TelsId parent = get_u32(p + 4);
        uint8_t type = p[8];
        uint8_t nlen = p[9];
        p += 10;
        if (nlen == 0 || (uint64_t)(end - p) < (uint64_t)nlen + 12u) goto bad;  /* uint8_t: ≤ 255 */
        if (id == 0 || id >= next_id) goto bad;
        if (type != TELS_TYPE_FILE && type != TELS_TYPE_DIR) goto bad;
        if (parent != TELS_ROOT && parent >= id) goto bad;
        char name[TELS_MAX_NAME + 1];
        memcpy(name, p, nlen);
        name[nlen] = '\0';
        p += nlen;
        if (!name_valid(name)) goto bad;
        uint64_t size = get_u64(p);
        p += 8;
        uint32_t ec = get_u32(p);
        p += 4;
        if (ec > TELS_MAX_EXT_PER_ENTRY || (uint64_t)(end - p) < (uint64_t)ec * TELS_EXT_BYTES) goto bad;
        TelsEntry *e = entry_new(c, id, parent, type, name, nlen);
        if (!e) goto bad;
        e->size = size;
        if (ec > 0) {
            e->ext = (TelsExtent *)malloc(sizeof(TelsExtent) * ec);
            if (!e->ext) goto bad;
            e->ext_count = ec;
        }
        uint64_t prev_end = 0;
        for (uint32_t j = 0; j < ec; j++) {
            TelsExtent *x = &e->ext[j];
            x->off = get_u64(p);
            x->len = get_u32(p + 8);
            x->pos = get_u64(p + 12);
            p += TELS_EXT_BYTES;
            if (x->len == 0 || x->off < prev_end) goto bad;
            prev_end = x->off + x->len;
        }
    }
    if (p != end) goto bad;
    if (next_id > 1 && !entry_slot(c, next_id - 1)) goto bad;
    c->next_id = next_id;
    return TRUE;
bad:
    entries_clear(c);
    return FALSE;
}

/* index را از آفست داده شده می‌خواند و CRC و ساختار را بررسی می‌کند */
static BOOL load_index_at(TelsContainer *c, uint64_t pos, uint64_t *payload_end) {
    uint8_t hdr[TELS_REC_HDR];
    if (!read_at(c, pos, hdr, TELS_REC_HDR)) return FALSE;
    if (get_u32(hdr) != TELS_REC_MAGIC || hdr[8] != REC_INDEX) return FALSE;
    uint32_t plen = get_u32(hdr + 20);
    if (plen > TELS_MAX_INDEX) return FALSE;
    uint8_t *buf = (uint8_t *)malloc(plen ? plen : 1u);
    if (!buf) return FALSE;
    BOOL ok = (plen == 0) || read_at(c, pos + TELS_REC_HDR, buf, plen);
    if (ok) {
        uint32_t crc = crc32_feed(0, hdr, 32);
        if (plen) crc = crc32_feed(crc, buf, plen);
        ok = (crc == get_u32(hdr + 32));
    }
    if (ok) ok = parse_index(c, buf, plen);
    free(buf);
    if (ok && payload_end) *payload_end = pos + TELS_REC_HDR + plen;
    return ok;
}

/* ───── اعمال رکوردها (هنگام replay) ───── */

static BOOL apply_create(TelsContainer *c, TelsId id, const uint8_t *p, uint32_t plen) {
    if (plen < 6u) return FALSE;
    TelsId parent = get_u32(p);
    uint8_t type = p[4];
    uint8_t nlen = p[5];
    if (nlen == 0 || plen != 6u + nlen) return FALSE;  /* uint8_t: ≤ 255 */
    if (type != TELS_TYPE_FILE && type != TELS_TYPE_DIR) return FALSE;
    if (id == 0 || id >= TELS_MAX_ENTRIES) return FALSE;
    if (id < c->entry_cap && c->entries[id]) return FALSE;
    char name[TELS_MAX_NAME + 1];
    memcpy(name, p + 6, nlen);
    name[nlen] = '\0';
    if (!name_valid(name)) return FALSE;
    if (parent != TELS_ROOT) {
        TelsEntry *pe = entry_get(c, parent);
        if (!pe || pe->type != TELS_TYPE_DIR) return FALSE;
    }
    if (find_child(c, parent, name)) return FALSE;
    if (entry_depth(c, parent) + 1 > (int)TELS_MAX_DEPTH) return FALSE;
    return entry_new(c, id, parent, type, name, nlen) != NULL;
}

static BOOL apply_data(TelsContainer *c, TelsId id, uint64_t off, uint64_t pos, uint32_t len) {
    TelsEntry *e = entry_get(c, id);
    if (!e || e->type != TELS_TYPE_FILE || len == 0) return FALSE;
    if (off + len < off) return FALSE;
    return entry_add_extent(e, off, len, pos);
}

/* رکوردهای معتبر از pos تا اولین رکورد خراب را اعمال می‌کند؛ valid_end انتهای آخرین رکورد سالم است */
static BOOL replay(TelsContainer *c, uint64_t start, uint64_t *valid_end) {
    uint64_t fsize = file_size(c);
    uint64_t pos = start;
    uint8_t *chunk = (uint8_t *)malloc(TELS_CHUNK);
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;
    if (!chunk) return FALSE;

    while (pos + TELS_REC_HDR <= fsize) {
        uint8_t hdr[TELS_REC_HDR];
        if (!read_at(c, pos, hdr, TELS_REC_HDR)) break;
        if (get_u32(hdr) != TELS_REC_MAGIC) break;
        uint8_t kind = hdr[8];
        TelsId id = get_u32(hdr + 4);
        uint64_t off = get_u64(hdr + 12);
        uint32_t plen = get_u32(hdr + 20);
        uint64_t limit = (kind == REC_INDEX) ? TELS_MAX_INDEX : TELS_MAX_RECORD;
        if ((uint64_t)plen > limit) break;
        if (pos + TELS_REC_HDR + plen > fsize) break;          /* رکورد نیمه‌نوشته */

        uint32_t crc = crc32_feed(0, hdr, 32);
        BOOL ok = TRUE;
        if (kind == REC_DATA) {
            uint64_t done = 0;
            while (ok && done < plen) {
                uint32_t n = (uint32_t)min64(TELS_CHUNK, plen - done);
                ok = read_at(c, pos + TELS_REC_HDR + done, chunk, n);
                if (ok) crc = crc32_feed(crc, chunk, n);
                done += n;
            }
        } else if (plen > 0) {
            if (plen > payload_cap) {
                free(payload);
                payload = (uint8_t *)malloc(plen);
                payload_cap = payload ? plen : 0;
                if (!payload) ok = FALSE;
            }
            if (ok) {
                ok = read_at(c, pos + TELS_REC_HDR, payload, plen);
                if (ok) crc = crc32_feed(crc, payload, plen);
            }
        }
        if (!ok || crc != get_u32(hdr + 32)) break;

        BOOL applied = FALSE;
        switch (kind) {
        case REC_DATA:
            applied = apply_data(c, id, off, pos + TELS_REC_HDR, plen);
            break;
        case REC_CREATE:
            applied = (off == 0) && apply_create(c, id, payload, plen);
            break;
        case REC_DELETE:
            if (plen == 0 && id != TELS_ROOT && entry_get(c, id)) {
                entry_delete_subtree(c, id);
                applied = TRUE;
            }
            break;
        case REC_INDEX:
            applied = (id == 0 && off == 0) && parse_index(c, payload, plen);
            break;
        default:
            applied = FALSE;
        }
        if (!applied) break;
        pos += TELS_REC_HDR + plen;
    }
    free(chunk);
    free(payload);
    *valid_end = pos;
    return TRUE;
}

/* ───── superblock ───── */

static BOOL write_superblock(TelsContainer *c, uint64_t gen, uint64_t index_pos,
                             uint64_t data_end, uint64_t slot) {
    uint8_t sb[TELS_SB_SIZE];
    memset(sb, 0, sizeof(sb));
    memcpy(sb, "LASSTELS", 8);
    put_u32(sb + 8, TELS_VERSION);
    put_u64(sb + 12, gen);
    put_u64(sb + 20, index_pos);
    put_u64(sb + 28, data_end);
    put_u32(sb + 40, crc32_feed(0, sb, 40));
    return write_at(c, slot, sb, TELS_SB_SIZE);
}

static BOOL read_superblock(TelsContainer *c, uint64_t slot,
                            uint64_t *gen, uint64_t *index_pos, uint64_t *data_end) {
    uint8_t sb[44];
    if (!read_at(c, slot, sb, sizeof(sb))) return FALSE;
    if (memcmp(sb, "LASSTELS", 8) != 0) return FALSE;
    if (get_u32(sb + 8) != TELS_VERSION) return FALSE;
    if (crc32_feed(0, sb, 40) != get_u32(sb + 40)) return FALSE;
    *gen       = get_u64(sb + 12);
    *index_pos = get_u64(sb + 20);
    *data_end  = get_u64(sb + 28);
    return TRUE;
}

/* ───── باز و بسته کردن ───── */

static void container_release(TelsContainer *c) {
    if (!c) return;
    entries_clear(c);
    free(c->entries);
    if (c->file != INVALID_HANDLE_VALUE) CloseHandle(c->file);
    DeleteCriticalSection(&c->lock);
    free(c);
}

TelsContainer *tels_open(const char *path, BOOL create) {
    if (!path || !path[0]) return NULL;
    crc_init();
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    TelsContainer *c = (TelsContainer *)calloc(1, sizeof(*c));
    if (!c) {
        CloseHandle(h);
        return NULL;
    }
    c->file = h;
    c->writable = TRUE;
    c->next_id = 1;
    InitializeCriticalSection(&c->lock);

    uint64_t fsize = file_size(c);
    if (fsize == 0) {
        /* کانتینر تازه: superblock A با نسل ۱ و بدون index؛ B خالی */
        uint8_t zero[TELS_SB_SIZE];
        memset(zero, 0, sizeof(zero));
        BOOL made = write_superblock(c, 1, 0, TELS_DATA_START, TELS_SB_SLOT_A) &&
                    write_at(c, TELS_SB_SLOT_B, zero, TELS_SB_SIZE) &&
                    FlushFileBuffers(c->file);
        if (!made) {
            container_release(c);
            return NULL;
        }
        c->gen = 1;
        c->append_pos = TELS_DATA_START;
        return c;
    }

    uint64_t ga = 0, ia = 0, ea = 0, gb = 0, ib = 0, eb = 0;
    BOOL va = read_superblock(c, TELS_SB_SLOT_A, &ga, &ia, &ea);
    BOOL vb = read_superblock(c, TELS_SB_SLOT_B, &gb, &ib, &eb);
    uint64_t best_index = 0;
    if (va && (!vb || ga >= gb)) {
        c->gen = ga;
        best_index = ia;
    } else if (vb) {
        c->gen = gb;
        best_index = ib;
    }

    uint64_t start = TELS_DATA_START;
    if (best_index != 0) {
        uint64_t idx_end = 0;
        if (load_index_at(c, best_index, &idx_end)) start = idx_end;
        else entries_clear(c);
    }
    uint64_t valid_end = start;
    if (!replay(c, start, &valid_end)) {
        container_release(c);
        return NULL;
    }
    c->append_pos = valid_end;
    if (fsize > valid_end) {
        /* رکورد نیمه‌نوشته‌ی انتهای فایل حذف می‌شود تا نوشتن بعدی از همان‌جا ادامه یابد */
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)valid_end;
        if (SetFilePointerEx(c->file, li, NULL, FILE_BEGIN)) SetEndOfFile(c->file);
    }
    return c;
}

BOOL tels_commit(TelsContainer *c) {
    if (!c || !c->writable) return FALSE;
    EnterCriticalSection(&c->lock);
    BOOL ok = FALSE;
    uint8_t *buf = NULL;
    uint32_t len = 0;
    if (serialize_index(c, &buf, &len)) {
        uint64_t rec_pos = 0;
        if (append_record(c, REC_INDEX, 0, 0, buf, len, &rec_pos, NULL) &&
            FlushFileBuffers(c->file)) {
            uint64_t next_gen = c->gen + 1;
            uint64_t slot = (next_gen & 1u) ? TELS_SB_SLOT_A : TELS_SB_SLOT_B;
            if (write_superblock(c, next_gen, rec_pos, c->append_pos, slot) &&
                FlushFileBuffers(c->file)) {
                c->gen = next_gen;
                ok = TRUE;
            }
        }
    }
    free(buf);
    LeaveCriticalSection(&c->lock);
    return ok;
}

void tels_close(TelsContainer *c) {
    if (!c) return;
    if (c->writable) tels_commit(c);
    container_release(c);
}

void tels_abort(TelsContainer *c) {
    container_release(c);
}

/* ───── API عمومی ───── */

BOOL tels_create(TelsContainer *c, TelsId parent, const char *name, TelsType type, TelsId *out) {
    if (!c || !c->writable || !name_valid(name)) return FALSE;
    if (type != TELS_TYPE_FILE && type != TELS_TYPE_DIR) return FALSE;
    size_t nl = strlen(name);
    BOOL ok = FALSE;
    EnterCriticalSection(&c->lock);
    if (parent != TELS_ROOT) {
        TelsEntry *pe = entry_get(c, parent);
        if (!pe || pe->type != TELS_TYPE_DIR) goto done;
    }
    if (find_child(c, parent, name)) goto done;
    if (entry_depth(c, parent) + 1 > (int)TELS_MAX_DEPTH) goto done;
    if (c->next_id >= TELS_MAX_ENTRIES) goto done;
    {
        TelsId id = c->next_id;
        uint8_t payload[6 + TELS_MAX_NAME];
        put_u32(payload, parent);
        payload[4] = (uint8_t)type;
        payload[5] = (uint8_t)nl;
        memcpy(payload + 6, name, nl);
        if (!append_record(c, REC_CREATE, id, 0, payload, (uint32_t)(6 + nl), NULL, NULL)) goto done;
        if (!entry_new(c, id, parent, (uint8_t)type, name, (uint8_t)nl)) goto done;
        if (out) *out = id;
        ok = TRUE;
    }
done:
    LeaveCriticalSection(&c->lock);
    return ok;
}

BOOL tels_lookup(TelsContainer *c, TelsId parent, const char *name, TelsId *out) {
    if (!c || !name) return FALSE;
    EnterCriticalSection(&c->lock);
    TelsEntry *e = find_child(c, parent, name);
    if (e && out) *out = e->id;
    LeaveCriticalSection(&c->lock);
    return e != NULL;
}

BOOL tels_write(TelsContainer *c, TelsId id, uint64_t offset, const void *buf, uint32_t len) {
    if (!c || !c->writable || !buf) return FALSE;
    if (len == 0) return TRUE;
    if (len > TELS_MAX_RECORD) return FALSE;
    if (offset + len < offset) return FALSE;
    BOOL ok = FALSE;
    EnterCriticalSection(&c->lock);
    TelsEntry *e = entry_get(c, id);
    if (e && e->type == TELS_TYPE_FILE) {
        uint64_t payload_pos = 0;
        if (append_record(c, REC_DATA, id, offset, buf, len, NULL, &payload_pos))
            ok = entry_add_extent(e, offset, len, payload_pos);
    }
    LeaveCriticalSection(&c->lock);
    return ok;
}

int32_t tels_read(TelsContainer *c, TelsId id, uint64_t offset, void *buf, uint32_t len) {
    if (!c || !buf) return -1;
    if (len == 0) return 0;
    int32_t result = -1;
    EnterCriticalSection(&c->lock);
    TelsEntry *e = entry_get(c, id);
    if (e && e->type == TELS_TYPE_FILE) {
        if (offset >= e->size) {
            result = 0;
        } else {
            uint64_t avail = e->size - offset;
            uint32_t want = (uint32_t)min64((uint64_t)len, avail);
            uint8_t *out = (uint8_t *)buf;
            memset(out, 0, want);
            result = (int32_t)want;
            uint64_t rs = offset, re = offset + want;
            for (uint32_t i = 0; i < e->ext_count && result > 0; i++) {
                TelsExtent *x = &e->ext[i];
                uint64_t xs = x->off, xe = x->off + x->len;
                uint64_t s = xs > rs ? xs : rs;
                uint64_t t = xe < re ? xe : re;
                if (s >= t) continue;
                uint32_t n = (uint32_t)(t - s);
                if (!read_at(c, x->pos + (s - xs), out + (s - rs), n)) result = -1;
            }
        }
    }
    LeaveCriticalSection(&c->lock);
    return result;
}

BOOL tels_size(TelsContainer *c, TelsId id, uint64_t *size_out) {
    if (!c || !size_out) return FALSE;
    EnterCriticalSection(&c->lock);
    TelsEntry *e = entry_get(c, id);
    BOOL ok = (e != NULL);
    if (ok) *size_out = e->size;
    LeaveCriticalSection(&c->lock);
    return ok;
}

BOOL tels_delete(TelsContainer *c, TelsId id) {
    if (!c || !c->writable || id == TELS_ROOT) return FALSE;
    BOOL ok = FALSE;
    EnterCriticalSection(&c->lock);
    if (entry_get(c, id) && append_record(c, REC_DELETE, id, 0, NULL, 0, NULL, NULL)) {
        entry_delete_subtree(c, id);
        ok = TRUE;
    }
    LeaveCriticalSection(&c->lock);
    return ok;
}

uint32_t tels_list(TelsContainer *c, TelsId parent, TelsId *out, uint32_t cap) {
    if (!c) return 0;
    uint32_t n = 0;
    EnterCriticalSection(&c->lock);
    for (TelsId i = 1; i < c->next_id; i++) {
        TelsEntry *e = c->entries[i];
        if (!e || e->parent != parent) continue;
        if (out && n < cap) out[n] = e->id;
        n++;
    }
    LeaveCriticalSection(&c->lock);
    return n;
}

BOOL tels_info(TelsContainer *c, TelsId id, TelsType *type, TelsId *parent,
               char *name, size_t name_sz) {
    if (!c) return FALSE;
    EnterCriticalSection(&c->lock);
    TelsEntry *e = entry_get(c, id);
    BOOL ok = (e != NULL);
    if (ok) {
        if (type) *type = (TelsType)e->type;
        if (parent) *parent = e->parent;
        if (name && name_sz) {
            size_t n = e->name_len < name_sz - 1 ? e->name_len : name_sz - 1;
            memcpy(name, e->name, n);
            name[n] = '\0';
        }
    }
    LeaveCriticalSection(&c->lock);
    return ok;
}

BOOL tels_resolve(TelsContainer *c, const char *path, BOOL create_dirs, TelsId *out) {
    if (!c || !path) return FALSE;
    TelsId cur = TELS_ROOT;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != '/' && p[n] != '\\') n++;
        if (n > TELS_MAX_NAME) return FALSE;
        char part[TELS_MAX_NAME + 1];
        memcpy(part, p, n);
        part[n] = '\0';
        p += n;
        TelsId next = 0;
        if (tels_lookup(c, cur, part, &next)) {
            cur = next;
            continue;
        }
        if (!create_dirs || !tels_create(c, cur, part, TELS_TYPE_DIR, &next)) return FALSE;
        cur = next;
    }
    if (out) *out = cur;
    return TRUE;
}
