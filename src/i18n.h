/* i18n.h — سیستم چندزبانه‌ی سبک برای رشته‌های UI
   ─────────────────────────────────────────────────────────────
   طراحی عمداً ساده: به‌جای فایل‌های زبان خارجی (که می‌تونن گم/خراب
   بشن و باعث یه failure mode جدید بشن)، جدول‌های ترجمه مستقیم توی
   i18n.c به‌صورت static embedded شدن — یعنی برنامه همیشه، با یا
   بدون هیچ فایل کناری، زبانش کامل و درسته.

   این اولین گام i18n هست، نه پوشش کامل: رشته‌های اصلی و همیشه‌
   دیده‌شونده (state، ستون‌های ListView، منوی tray/context/menubar،
   toolbar) پوشش داده شدن. دیالوگ‌های کم‌تکرار (Preferences، About،
   Schedule) هنوز فارسی/انگلیسی هاردکد هستن — یه گام بعدی طبیعی،
   نه چیزی که این پاس ادعا کنه انجام داده.

   افزودن یه رشته‌ی جدید: 1) یه STR_* به enum پایین اضافه کن،
   2) هم توی EN_STRINGS هم FA_STRINGS (در i18n.c) به همون ترتیب
   یه مقدار بذار — کامپایلر با تعداد نامساوی عضو هشدار میده اگه
   جا بندازی چون هر دو جدول به‌اندازه‌ی STR_COUNT تعریف میشن.
   ─────────────────────────────────────────────────────────────
*/
#ifndef LASS_I18N_H
#define LASS_I18N_H

typedef enum {
    /* ── Download states (core/guo.c: state_str) ── */
    STR_STATE_IDLE = 0,
    STR_STATE_QUEUED,
    STR_STATE_CONNECTING,
    STR_STATE_DOWNLOADING,
    STR_STATE_PAUSING,
    STR_STATE_PAUSED,
    STR_STATE_VERIFYING,
    STR_STATE_COMPLETED,
    STR_STATE_FAILED,
    STR_STATE_CANCELLED,
    STR_STATE_UNKNOWN,

    /* ── ListView column headers ── */
    STR_COL_NUM,
    STR_COL_FILENAME,
    STR_COL_SIZE,
    STR_COL_PROGRESS,
    STR_COL_SPEED,
    STR_COL_STATUS,
    STR_COL_CATEGORY,

    /* ── Tray icon context menu ── */
    STR_TRAY_SHOW,
    STR_TRAY_ADD,
    STR_TRAY_EXIT,

    /* ── ListView row context menu ── */
    STR_CTX_PAUSE,
    STR_CTX_RESUME,
    STR_CTX_CANCEL,
    STR_CTX_OPEN_FILE,
    STR_CTX_OPEN_FOLDER,
    STR_CTX_VERIFY_CHECKSUM,
    STR_CTX_COPY_URL,
    STR_CTX_SCHEDULE,
    STR_CTX_REMOVE,

    /* ── Toolbar button tooltips ── */
    STR_TB_ADD,
    STR_TB_PAUSE,
    STR_TB_RESUME,
    STR_TB_CANCEL,
    STR_TB_REMOVE,
    STR_TB_OPEN_FOLDER,
    STR_TB_SPEED_GRAPH,
    STR_TB_BASKET,
    STR_TB_CLIP_MONITOR,
    STR_TB_CLEAR_DONE,

    /* ── Top menu bar ── */
    STR_MENU_FILE,
    STR_MENU_ADD_URL,
    STR_MENU_ADD_BATCH,
    STR_MENU_ADD_SEQ,
    STR_MENU_EXIT,
    STR_MENU_VIEW,
    STR_MENU_SPEED_GRAPH,
    STR_MENU_BASKET,
    STR_MENU_SETTINGS,
    STR_MENU_PREFERENCES,
    STR_MENU_CLIP_MONITOR,
    STR_MENU_HELP,
    STR_MENU_ABOUT,

    /* ── Preferences dialog ── */
    STR_PREFS_TITLE,
    STR_PREFS_GENERAL,
    STR_PREFS_MAX_DL,
    STR_PREFS_SEGMENTS,
    STR_PREFS_DIR,
    STR_PREFS_BEHAVIOR,
    STR_PREFS_TRAY,
    STR_PREFS_CLIP,
    STR_PREFS_BASKET,
    STR_PREFS_STARTUP,
    STR_PREFS_PROXY_GROUP,
    STR_PREFS_PROXY_ENABLE,
    STR_PREFS_PROXY_HINT,
    STR_PREFS_BROWSER_EXT,
    STR_PREFS_SPEED_GROUP,
    STR_PREFS_SPEED_SCHED_BTN,
    STR_PREFS_SAVE,
    STR_GENERIC_CANCEL,

    /* ── Speed Schedule dialog ── */
    STR_SPEEDSCHED_TITLE,
    STR_SPEEDSCHED_DESC,
    STR_SPEEDSCHED_ENABLE,
    STR_SPEEDSCHED_FROM,
    STR_SPEEDSCHED_TO,
    STR_SPEEDSCHED_LIMIT,
    STR_SPEEDSCHED_OUTSIDE,
    STR_GENERIC_SAVE,

    /* ── Verify Checksum dialog ── */
    STR_CHECKSUM_TITLE,
    STR_CHECKSUM_ALGO,
    STR_CHECKSUM_EXPECTED,
    STR_CHECKSUM_COMPUTED,
    STR_CHECKSUM_VERIFY_BTN,
    STR_CHECKSUM_COMPUTING,
    STR_CHECKSUM_MATCH,
    STR_CHECKSUM_MISMATCH,

    /* ── Download Sequence dialog ── */
    STR_SEQ_TITLE,
    STR_SEQ_PATTERN,
    STR_SEQ_FROM,
    STR_SEQ_TO,
    STR_SEQ_FOLDER,
    STR_SEQ_GROUP,
    STR_SEQ_ADD_ALL,
    STR_SEQ_ERR_NO_HASH,
    STR_SEQ_ERR_RANGE,
    STR_SEQ_ERR_TOO_MANY,

    /* ── Unscanned-executable notification ── */
    STR_NOTIFY_UNSCANNED_TITLE,
    STR_NOTIFY_UNSCANNED_BODY,

    STR_COUNT /* همیشه آخر — سایز جدول‌ها */
} Str_ID;

typedef enum {
    LASS_LANG_EN = 0,
    LASS_LANG_FA = 1,
    LASS_LANG_COUNT
} Lass_Lang;

/* زبان فعلی رو تغییر میده — thread-safe نیست، فقط باید از UI
   thread و ترجیحاً قبل از ساخت هر پنجره‌ای صدا زده بشه (تغییر
   زبان بعد از ساخت پنجره‌ها باعث نمی‌شه متن‌های از‌قبل-نوشته‌شده
   خودکار به‌روز بشن — این محدودیت شناخته‌شده‌ست، نه باگ). */
void i18n_set_language(Lass_Lang lang);
Lass_Lang i18n_current_language(void);

/* تبدیل کد دو-حرفی (مثل "fa"، "en") به enum — کد ناشناخته یا NULL
   همیشه به LASS_LANG_EN می‌افته، پس رفتار پیش‌فرض برنامه هیچ‌وقت
   با یه کد بد/گم‌شده در ini تغییر نمی‌کنه. */
Lass_Lang i18n_lang_from_code(const char *code);
const char *i18n_lang_code(Lass_Lang lang); /* برای ذخیره در lass.ini */

/* رشته‌ی ترجمه‌شده برای id، طبق زبان فعلی. id نامعتبر یا زبان
   نامعتبر همیشه یه رشته‌ی خالی امن برمی‌گردونه، نه NULL — کد
   صدازننده هیچ‌وقت لازم نیست چک null کنه. */
const char *i18n_str(Str_ID id);

#endif /* LASS_I18N_H */
