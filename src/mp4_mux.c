/* mp4_mux.c — خواندن fMP4 و نوشتن MP4 معمولی؛ رجوع کن به mp4_mux.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "mp4_mux.h"

#define MP4_MAX_TREX          16u
#define MP4_MAX_TRUN_SAMPLES  1000000u

static void set_err(char *err, size_t sz, const char *fmt, ...) {
    va_list ap;
    if (!err || sz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, sz, fmt, ap);
    va_end(ap);
    err[sz - 1] = '\0';
}

/* ======================================================
   خواندن big-endian
   ====================================================== */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* ======================================================
   box ها
   ====================================================== */
typedef struct {
    uint64_t start;     /* آفست سرآیند در بافر */
    uint64_t size;      /* کل اندازه، شامل سرآیند */
    uint64_t hdr;       /* ۸ یا ۱۶ */
    char     type[5];
} Box;

/* box را در off می‌خواند؛ هر چیزی که از limit بیرون بزند ناموفق است */
static BOOL box_at(const uint8_t *buf, uint64_t limit, uint64_t off, Box *b) {
    uint64_t size, hdr = 8;
    if (off > limit || limit - off < 8) return FALSE;
    size = be32(buf + off);
    memcpy(b->type, buf + off + 4, 4);
    b->type[4] = '\0';
    if (size == 1) {
        if (limit - off < 16) return FALSE;
        size = be64(buf + off + 8);
        hdr = 16;
    } else if (size == 0) {
        size = limit - off;
    }
    if (size < hdr || size > limit - off) return FALSE;
    b->start = off;
    b->size  = size;
    b->hdr   = hdr;
    return TRUE;
}

static uint64_t payload_start(const Box *b) { return b->start + b->hdr; }
static uint64_t payload_end(const Box *b)   { return b->start + b->size; }

/* اولین فرزند با نوع داده شده در محدوده‌ی [pstart, pend) */
static BOOL find_box(const uint8_t *buf, uint64_t pstart, uint64_t pend,
                     const char *type, Box *out) {
    uint64_t off = pstart;
    while (off < pend) {
        Box b;
        if (!box_at(buf, pend, off, &b)) return FALSE;
        if (strcmp(b.type, type) == 0) {
            *out = b;
            return TRUE;
        }
        off += b.size;
    }
    return FALSE;
}

/* ======================================================
   تجزیه‌ی init
   ====================================================== */
static BOOL parse_tkhd(const uint8_t *buf, const Box *b,
                       uint32_t *track_id, uint32_t *w, uint32_t *h) {
    const uint8_t *p = buf + payload_start(b);
    uint64_t n = b->size - b->hdr;
    if (n < 4) return FALSE;
    if (p[0] == 0) {
        if (n < 84) return FALSE;
        *track_id = be32(p + 12);
        *w = be32(p + 76) >> 16;
        *h = be32(p + 80) >> 16;
    } else if (p[0] == 1) {
        if (n < 96) return FALSE;
        *track_id = be32(p + 20);
        *w = be32(p + 88) >> 16;
        *h = be32(p + 92) >> 16;
    } else {
        return FALSE;
    }
    return TRUE;
}

static BOOL parse_mdhd(const uint8_t *buf, const Box *b,
                       uint32_t *timescale, uint16_t *lang) {
    const uint8_t *p = buf + payload_start(b);
    uint64_t n = b->size - b->hdr;
    if (n < 4) return FALSE;
    if (p[0] == 0) {
        if (n < 24) return FALSE;
        *timescale = be32(p + 12);
        *lang      = be16(p + 20);
    } else if (p[0] == 1) {
        if (n < 36) return FALSE;
        *timescale = be32(p + 20);
        *lang      = be16(p + 32);
    } else {
        return FALSE;
    }
    return *timescale != 0;
}

static BOOL parse_trak(const uint8_t *buf, const Box *trak, Mp4TrackInfo *t,
                       char *err, size_t err_sz) {
    Box tk, ed, mdia, mdh, hdl, minf, mh, dinf, stbl, stsd;
    uint32_t w = 0, h = 0;
    uint64_t ps = payload_start(trak), pe = payload_end(trak);

    memset(t, 0, sizeof(*t));
    if (!find_box(buf, ps, pe, "tkhd", &tk) ||
        !parse_tkhd(buf, &tk, &t->track_id, &w, &h)) {
        set_err(err, err_sz, "track without a valid tkhd");
        return FALSE;
    }
    t->width  = w;
    t->height = h;

    if (find_box(buf, ps, pe, "edts", &ed)) {
        t->edts     = buf + ed.start;
        t->edts_len = (uint32_t)ed.size;
    }

    if (!find_box(buf, ps, pe, "mdia", &mdia)) {
        set_err(err, err_sz, "track %u has no mdia", t->track_id);
        return FALSE;
    }
    ps = payload_start(&mdia);
    pe = payload_end(&mdia);

    if (!find_box(buf, ps, pe, "mdhd", &mdh) ||
        !parse_mdhd(buf, &mdh, &t->timescale, &t->language)) {
        set_err(err, err_sz, "track %u has no valid mdhd", t->track_id);
        return FALSE;
    }
    if (!find_box(buf, ps, pe, "hdlr", &hdl) || hdl.size - hdl.hdr < 12) {
        set_err(err, err_sz, "track %u has no valid hdlr", t->track_id);
        return FALSE;
    }
    memcpy(t->handler, buf + payload_start(&hdl) + 8, 4);
    t->handler[4] = '\0';
    t->hdlr     = buf + hdl.start;
    t->hdlr_len = (uint32_t)hdl.size;

    if (!find_box(buf, ps, pe, "minf", &minf)) {
        set_err(err, err_sz, "track %u has no minf", t->track_id);
        return FALSE;
    }
    ps = payload_start(&minf);
    pe = payload_end(&minf);

    if (find_box(buf, ps, pe, "vmhd", &mh) || find_box(buf, ps, pe, "smhd", &mh)) {
        t->media_hdr     = buf + mh.start;
        t->media_hdr_len = (uint32_t)mh.size;
    } else {
        set_err(err, err_sz, "track %u has no vmhd or smhd", t->track_id);
        return FALSE;
    }
    if (find_box(buf, ps, pe, "dinf", &dinf)) {
        t->dinf     = buf + dinf.start;
        t->dinf_len = (uint32_t)dinf.size;
    }

    if (!find_box(buf, ps, pe, "stbl", &stbl)) {
        set_err(err, err_sz, "track %u has no stbl", t->track_id);
        return FALSE;
    }
    if (!find_box(buf, payload_start(&stbl), payload_end(&stbl), "stsd", &stsd) ||
        stsd.size - stsd.hdr < 8) {
        set_err(err, err_sz, "track %u has no valid stsd", t->track_id);
        return FALSE;
    }
    if (be32(buf + payload_start(&stsd) + 4) != 1) {
        set_err(err, err_sz, "track %u: stsd with more than one entry is not supported",
                t->track_id);
        return FALSE;
    }
    t->stsd     = buf + stsd.start;
    t->stsd_len = (uint32_t)stsd.size;
    return TRUE;
}

static uint32_t movie_ts_of(const uint8_t *buf, const Box *b) {
    const uint8_t *p = buf + payload_start(b);
    uint64_t n = b->size - b->hdr;
    if (n >= 24 && p[0] == 0) return be32(p + 12);
    if (n >= 36 && p[0] == 1) return be32(p + 20);
    return 0;
}

BOOL mp4_parse_init(const uint8_t *buf, uint64_t len, Mp4Init *out,
                    char *err, size_t err_sz) {
    Box moov = { 0, 0, 0, { 0 } };
    BOOL found = FALSE;
    uint64_t off = 0, ps, pe;
    uint32_t trex_ids[MP4_MAX_TREX];
    uint32_t trex_dur[MP4_MAX_TREX], trex_size[MP4_MAX_TREX], trex_flags[MP4_MAX_TREX];
    uint32_t trex_n = 0;
    int i, j;

    memset(out, 0, sizeof(*out));
    if (!buf || len < 8) {
        set_err(err, err_sz, "init segment is empty");
        return FALSE;
    }
    while (off < len) {
        Box b;
        if (!box_at(buf, len, off, &b)) break;
        if (strcmp(b.type, "moov") == 0) {
            moov = b;
            found = TRUE;
            break;
        }
        off += b.size;
    }
    if (!found) {
        set_err(err, err_sz, "init segment has no moov box");
        return FALSE;
    }

    ps = payload_start(&moov);
    pe = payload_end(&moov);
    off = ps;
    while (off < pe) {
        Box b;
        if (!box_at(buf, pe, off, &b)) {
            set_err(err, err_sz, "damaged box inside moov");
            return FALSE;
        }
        if (strcmp(b.type, "mvhd") == 0) {
            out->movie_timescale = movie_ts_of(buf, &b);
        } else if (strcmp(b.type, "trak") == 0) {
            Mp4TrackInfo *t;
            if (out->track_count >= MP4_MAX_TRACKS) {
                set_err(err, err_sz, "more than %d tracks are not supported", MP4_MAX_TRACKS);
                return FALSE;
            }
            t = &out->track[out->track_count];
            if (!parse_trak(buf, &b, t, err, err_sz)) return FALSE;
            for (i = 0; i < out->track_count; i++) {
                if (out->track[i].track_id == t->track_id) {
                    set_err(err, err_sz, "duplicate track id %u", t->track_id);
                    return FALSE;
                }
            }
            out->track_count++;
        } else if (strcmp(b.type, "mvex") == 0) {
            uint64_t xs = payload_start(&b), xe = payload_end(&b);
            uint64_t xo = xs;
            while (xo < xe) {
                Box tx;
                if (!box_at(buf, xe, xo, &tx)) break;
                if (strcmp(tx.type, "trex") == 0 && tx.size - tx.hdr >= 24 &&
                    trex_n < MP4_MAX_TREX) {
                    const uint8_t *p = buf + payload_start(&tx);
                    trex_ids[trex_n]   = be32(p + 4);
                    trex_dur[trex_n]   = be32(p + 12);
                    trex_size[trex_n]  = be32(p + 16);
                    trex_flags[trex_n] = be32(p + 20);
                    trex_n++;
                }
                xo += tx.size;
            }
        }
        off += b.size;
    }

    if (out->movie_timescale == 0) out->movie_timescale = 1000;
    if (out->track_count == 0) {
        set_err(err, err_sz, "init segment has no tracks");
        return FALSE;
    }
    for (i = 0; i < out->track_count; i++) {
        for (j = 0; j < (int)trex_n; j++) {
            if (trex_ids[j] == out->track[i].track_id) {
                out->track[i].trex_found    = TRUE;
                out->track[i].trex_duration = trex_dur[j];
                out->track[i].trex_size     = trex_size[j];
                out->track[i].trex_flags    = trex_flags[j];
            }
        }
    }
    return TRUE;
}

int mp4_find_track(const Mp4Init *init, uint32_t track_id) {
    int i;
    if (!init) return -1;
    for (i = 0; i < init->track_count; i++) {
        if (init->track[i].track_id == track_id) return i;
    }
    return -1;
}

/* ======================================================
   خواندن قطعات (moof / traf / trun)
   ====================================================== */
typedef struct {
    uint32_t track_id;
    BOOL     has_base;    uint64_t base;
    BOOL     has_desc;    uint32_t desc;
    BOOL     has_dur;     uint32_t dur;
    BOOL     has_size;    uint32_t size;
    BOOL     has_flags;   uint32_t flags;
    BOOL     base_is_moof;
} TfHdr;

static BOOL parse_tfhd(const uint8_t *buf, const Box *b, TfHdr *h) {
    const uint8_t *p = buf + payload_start(b);
    uint64_t n = b->size - b->hdr;
    uint32_t fl;
    uint64_t pos = 8;

    memset(h, 0, sizeof(*h));
    if (n < 8) return FALSE;
    fl = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    h->track_id = be32(p + 4);
    if (fl & 0x000001) {
        if (n < pos + 8) return FALSE;
        h->has_base = TRUE;
        h->base = be64(p + pos);
        pos += 8;
    }
    if (fl & 0x000002) {
        if (n < pos + 4) return FALSE;
        h->has_desc = TRUE;
        h->desc = be32(p + pos);
        pos += 4;
    }
    if (fl & 0x000008) {
        if (n < pos + 4) return FALSE;
        h->has_dur = TRUE;
        h->dur = be32(p + pos);
        pos += 4;
    }
    if (fl & 0x000010) {
        if (n < pos + 4) return FALSE;
        h->has_size = TRUE;
        h->size = be32(p + pos);
        pos += 4;
    }
    if (fl & 0x000020) {
        if (n < pos + 4) return FALSE;
        h->has_flags = TRUE;
        h->flags = be32(p + pos);
        pos += 4;
    }
    h->base_is_moof = (fl & 0x020000) != 0;
    return TRUE;
}

/* یک trun را می‌خواند و برای هر نمونه fn را صدا می‌زند؛ next آفست داده‌ی بعدی را نگه می‌دارد */
static BOOL walk_trun(const uint8_t *seg, uint64_t seglen, const Box *tr,
                      const Mp4TrackInfo *ti, const TfHdr *th, uint64_t base,
                      uint64_t *next, Mp4SampleFn fn, void *ctx,
                      char *err, size_t err_sz) {
    const uint8_t *p = seg + payload_start(tr);
    uint64_t n = tr->size - tr->hdr;
    uint32_t fl, count, first_flags = 0;
    uint64_t pos = 8, data_pos;
    int32_t doff = 0;
    uint32_t i;
    uint64_t per = 0;

    if (n < 8) {
        set_err(err, err_sz, "damaged trun box");
        return FALSE;
    }
    fl    = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    count = be32(p + 4);
    if (count > MP4_MAX_TRUN_SAMPLES) {
        set_err(err, err_sz, "trun with %u samples is not supported", count);
        return FALSE;
    }
    if (fl & 0x000001) {
        if (n < pos + 4) goto damaged;
        doff = (int32_t)be32(p + pos);
        pos += 4;
    }
    if (fl & 0x000004) {
        if (n < pos + 4) goto damaged;
        first_flags = be32(p + pos);
        pos += 4;
    }
    if (fl & 0x000100) per += 4;
    if (fl & 0x000200) per += 4;
    if (fl & 0x000400) per += 4;
    if (fl & 0x000800) per += 4;
    if (per > 0 && (uint64_t)count > (n - pos) / per) goto damaged;

    if (fl & 0x000001) {
        int64_t d = (int64_t)base + (int64_t)doff;
        if (d < 0) {
            set_err(err, err_sz, "trun data offset is before the segment start");
            return FALSE;
        }
        data_pos = (uint64_t)d;
    } else {
        data_pos = *next;
    }

    for (i = 0; i < count; i++) {
        Mp4FragSample s;
        uint32_t dur, sz, sf, per_flags = 0;
        int32_t cto = 0;

        if (fl & 0x000100) { dur = be32(p + pos); pos += 4; }
        else if (th->has_dur) dur = th->dur;
        else dur = ti->trex_duration;

        if (fl & 0x000200) { sz = be32(p + pos); pos += 4; }
        else if (th->has_size) sz = th->size;
        else sz = ti->trex_size;

        if (fl & 0x000400) { per_flags = be32(p + pos); pos += 4; }
        if (i == 0 && (fl & 0x000004)) sf = first_flags;
        else if (fl & 0x000400)        sf = per_flags;
        else if (th->has_flags)        sf = th->flags;
        else                           sf = ti->trex_flags;

        if (fl & 0x000800) { cto = (int32_t)be32(p + pos); pos += 4; }

        if (data_pos > seglen || (uint64_t)sz > seglen - data_pos) {
            set_err(err, err_sz, "sample %u of track %u lies outside its segment", i, ti->track_id);
            return FALSE;
        }
        s.track_id = ti->track_id;
        s.offset   = data_pos;
        s.size     = sz;
        s.duration = dur;
        s.cto      = cto;
        s.sync     = ((sf & 0x00010000u) == 0);
        if (!fn(&s, ctx)) {
            set_err(err, err_sz, "processing of track %u stopped", ti->track_id);
            return FALSE;
        }
        data_pos += sz;
    }
    *next = data_pos;
    return TRUE;

damaged:
    set_err(err, err_sz, "damaged trun box");
    return FALSE;
}

static BOOL walk_traf(const uint8_t *seg, uint64_t seglen, const Box *traf,
                      uint64_t moof_start, BOOL first_traf, uint64_t *prev_end,
                      const Mp4Init *init, Mp4SampleFn fn, void *ctx,
                      char *err, size_t err_sz) {
    Box tfh, b;
    TfHdr th;
    int ti;
    uint64_t base, next, off, pe;

    if (!find_box(seg, payload_start(traf), payload_end(traf), "tfhd", &tfh) ||
        !parse_tfhd(seg, &tfh, &th)) {
        set_err(err, err_sz, "traf without a valid tfhd");
        return FALSE;
    }
    if (th.has_desc && th.desc != 1) {
        set_err(err, err_sz, "sample description index %u is not supported", th.desc);
        return FALSE;
    }
    ti = mp4_find_track(init, th.track_id);
    if (ti < 0) {
        set_err(err, err_sz, "fragment uses unknown track %u", th.track_id);
        return FALSE;
    }

    if (th.has_base)            base = th.base;
    else if (th.base_is_moof)   base = moof_start;
    else if (first_traf)        base = moof_start;
    else                        base = *prev_end;
    next = base;

    off = payload_start(traf);
    pe  = payload_end(traf);
    while (off < pe) {
        if (!box_at(seg, pe, off, &b)) {
            set_err(err, err_sz, "damaged box inside traf");
            return FALSE;
        }
        if (strcmp(b.type, "trun") == 0) {
            if (!walk_trun(seg, seglen, &b, &init->track[ti], &th, base, &next,
                           fn, ctx, err, err_sz))
                return FALSE;
        }
        off += b.size;
    }
    *prev_end = next;
    return TRUE;
}

static BOOL walk_moof(const uint8_t *seg, uint64_t seglen, const Box *moof,
                      const Mp4Init *init, Mp4SampleFn fn, void *ctx,
                      char *err, size_t err_sz) {
    uint64_t off = payload_start(moof), pe = payload_end(moof);
    uint64_t prev_end = 0;
    BOOL first = TRUE;

    while (off < pe) {
        Box b;
        if (!box_at(seg, pe, off, &b)) {
            set_err(err, err_sz, "damaged box inside moof");
            return FALSE;
        }
        if (strcmp(b.type, "traf") == 0) {
            if (!walk_traf(seg, seglen, &b, moof->start, first, &prev_end,
                           init, fn, ctx, err, err_sz))
                return FALSE;
            first = FALSE;
        }
        off += b.size;
    }
    return TRUE;
}

BOOL mp4_walk_fragments(const uint8_t *seg, uint64_t len, const Mp4Init *init,
                        Mp4SampleFn fn, void *ctx, char *err, size_t err_sz) {
    uint64_t off = 0;
    if (!seg || !init || !fn) {
        set_err(err, err_sz, "invalid arguments");
        return FALSE;
    }
    while (off < len) {
        Box b;
        if (!box_at(seg, len, off, &b)) {
            set_err(err, err_sz, "damaged box at offset %llu", (unsigned long long)off);
            return FALSE;
        }
        if (strcmp(b.type, "moof") == 0) {
            if (!walk_moof(seg, len, &b, init, fn, ctx, err, err_sz)) return FALSE;
        }
        off += b.size;
    }
    return TRUE;
}

/* ======================================================
   نوشتن
   ====================================================== */
typedef struct {
    uint8_t *buf;      /* NULL یعنی فقط شمارش */
    uint64_t cap;
    uint64_t pos;
    BOOL     overflow; /* اندازه‌ی یک box از ۳۲ بیت بیشتر شد، یا بافر کوچک بود */
} Mw;

static void mw_put(Mw *w, const void *src, uint64_t n) {
    if (n == 0) return;
    if (w->buf) {
        if (w->pos + n <= w->cap) memcpy(w->buf + w->pos, src, (size_t)n);
        else w->overflow = TRUE;
    }
    w->pos += n;
}

static void mw_u16(Mw *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    mw_put(w, b, 2);
}

static void mw_u32(Mw *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    mw_put(w, b, 4);
}

static void mw_u64(Mw *w, uint64_t v) {
    mw_u32(w, (uint32_t)(v >> 32));
    mw_u32(w, (uint32_t)v);
}

static void mw_type(Mw *w, const char *t) { mw_put(w, t, 4); }

static void mw_patch32(Mw *w, uint64_t at, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    if (w->buf && at + 4 <= w->cap) memcpy(w->buf + at, b, 4);
}

/* box را باز می‌کند؛ اندازه‌ی آن بعداً با mw_close پر می‌شود */
static uint64_t mw_open(Mw *w, const char *type) {
    uint64_t at = w->pos;
    mw_u32(w, 0);
    mw_type(w, type);
    return at;
}

static void mw_close(Mw *w, uint64_t at) {
    uint64_t size = w->pos - at;
    if (size > 0xFFFFFFFFu) {
        w->overflow = TRUE;
        return;
    }
    mw_patch32(w, at, (uint32_t)size);
}

static void write_identity_matrix(Mw *w) {
    mw_u32(w, 0x00010000); mw_u32(w, 0); mw_u32(w, 0);
    mw_u32(w, 0); mw_u32(w, 0x00010000); mw_u32(w, 0);
    mw_u32(w, 0); mw_u32(w, 0); mw_u32(w, 0x40000000);
}

static void write_tkhd(Mw *w, const Mp4OutTrack *t, uint64_t movie_dur) {
    uint64_t at = mw_open(w, "tkhd");
    mw_u32(w, 0x00000003);          /* version 0؛ enabled + in movie */
    mw_u32(w, 0);
    mw_u32(w, 0);
    mw_u32(w, t->track_id);
    mw_u32(w, 0);
    mw_u32(w, (uint32_t)movie_dur);
    mw_u32(w, 0);
    mw_u32(w, 0);
    mw_u16(w, 0);                   /* layer */
    mw_u16(w, 0);                   /* alternate group */
    mw_u16(w, t->is_audio ? 0x0100 : 0);
    mw_u16(w, 0);
    write_identity_matrix(w);
    mw_u32(w, t->width << 16);
    mw_u32(w, t->height << 16);
    mw_close(w, at);
}

static void write_mdhd(Mw *w, const Mp4OutTrack *t, uint64_t dur) {
    uint64_t at = mw_open(w, "mdhd");
    mw_u32(w, 0);
    mw_u32(w, 0);
    mw_u32(w, 0);
    mw_u32(w, t->timescale);
    mw_u32(w, (uint32_t)dur);
    mw_u16(w, t->language);
    mw_u16(w, 0);
    mw_close(w, at);
}

static void write_stts(Mw *w, const uint32_t *dur, uint32_t n) {
    uint64_t at = mw_open(w, "stts");
    uint64_t ec;
    uint32_t entries = 0, i = 0;
    mw_u32(w, 0);
    ec = w->pos;
    mw_u32(w, 0);
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && dur[j] == dur[i]) j++;
        mw_u32(w, j - i);
        mw_u32(w, dur[i]);
        entries++;
        i = j;
    }
    mw_patch32(w, ec, entries);
    mw_close(w, at);
}

static void write_ctts(Mw *w, const int32_t *cto, uint32_t n) {
    BOOL neg = FALSE;
    uint32_t i = 0, entries = 0;
    uint64_t at, ec;
    for (i = 0; i < n; i++) if (cto[i] < 0) neg = TRUE;
    at = mw_open(w, "ctts");
    mw_u32(w, neg ? 0x01000000u : 0);   /* version 1 برای مقدار منفی */
    ec = w->pos;
    mw_u32(w, 0);
    i = 0;
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && cto[j] == cto[i]) j++;
        mw_u32(w, j - i);
        mw_u32(w, (uint32_t)cto[i]);
        entries++;
        i = j;
    }
    mw_patch32(w, ec, entries);
    mw_close(w, at);
}

static void write_stss(Mw *w, const uint8_t *sync, uint32_t n) {
    uint32_t c = 0, i;
    uint64_t at;
    for (i = 0; i < n; i++) if (sync[i]) c++;
    at = mw_open(w, "stss");
    mw_u32(w, 0);
    mw_u32(w, c);
    for (i = 0; i < n; i++) if (sync[i]) mw_u32(w, i + 1);
    mw_close(w, at);
}

static void write_stsc(Mw *w, uint32_t n) {
    uint64_t at = mw_open(w, "stsc");
    mw_u32(w, 0);
    mw_u32(w, 1);      /* یک entry */
    mw_u32(w, 1);      /* first_chunk */
    mw_u32(w, n);      /* samples_per_chunk: همه در یک chunk */
    mw_u32(w, 1);      /* sample_description_index */
    mw_close(w, at);
}

static void write_stsz(Mw *w, const uint32_t *sizes, uint32_t n) {
    uint32_t i;
    uint64_t at = mw_open(w, "stsz");
    mw_u32(w, 0);
    mw_u32(w, 0);      /* sample_size = ۰ یعنی جدول کامل */
    mw_u32(w, n);
    for (i = 0; i < n; i++) mw_u32(w, sizes[i]);
    mw_close(w, at);
}

static void write_stco(Mw *w, uint64_t off, BOOL use64) {
    uint64_t at;
    if (use64) {
        at = mw_open(w, "co64");
        mw_u32(w, 0);
        mw_u32(w, 1);
        mw_u64(w, off);
    } else {
        at = mw_open(w, "stco");
        mw_u32(w, 0);
        mw_u32(w, 1);
        mw_u32(w, (uint32_t)off);
    }
    mw_close(w, at);
}

/* dinf پیش‌فرض، وقتی init آن را نداشته باشد: dref با یک url ‌ داخلی */
static void write_default_dinf(Mw *w) {
    uint64_t at = mw_open(w, "dinf");
    uint64_t dr = mw_open(w, "dref");
    uint64_t ur;
    mw_u32(w, 0);
    mw_u32(w, 1);
    ur = mw_open(w, "url ");
    mw_u32(w, 0x00000001);
    mw_close(w, ur);
    mw_close(w, dr);
    mw_close(w, at);
}

/* edts را کپی می‌کند؛ مدت هر ورودی elst از مقیاس mvhd منبع به ۱۰۰۰ تبدیل می‌شود */
static void write_edts(Mw *w, const Mp4OutTrack *t) {
    Box eb, el;
    const uint8_t *p;
    uint64_t n, at, ea, esz;
    uint32_t ver, cnt, k, src_ts;

    if (!t->edts || t->edts_len == 0) return;
    if (!box_at(t->edts, t->edts_len, 0, &eb) ||
        !find_box(t->edts, payload_start(&eb), payload_end(&eb), "elst", &el)) {
        mw_put(w, t->edts, t->edts_len);
        return;
    }
    p = t->edts + payload_start(&el);
    n = el.size - el.hdr;
    if (n < 8) {
        mw_put(w, t->edts, t->edts_len);
        return;
    }
    ver = p[0];
    cnt = be32(p + 4);
    esz = (ver == 1) ? 20u : 12u;
    if (ver > 1 || n < 8 + (uint64_t)cnt * esz) {
        mw_put(w, t->edts, t->edts_len);
        return;
    }
    src_ts = t->src_movie_ts ? t->src_movie_ts : 1000u;

    at = mw_open(w, "edts");
    ea = mw_open(w, "elst");
    mw_u32(w, ver << 24);
    mw_u32(w, cnt);
    for (k = 0; k < cnt; k++) {
        const uint8_t *e = p + 8 + (uint64_t)k * esz;
        if (ver == 1) {
            uint64_t seg = be64(e);
            mw_u64(w, (seg * 1000u + src_ts / 2u) / src_ts);
            mw_put(w, e + 8, 8);
            mw_put(w, e + 16, 4);
        } else {
            uint64_t seg = be32(e);
            mw_u32(w, (uint32_t)((seg * 1000u + src_ts / 2u) / src_ts));
            mw_put(w, e + 4, 4);
            mw_put(w, e + 8, 4);
        }
    }
    mw_close(w, ea);
    mw_close(w, at);
}

static void write_trak(Mw *w, const Mp4OutTrack *t, uint64_t chunk_off,
                       BOOL use64, uint64_t movie_ts) {
    uint64_t dur = 0, movie_dur, trak, mdia, minf, stbl;
    uint32_t i;
    BOOL has_ctts = FALSE, has_nonsync = FALSE;

    for (i = 0; i < t->sample_count; i++) dur += t->durations[i];
    movie_dur = t->timescale ? (dur * movie_ts) / t->timescale : 0;
    if (t->ctos) for (i = 0; i < t->sample_count; i++) if (t->ctos[i] != 0) has_ctts = TRUE;
    if (t->sync) for (i = 0; i < t->sample_count; i++) if (!t->sync[i]) has_nonsync = TRUE;

    trak = mw_open(w, "trak");
    write_tkhd(w, t, movie_dur);
    write_edts(w, t);
    mdia = mw_open(w, "mdia");
    write_mdhd(w, t, dur);
    mw_put(w, t->hdlr, t->hdlr_len);
    minf = mw_open(w, "minf");
    mw_put(w, t->media_hdr, t->media_hdr_len);
    if (t->dinf_len) mw_put(w, t->dinf, t->dinf_len);
    else             write_default_dinf(w);
    stbl = mw_open(w, "stbl");
    mw_put(w, t->stsd, t->stsd_len);
    write_stts(w, t->durations, t->sample_count);
    if (has_ctts) write_ctts(w, t->ctos, t->sample_count);
    if (has_nonsync) write_stss(w, t->sync, t->sample_count);
    write_stsc(w, t->sample_count);
    write_stsz(w, t->sizes, t->sample_count);
    write_stco(w, chunk_off, use64);
    mw_close(w, stbl);
    mw_close(w, minf);
    mw_close(w, mdia);
    mw_close(w, trak);
}

uint64_t mp4_write_ftyp(uint8_t *buf, uint64_t cap) {
    Mw w;
    uint64_t at;
    memset(&w, 0, sizeof(w));
    w.buf = buf;
    w.cap = cap;
    at = mw_open(&w, "ftyp");
    mw_type(&w, "isom");
    mw_u32(&w, 512);
    mw_type(&w, "isom");
    mw_type(&w, "iso2");
    mw_type(&w, "mp41");
    mw_close(&w, at);
    return w.pos;
}

uint64_t mp4_write_moov(const Mp4OutTrack *tracks, int n, uint64_t data_start,
                        BOOL use64, uint8_t *buf, uint64_t cap) {
    Mw w;
    uint64_t at, mvdur = 0, chunk = data_start;
    int i;

    memset(&w, 0, sizeof(w));
    w.buf = buf;
    w.cap = cap;
    for (i = 0; i < n; i++) {
        uint64_t dur = 0;
        uint32_t k;
        for (k = 0; k < tracks[i].sample_count; k++) dur += tracks[i].durations[k];
        if (tracks[i].timescale) {
            uint64_t m = (dur * 1000u) / tracks[i].timescale;
            if (m > mvdur) mvdur = m;
        }
    }

    at = mw_open(&w, "moov");
    {
        uint64_t ma = mw_open(&w, "mvhd");
        mw_u32(&w, 0);
        mw_u32(&w, 0);
        mw_u32(&w, 0);
        mw_u32(&w, 1000);
        mw_u32(&w, (uint32_t)mvdur);
        mw_u32(&w, 0x00010000);
        mw_u16(&w, 0x0100);
        mw_u16(&w, 0);
        mw_u32(&w, 0);
        mw_u32(&w, 0);
        write_identity_matrix(&w);
        mw_u32(&w, 0); mw_u32(&w, 0); mw_u32(&w, 0);
        mw_u32(&w, 0); mw_u32(&w, 0); mw_u32(&w, 0);
        mw_u32(&w, (uint32_t)n + 1u);
        mw_close(&w, ma);
    }
    for (i = 0; i < n; i++) {
        write_trak(&w, &tracks[i], chunk, use64, 1000);
        chunk += tracks[i].total_bytes;
    }
    mw_close(&w, at);
    return w.pos;
}

uint32_t mp4_mdat_header(uint64_t payload, uint8_t out[16]) {
    if (payload + 8u <= 0xFFFFFFFFu) {
        uint32_t sz = (uint32_t)(payload + 8u);
        out[0] = (uint8_t)(sz >> 24); out[1] = (uint8_t)(sz >> 16);
        out[2] = (uint8_t)(sz >> 8);  out[3] = (uint8_t)sz;
        memcpy(out + 4, "mdat", 4);
        return 8;
    }
    {
        uint64_t sz = payload + 16u;
        int k;
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
        memcpy(out + 4, "mdat", 4);
        for (k = 0; k < 8; k++) out[8 + k] = (uint8_t)(sz >> (56 - 8 * k));
    }
    return 16;
}
