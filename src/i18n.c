/* i18n.c — پیاده‌سازی جدول‌های ترجمه‌ی embedded */
#include "i18n.h"
#include <string.h> /* _stricmp روی MinGW از همین‌جا میاد */

static Lass_Lang g_current_lang = LASS_LANG_EN;

/* ترتیب دقیقاً باید با enum Str_ID در i18n.h یکی باشه — هر
   ورودی جدید اول به enum اضافه میشه، بعد این‌جا (هر دو جدول)
   در همون موقعیت. */
static const char *EN_STRINGS[STR_COUNT] = {
    /* states */
    "Idle", "Queued", "Connecting...", "Downloading", "Pausing...",
    "Paused", "Verifying", "Completed", "Failed", "Cancelled", "Unknown",
    /* columns */
    "#", "File Name", "Size", "Progress", "Speed", "Status", "Category",
    /* tray */
    "Show / Hide", "Add URL...", "Exit",
    /* context menu */
    "Pause", "Resume", "Cancel", "Open file", "Open folder",
    "Verify Checksum...",
    "Copy URL", "Schedule...", "Remove from list",
    /* toolbar */
    "Add URL", "Pause", "Resume", "Cancel", "Remove", "Open folder",
    "Speed Graph", "Basket", "Clip Monitor", "Clear Done",
    /* menu bar */
    "&File", "&Add URL...\tCtrl+N", "Add &Batch...", "Download &Sequence...", "E&xit",
    "&View", "Speed &Graph", "Download &Basket",
    "&Settings", "&Preferences...\tCtrl+,", "&Clipboard Monitor",
    "&Help", "&About",

    /* Preferences dialog */
    "Lass - Preferences",
    "General",
    "Max downloads (1-20):",
    "Segments per download (1-32):",
    "Download folder:",
    "Behavior",
    "Minimize to system tray",
    "Monitor clipboard for links",
    "Show floating download basket",
    "Run Lass on Windows startup",
    "Proxy",
    "Enable proxy:",
    "http://user:pass@host:port  |  socks5://host:port",
    "Browser Extension",
    "Speed",
    "Speed Schedule...",
    "Save & Apply",
    "Cancel",

    /* Speed Schedule dialog */
    "Lass - Speed Schedule",
    "Limit download speed during specific hours of the day.",
    "Enable speed schedule",
    "From hour (0-23):",
    "To hour (0-23):",
    "Speed limit (KB/s):",
    "Outside this range, downloads run at full speed.",
    "Save",

    /* Verify Checksum dialog */
    "Lass - Verify Checksum",
    "Algorithm:",
    "Expected hash:",
    "Computed hash:",
    "Verify",
    "Computing...",
    "\xE2\x9C\x93 Match \xE2\x80\x94 file is intact.",
    "\xE2\x9C\x97 Mismatch \xE2\x80\x94 file may be corrupted or altered.",

    /* Download Sequence dialog */
    "Lass - Download Sequence",
    "URL pattern (use # for digits, e.g. file###.jpg):",
    "From:",
    "To:",
    "Folder:",
    "Group:",
    "Add All",
    "Pattern must contain at least one # placeholder.",
    "'From' must be less than or equal to 'To'.",
    "Too many files in range (max 2000) \xE2\x80\x94 narrow the range.",

    /* Unscanned executable notification */
    "Executable Downloaded",
    "This file was not scanned. Verify the source before running it.",
};

static const char *FA_STRINGS[STR_COUNT] = {
    /* states */
    "بی‌کار", "در صف", "در حال اتصال...", "در حال دانلود", "در حال توقف...",
    "متوقف‌شده", "در حال بررسی", "کامل‌شده", "ناموفق", "لغوشده", "نامشخص",
    /* columns */
    "#", "نام فایل", "حجم", "پیشرفت", "سرعت", "وضعیت", "دسته",
    /* tray */
    "نمایش / پنهان‌سازی", "افزودن لینک...", "خروج",
    /* context menu */
    "توقف موقت", "ادامه", "لغو", "باز کردن فایل", "باز کردن پوشه",
    "تأیید چک‌سام...",
    "کپی لینک", "زمان‌بندی...", "حذف از لیست",
    /* toolbar */
    "افزودن لینک", "توقف موقت", "ادامه", "لغو", "حذف", "باز کردن پوشه",
    "نمودار سرعت", "سبد دانلود", "پایشگر کلیپ‌بورد", "پاک‌سازی کامل‌شده‌ها",
    /* menu bar — عمداً بدون میانبر &/Tab فارسی‌سازی نشده، چون این‌ها
       accelerator های واقعی ویندوز هستن (Ctrl+N و غیره کار می‌کنن)
       و تغییرشون بدون بازبینی کامل accelerator table ریسک داره؛
       فقط متن قابل‌خوندن ترجمه شده. */
    "فایل", "افزودن لینک...\tCtrl+N", "افزودن دسته‌ای...", "دانلود دنباله‌ای...", "خروج",
    "نما", "نمودار سرعت", "سبد دانلود",
    "تنظیمات", "تنظیمات...\tCtrl+,", "پایشگر کلیپ‌بورد",
    "راهنما", "درباره",

    /* Preferences dialog */
    "لس - تنظیمات",
    "عمومی",
    "حداکثر دانلود همزمان (1 تا 20):",
    "تعداد قطعه در هر دانلود (1 تا 32):",
    "پوشه‌ی دانلود:",
    "رفتار",
    "کوچک‌سازی به system tray",
    "پایش کلیپ‌بورد برای لینک",
    "نمایش سبد شناور دانلود",
    "اجرای Lass هنگام روشن شدن ویندوز",
    "پروکسی",
    "فعال‌سازی پروکسی:",
    "http://user:pass@host:port  |  socks5://host:port",
    "افزونه‌ی مرورگر",
    "سرعت",
    "زمان‌بندی سرعت...",
    "ذخیره و اعمال",
    "لغو",

    /* Speed Schedule dialog */
    "لس - زمان‌بندی سرعت",
    "محدود کردن سرعت دانلود در ساعات مشخصی از روز.",
    "فعال‌سازی زمان‌بندی سرعت",
    "از ساعت (0 تا 23):",
    "تا ساعت (0 تا 23):",
    "محدودیت سرعت (KB/s):",
    "خارج از این بازه، دانلودها با سرعت کامل اجرا می‌شن.",
    "ذخیره",

    /* Verify Checksum dialog */
    "لس - تأیید چک‌سام",
    "الگوریتم:",
    "چک‌سام مورد انتظار:",
    "چک‌سام محاسبه‌شده:",
    "تأیید",
    "در حال محاسبه...",
    "✓ تطابق دارد — فایل سالم است.",
    "✗ تطابق ندارد — ممکن است فایل خراب یا دستکاری‌شده باشد.",

    /* Download Sequence dialog */
    "لس - دانلود دنباله‌ای",
    "الگوی لینک (از # برای عدد استفاده کن، مثلاً file###.jpg):",
    "از:",
    "تا:",
    "پوشه:",
    "گروه:",
    "افزودن همه",
    "الگو باید حداقل یک # داشته باشد.",
    "«از» باید کوچک‌تر یا مساوی «تا» باشد.",
    "تعداد فایل‌ها در این بازه زیاده (حداکثر ۲۰۰۰) — بازه رو کوچیک‌تر کن.",

    /* Unscanned executable notification */
    "فایل اجرایی دانلود شد",
    "این فایل اسکن نشده است. قبل از اجرا از منبعش مطمئن شو.",
};

void i18n_set_language(Lass_Lang lang) {
    if (lang >= 0 && lang < LASS_LANG_COUNT)
        g_current_lang = lang;
    /* مقدار نامعتبر عمداً نادیده گرفته میشه — زبان فعلی همون
       می‌مونه که بود، به‌جای افتادن به یه enum خارج از محدوده. */
}

Lass_Lang i18n_current_language(void) {
    return g_current_lang;
}

Lass_Lang i18n_lang_from_code(const char *code) {
    if (!code || !code[0]) return LASS_LANG_EN;
    if (_stricmp(code, "fa") == 0) return LASS_LANG_FA;
    return LASS_LANG_EN; /* هر کد ناشناخته → پیش‌فرض امن */
}

const char *i18n_lang_code(Lass_Lang lang) {
    return (lang == LASS_LANG_FA) ? "fa" : "en";
}

const char *i18n_str(Str_ID id) {
    if (id < 0 || id >= STR_COUNT) return "";
    const char *s = (g_current_lang == LASS_LANG_FA)
                    ? FA_STRINGS[id] : EN_STRINGS[id];
    return s ? s : "";
}
