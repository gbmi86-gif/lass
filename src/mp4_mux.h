/* mp4_mux.h
   ─────────────────────────────────────────────────────────────
   خواندن fMP4 (init + قطعات moof/traf/trun) و نوشتن MP4 معمولی
   (progressive، با moov پیش از داده برای پخش فوری) بدون lib خارجی.

   این ماژول فقط ساختار ISO BMFF را می‌فهمد؛ دانلود و کانتینر در dash_download.c است.

   پشتیبانی:
     - init با تا MP4_MAX_TRACKS track؛ هر track یک stsd با یک ورودی
     - قطعات با tfhd (base_data_offset، default-base-is-moof، پیش‌فرض‌ها)
       و trun (data_offset، first_sample_flags، duration/size/flags/cto)
     - ترتیب مطابق استاندارد برای آفست پایه‌ی traf های بعدی

   محدودیت‌ها (با خطای صریح، نه بی‌صدا):
     - stsd با بیش از یک ورودی، یا sample_description_index غیر از ۱
     - edit list (edts) نادیده گرفته می‌شود
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_MP4_MUX_H
#define LASS_MP4_MUX_H

#include <windows.h>
#include <stdint.h>

#define MP4_MAX_TRACKS 4

/* اطلاعات یک track از init segment؛ اشاره‌گرها به بافر init اشاره می‌کنند */
typedef struct {
    uint32_t track_id;
    uint32_t timescale;
    char     handler[5];          /* "vide"، "soun" ... */
    uint32_t width, height;       /* از tkhd، عدد صحیح */
    uint16_t language;            /* از mdhd، همان کد ۱۵ بیتی */
    const uint8_t *stsd;          uint32_t stsd_len;
    const uint8_t *media_hdr;     uint32_t media_hdr_len;   /* vmhd یا smhd */
    const uint8_t *dinf;          uint32_t dinf_len;        /* ۰ یعنی پیش‌فرض */
    const uint8_t *hdlr;          uint32_t hdlr_len;
    uint32_t trex_duration;
    uint32_t trex_size;
    uint32_t trex_flags;
    BOOL     trex_found;
    const uint8_t *edts;          uint32_t edts_len;   /* edts کامل؛ ۰ یعنی ندارد */
} Mp4TrackInfo;

typedef struct {
    Mp4TrackInfo track[MP4_MAX_TRACKS];
    int          track_count;
    uint32_t     movie_timescale;   /* از mvhd؛ بعد از تجزیه هرگز ۰ نیست */
} Mp4Init;

/* یک نمونه از یک قطعه؛ offset مطلق داخل بافر segment است */
typedef struct {
    uint32_t track_id;
    uint64_t offset;
    uint32_t size;
    uint32_t duration;     /* در timescale همان track */
    int32_t  cto;          /* composition offset */
    BOOL     sync;
} Mp4FragSample;

/* FALSE برگرداندن یعنی ادامه نده؛ walker با خطا برمی‌گردد */
typedef BOOL (*Mp4SampleFn)(const Mp4FragSample *s, void *ctx);

/* خروجی: هر track یک جدول نمونه‌ها، با نمونه‌های پیوسته در mdat */
typedef struct {
    uint32_t track_id;            /* track_ID در خروجی (۱ به بعد) */
    uint32_t timescale;
    char     handler[5];
    uint32_t width, height;
    uint16_t language;
    BOOL     is_audio;
    const uint8_t *stsd;          uint32_t stsd_len;
    const uint8_t *media_hdr;     uint32_t media_hdr_len;
    const uint8_t *dinf;          uint32_t dinf_len;
    const uint8_t *hdlr;          uint32_t hdlr_len;
    uint32_t       sample_count;  /* بیشتر از ۰ */
    const uint32_t *sizes;
    const uint32_t *durations;
    const int32_t  *ctos;         /* NULL یعنی همه صفر */
    const uint8_t  *sync;         /* NULL یعنی همه sync */
    uint64_t        total_bytes;  /* مجموع sizes */
    const uint8_t  *edts;         uint32_t edts_len;   /* از Mp4TrackInfo؛ ۰ یعنی ندارد */
    uint32_t        src_movie_ts; /* مقیاس mvhd منبع؛ مدت elst به ۱۰۰۰ تبدیل می‌شود */
} Mp4OutTrack;

/* init (moov) را تجزیه می‌کند؛ buf باید تا پایان استفاده زنده بماند */
BOOL     mp4_parse_init(const uint8_t *buf, uint64_t len, Mp4Init *out,
                        char *err, size_t err_sz);
int      mp4_find_track(const Mp4Init *init, uint32_t track_id);   /* اندیس یا -1 */

/* همه‌ی moof های یک segment را می‌خواند؛ برای هر نمونه fn صدا زده می‌شود */
BOOL     mp4_walk_fragments(const uint8_t *seg, uint64_t len, const Mp4Init *init,
                            Mp4SampleFn fn, void *ctx, char *err, size_t err_sz);

/* خروجی: ftyp، moov (با buf=NULL فقط اندازه)، و سرآیند mdat؛ برمی‌گردانند اندازه‌ها */
uint64_t mp4_write_ftyp(uint8_t *buf, uint64_t cap);
uint64_t mp4_write_moov(const Mp4OutTrack *tracks, int n, uint64_t data_start,
                        BOOL use64, uint8_t *buf, uint64_t cap);
uint32_t mp4_mdat_header(uint64_t payload, uint8_t out[16]);

#endif /* LASS_MP4_MUX_H */
