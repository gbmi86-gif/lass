/* guo.c
   ─────────────────────────────────────────────────────────────
   Lass – Graphic Unit Operator

   تمام کد UI اینجاست. هیچ منطق دانلودی نیست.
   هر تعامل کاربر → core_dispatch() → engine
   هر رویداد engine → LDM_NotifyCb → آپدیت UI
   ─────────────────────────────────────────────────────────────
*/
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>    /* PathCombineA */
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "guo.h"
#include "../src/types.h"
#include "../src/engine.h"
#include "../src/version.h"
#include "../src/settings.h"
#include "../src/i18n.h"
#include "../plugins/plugins_setup.h"
#include "../plugins/plugin_speed_graph.h"
#include "../plugins/plugin_clipboard.h"
#include "../plugins/plugin_basket.h"
#include "../plugins/plugin_browser_ext.h"
#include "../src/checksum.h"
#include "../plugins/plugin_proxy.h"
#include "../plugins/plugin_categories.h"
#include "../plugins/plugin_notify.h"
#include "../plugins/plugin_rtmp.h"
#include "../plugins/plugin_video_sniffer.h"
#include "../src/lass_log.h"
#include "../src/shutdown_brake.h"

/* ── global context (singleton — یه پنجره اصلی داریم) ── */
static GUO_Context *g_guo = NULL;

/* ======================================================
   IDs
   ====================================================== */
/* Toolbar */
#define IDC_TOOLBAR    1001
#define IDC_LISTVIEW   1002
#define IDC_STATUSBAR  1003
#define IDC_TIMER      1004

#define ID_BTN_ADD     2001
#define ID_BTN_PAUSE   2002
#define ID_BTN_RESUME  2003
#define ID_BTN_CANCEL  2004
#define ID_BTN_REMOVE  2005
#define ID_BTN_OPEN    2006
#define ID_BTN_GRAPH   2007
#define ID_BTN_BASKET  2008
#define ID_BTN_CLIPMON 2009
#define ID_BTN_CLEAR   2010
#define IDC_CAT_FILTER 2011

/* Menu bar */
#define IDM_FILE_ADD        3001
#define IDM_FILE_ADDGROUP   3010
#define IDM_FILE_ADDSEQ     3011
#define IDM_FILE_EXIT       3002
#define IDM_VIEW_GRAPH      3003
#define IDM_VIEW_BASKET     3004
#define IDM_SETTINGS_DLG    3006
#define IDM_SETTINGS_CLIPMON 3007
#define IDM_HELP_ABOUT      3009

/* Context menu */
#define IDM_CTX_PAUSE       4001

/* ── async wrapper برای عملیات blocking (pause/cancel/remove) ──
   core_pause/core_cancel می‌تونن تا چند ثانیه block کنن (seg_pause
   منتظر worker thread دانلود می‌مونه). اگه مستقیم از UI message
   loop صدا زده بشن، Windows برنامه رو "Not Responding" نشون میده.
   این wrapper یه thread جداگانه می‌سازه تا UI فوری برگرده. */
typedef struct {
    LDM_Core *core;
    uint32_t  id;
    int       op; /* 0=pause, 1=cancel, 2=remove */
} AsyncOp;

static DWORD WINAPI async_op_thread(LPVOID param) {
    AsyncOp *op = (AsyncOp *)param;
    int brake_id = shutdown_brake_register("ui_async_op");
    switch (op->op) {
        case 0: core_pause (op->core, op->id); break;
        case 1: core_cancel(op->core, op->id); break;
        case 2: core_remove(op->core, op->id); break;
    }
    free(op);
    shutdown_brake_unregister(brake_id);
    return 0;
}

static void dispatch_async(LDM_Core *core, uint32_t id, int op_type) {
    AsyncOp *op = (AsyncOp *)malloc(sizeof(*op));
    if (!op) return;
    op->core = core; op->id = id; op->op = op_type;
    HANDLE t = CreateThread(NULL, 0, async_op_thread, op, 0, NULL);
    if (t) CloseHandle(t);
    else { free(op); } /* fallback: نباید اتفاق بیفته */
}
#define IDM_CTX_RESUME      4002
#define IDM_CTX_CANCEL      4003
#define IDM_CTX_REMOVE      4004
#define IDM_CTX_OPEN_FILE   4005
#define IDM_CTX_OPEN_FOLDER 4006
#define IDM_CTX_VERIFY_CHECKSUM 4009
#define IDM_CTX_COPY_URL    4007
#define IDM_CTX_SCHEDULE    4008

/* Settings dialog controls */
#define IDC_SET_MAXDL    5001
#define IDC_SET_SEGS     5002
#define IDC_SET_DIR      5003
#define IDC_SET_BROWSE   5004
#define IDC_SET_CLIP     5005
#define IDC_SET_BASKET   5006
#define IDC_SET_TRAY     5007
#define IDC_SET_STARTUP  5008
#define IDC_SET_PROXY    5009
#define IDC_SET_PROXYURL 5010
#define IDC_SET_EXT_REG  5011
#define IDC_SET_UNREG    5012
#define IDC_SET_SPEED_SCHED 5013
#define IDC_SET_EXT_TEST 5014

/* Tray */
#define WM_TRAY_ICON  (WM_APP + 10)
#define ID_TRAY_ICON   9001
#define ID_TRAY_SHOW   9002
#define ID_TRAY_ADD    9003
#define ID_TRAY_EXIT   9004

/* ListView columns */
#define COL_NUM      0
#define COL_NAME     1
#define COL_SIZE     2
#define COL_PROGRESS 3
#define COL_SPEED    4
#define COL_STATUS   5
#define COL_CATEGORY 6
#define COL_COUNT    7

/* WndClass names */
#define WC_MAIN     "LassMainWnd"
#define WC_ADDDLG   "LassAddDlg"
#define WC_SETTINGS "LassSettingsWnd"

#define IDI_MAIN_ICON 1   /* باید با resource.rc یکی باشه */

/* ── آیکون‌های toolbar — باید دقیقاً با resource.rc یکی باشن ── */
#define IDI_TB_ADD          4001
#define IDI_TB_PAUSE        4002
#define IDI_TB_RESUME       4003
#define IDI_TB_CANCEL       4004
#define IDI_TB_REMOVE       4005
#define IDI_TB_OPEN_FOLDER  4006
#define IDI_TB_SPEED_GRAPH  4007
#define IDI_TB_BASKET       4008
#define IDI_TB_CLIP_MONITOR 4009
#define IDI_TB_CLEAR_DONE   4010

/* ======================================================
   Helpers
   ====================================================== */
/* ── ترکیب امنِ دایرکتوری + نام فایل ──
   رجوع کن به توضیح کامل کنار نسخه‌ی مشابه در plugin_categories.c.
   خلاصه: PathCombineA صرف‌نظر از اندازه‌ی بافر مقصد، یه سقف داخلی
   MAX_PATH (۲۶۰) روی نتیجه داره و روی عبور از اون شکست می‌خوره؛
   یه filename طولانیِ percent-encoded (مثلاً عنوان فارسی در URL
   یه mirror ایرانی، که هر حرف فارسیش ۶ کاراکتر ASCII می‌شه) خیلی
   راحت از این سقف رد می‌شه. این تابع قبل از PathCombineA طول رو
   چک و در صورت لزوم filename رو (با حفظ پسوند) کوتاه می‌کنه. */
static BOOL safe_path_combine(char *out, size_t out_sz,
                              const char *dir, const char *fname) {
    if (!out || out_sz == 0) return FALSE;
    out[0] = '\0';
    if (out_sz < (size_t)MAX_PATH) return FALSE;
    if (!dir) dir = "";
    if (!fname || !fname[0]) fname = "download.bin";

    size_t dir_len = strlen(dir);
    size_t fn_len  = strlen(fname);
    size_t budget  = (size_t)MAX_PATH - 2;

    char fn_buf[MAX_PATH];
    if (dir_len + fn_len > budget) {
        size_t room = (budget > dir_len) ? (budget - dir_len) : 1;
        if (room >= sizeof(fn_buf)) room = sizeof(fn_buf) - 1;

        const char *dot = strrchr(fname, '.');
        size_t ext_len = (dot && strlen(dot) <= 10) ? strlen(dot) : 0;
        if (ext_len >= room) ext_len = 0;

        size_t base_room = room - ext_len;
        if (base_room < 1) base_room = 1;
        memcpy(fn_buf, fname, base_room);
        if (ext_len) memcpy(fn_buf + base_room, dot, ext_len);
        fn_buf[base_room + ext_len] = '\0';
        fname = fn_buf;
    }

    return PathCombineA(out, dir, fname) != NULL;
}

static void fmt_speed(double bps, char *out, int sz) {
    if      (bps >= 1024.0*1024.0) snprintf(out,sz,"%.1f MB/s",bps/1024.0/1024.0);
    else if (bps >= 1024.0)         snprintf(out,sz,"%.0f KB/s",bps/1024.0);
    else if (bps > 0)               snprintf(out,sz,"%.0f B/s", bps);
    else                            snprintf(out,sz,"--");
}

static void fmt_size(uint64_t b, char *out, int sz) {
    if      (b >= 1024ULL*1024*1024) snprintf(out,sz,"%.2f GB",b/1073741824.0);
    else if (b >= 1024ULL*1024)      snprintf(out,sz,"%.1f MB",b/1048576.0);
    else if (b >= 1024ULL)           snprintf(out,sz,"%.0f KB",b/1024.0);
    else if (b > 0)                  snprintf(out,sz,"%llu B",(unsigned long long)b);
    else                             snprintf(out,sz,"--");
}

static const char *state_str(int s) {
    switch ((DL_State)s) {
    case DL_IDLE:        return i18n_str(STR_STATE_IDLE);
    case DL_QUEUED:      return i18n_str(STR_STATE_QUEUED);
    case DL_CONNECTING:  return i18n_str(STR_STATE_CONNECTING);
    case DL_LAUNCHING:   return i18n_str(STR_STATE_CONNECTING);
    case DL_DOWNLOADING: return i18n_str(STR_STATE_DOWNLOADING);
    case DL_PAUSING:     return i18n_str(STR_STATE_PAUSING);
    case DL_PAUSED:      return i18n_str(STR_STATE_PAUSED);
    case DL_VERIFYING:   return i18n_str(STR_STATE_VERIFYING);
    case DL_COMPLETED:   return i18n_str(STR_STATE_COMPLETED);
    case DL_FAILED:      return i18n_str(STR_STATE_FAILED);
    case DL_CANCELLED:   return i18n_str(STR_STATE_CANCELLED);
    default:             return i18n_str(STR_STATE_UNKNOWN);
    }
}

/* برای DL_FAILED، اگه fail_reason موجود باشه، به‌جای "Failed"
   مبهم، دلیل دقیق رو نشون بده — مثلاً "Failed: Link not found
   (404)" به‌جای اینکه کاربر مجبور باشه لاگ رو باز کنه تا بفهمه
   چرا. */
static void status_text(const LDM_Item *it, char *out, int out_sz) {
    if (it->state == (int)DL_FAILED && it->fail_reason[0] != '\0') {
        snprintf(out, out_sz, "Failed: %s", it->fail_reason);
    } else {
        snprintf(out, out_sz, "%s", state_str(it->state));
    }
}

/* ======================================================
   ListView helpers
   ====================================================== */
static int lv_find_by_id(HWND lv, uint32_t id) {
    LVFINDINFOA fi = {0};
    fi.flags  = LVFI_PARAM;
    fi.lParam = (LPARAM)id;
    return ListView_FindItem(lv, -1, &fi);
}

static int lv_selected_id(HWND lv) {
    int idx = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (idx < 0) return -1;
    LVITEMA li;
    memset(&li, 0, sizeof(li));
    li.mask  = LVIF_PARAM;
    li.iItem = idx;
    SendMessageA(lv, LVM_GETITEMA, 0, (LPARAM)&li);
    return (int)li.lParam;
}

/* ── محاسبه نام category برای یه آیتم — از پلاگین categories ── */
static const char *get_item_category(const LDM_Item *it) {
    IDM_PluginHooks *cat = plugins_get_categories();
    if (!cat) return "";
    int idx = categories_classify(cat, it->url, it->filename);
    return (idx >= 0) ? categories_name(cat, idx) : "Other";
}

static void lv_add(HWND lv, const LDM_Item *it) {
    int n = ListView_GetItemCount(lv);
    LVITEMA li = {0};
    li.mask    = LVIF_TEXT | LVIF_PARAM;
    li.iItem   = n;
    li.lParam  = (LPARAM)it->id;
    char num[16]; snprintf(num, sizeof(num), "%d", n+1);
    li.pszText = num;
    SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&li);

    char sz[32], spd[32], pct[16];
    fmt_size(it->total_size, sz, sizeof(sz));
    fmt_speed(it->speed_bps, spd, sizeof(spd));
    snprintf(pct, sizeof(pct), "%d%%", it->progress_pct);

    ListView_SetItemText(lv, n, COL_NAME,     (char*)it->filename);
    ListView_SetItemText(lv, n, COL_SIZE,     sz);
    ListView_SetItemText(lv, n, COL_PROGRESS, pct);
    ListView_SetItemText(lv, n, COL_SPEED,    spd);
    { char st[128]; status_text(it, st, sizeof(st));
      ListView_SetItemText(lv, n, COL_STATUS, st); }
    ListView_SetItemText(lv, n, COL_CATEGORY, (char*)get_item_category(it));
}

static void lv_update(HWND lv, const LDM_Item *it) {
    int idx = lv_find_by_id(lv, it->id);
    if (idx < 0) { lv_add(lv, it); return; }

    char sz[32], spd[32], pct[16];
    fmt_size(it->total_size, sz, sizeof(sz));
    fmt_speed(it->speed_bps, spd, sizeof(spd));
    snprintf(pct, sizeof(pct), "%d%%", it->progress_pct);

    ListView_SetItemText(lv, idx, COL_NAME,     (char*)it->filename);
    ListView_SetItemText(lv, idx, COL_SIZE,     sz);
    ListView_SetItemText(lv, idx, COL_PROGRESS, pct);
    ListView_SetItemText(lv, idx, COL_SPEED,    spd);
    { char st[128]; status_text(it, st, sizeof(st));
      ListView_SetItemText(lv, idx, COL_STATUS, st); }
    ListView_SetItemText(lv, idx, COL_CATEGORY, (char*)get_item_category(it));
}

/* ======================================================
   Core notification callback → آپدیت UI
   ====================================================== */
static void guo_on_notify(const LDM_Notify *n, void *ud) {
    GUO_Context *g = (GUO_Context *)ud;
    if (!g) return;

    switch (n->type) {
    case LDM_NOTIFY_ADDED:
    case LDM_NOTIFY_PROGRESS:
    case LDM_NOTIFY_STATE: {
        /* اگه فیلتر فعاله و این آیتم متعلق به category انتخابی نیست، نادیده بگیر */
        if (g->cat_filter != -2) {
            IDM_PluginHooks *cat = plugins_get_categories();
            int idx = cat ? categories_classify(cat, n->item.url, n->item.filename) : -1;
            if (idx != g->cat_filter) {
                /* اگه قبلاً در لیست بود (مثلاً فیلتر بعد از اضافه‌شدن تغییر کرد) حذفش کن */
                int li = lv_find_by_id(g->listview, n->item.id);
                if (li >= 0) ListView_DeleteItem(g->listview, li);
                break;
            }
        }
        lv_update(g->listview, &n->item);
        break;
    }

    case LDM_NOTIFY_REMOVED: {
        int idx = lv_find_by_id(g->listview, n->id);
        if (idx >= 0) ListView_DeleteItem(g->listview, idx);
        break;
    }

    case LDM_NOTIFY_CLEARED: {
        /* حذف همه آیتم‌هایی که دیگه در core نیستن */
        int i = 0;
        while (i < ListView_GetItemCount(g->listview)) {
            LVITEMA li;
            memset(&li, 0, sizeof(li));
            li.mask  = LVIF_PARAM;
            li.iItem = i;
            SendMessageA(g->listview, LVM_GETITEMA, 0, (LPARAM)&li);
            LDM_Item it;
            if (!core_get_item(g->core, (uint32_t)li.lParam, &it))
                ListView_DeleteItem(g->listview, i);
            else
                i++;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Cleared %d completed download(s).", n->count);
        SendMessageA(g->statusbar, SB_SETTEXTA, 0, (LPARAM)msg);
        break;
    }

    case LDM_NOTIFY_EXT_URL: {
        /* برخلاف LDM_NOTIFY_CLIP_URL (که یه URL توی clipboard پیدا
           شده و ممکنه اصلاً منظور کاربر دانلود نبوده)، این‌جا کاربر
           از قبل توی مرورگر مشخصاً روی «Download with Lass» کلیک
           کرده — یعنی تأیید از قبل انجام شده. یه دیالوگ تأییدِ دومِ
           modal این‌جا فقط اصطکاک اضافه‌ست، و بدتر: اگه پنجره‌ی اصلی
           minimize-to-tray شده باشه (حالت رایج، مخصوصاً چون Run at
           Startup معمولاً با همین ترکیب استفاده میشه)، این دیالوگ
           ممکنه دیده نشه و کاربر فکر کنه دانلود «توی صف گیر کرده»
           در حالی که واقعاً حتی وارد صف هم نشده و منتظر یه کلیکِ
           دیده‌نشده‌ست — دقیقاً همون رفتار مشکوکی که گزارش شد. */
        char fname[256] = {0};
        const char *sl = strrchr(n->url, '/');
        strncpy(fname, (sl && *(sl+1)) ? sl+1 : "download", 255);
        char *q = strchr(fname, '?'); if (q) *q = '\0';
        if (!fname[0]) strcpy(fname, "download");

        char path[MAX_PATH*2] = {0};
        IDM_PluginHooks *cat = plugins_get_categories();
        if (cat)
            categories_resolve_path(cat, n->url, fname,
                                     g->cfg->default_dir,
                                     path, sizeof(path));
        else if (g->cfg->default_dir[0])
            safe_path_combine(path, sizeof(path), g->cfg->default_dir, fname);
        else {
            GetTempPathA(sizeof(path), path);
            strncat(path, fname, sizeof(path)-strlen(path)-1);
        }
        core_add_ex(g->core, n->url, path,
                    g->cfg->default_segments, 0,
                    n->referer[0] ? n->referer : NULL);
        break;
    }

    case LDM_NOTIFY_CLIP_URL: {
        char dlg_msg[2300];
        snprintf(dlg_msg, sizeof(dlg_msg),
                 "URL detected:\n\n%.200s\n\nAdd to download queue?", n->url);
        if (MessageBoxA(g->main_wnd, dlg_msg, "Lass",
                         MB_YESNO | MB_ICONQUESTION) == IDYES) {
            char fname[256] = {0};
            const char *sl = strrchr(n->url, '/');
            strncpy(fname, (sl && *(sl+1)) ? sl+1 : "download", 255);
            char *q = strchr(fname, '?'); if (q) *q = '\0';
            if (!fname[0]) strcpy(fname, "download");

            char path[MAX_PATH*2] = {0};
            IDM_PluginHooks *cat = plugins_get_categories();
            if (cat)
                categories_resolve_path(cat, n->url, fname,
                                         g->cfg->default_dir,
                                         path, sizeof(path));
            else if (g->cfg->default_dir[0])
                safe_path_combine(path, sizeof(path), g->cfg->default_dir, fname);
            else {
                GetTempPathA(sizeof(path), path);
                strncat(path, fname, sizeof(path)-strlen(path)-1);
            }
            core_add_ex(g->core, n->url, path,
                        g->cfg->default_segments, 0,
                        n->referer[0] ? n->referer : NULL);
        }
        break;
    }
    }
}

/* ======================================================
   Tray
   ====================================================== */
static void tray_add(GUO_Context *g) {
    if (g->tray_added) return;
    NOTIFYICONDATAA nid = {0};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g->main_wnd;
    nid.uID              = ID_TRAY_ICON;
    nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY_ICON;
    /* آیکون خود برنامه از resource — fallback به آیکون پیش‌فرض سیستم */
    HICON hi = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAIN_ICON));
    nid.hIcon = hi ? hi : LoadIconA(NULL, IDI_APPLICATION);
    strncpy(nid.szTip, "Lass Download Manager", sizeof(nid.szTip)-1);
    Shell_NotifyIconA(NIM_ADD, &nid);
    g->tray_added = TRUE;
}

static void tray_remove(GUO_Context *g) {
    if (!g->tray_added) return;
    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g->main_wnd;
    nid.uID    = ID_TRAY_ICON;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    g->tray_added = FALSE;
}

static void tray_menu(GUO_Context *g) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_TRAY_SHOW, i18n_str(STR_TRAY_SHOW));
    AppendMenuA(m, MF_STRING, ID_TRAY_ADD,  i18n_str(STR_TRAY_ADD));
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_TRAY_EXIT, i18n_str(STR_TRAY_EXIT));
    SetForegroundWindow(g->main_wnd);
    TrackPopupMenu(m, TPM_RIGHTALIGN|TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g->main_wnd, NULL);
    DestroyMenu(m);
}

/* ======================================================
   Context Menu روی ListView
   ====================================================== */
static void show_checksum_dialog(GUO_Context *g, const char *filepath);

static void ctx_menu(GUO_Context *g, int dl_id) {
    LDM_Item it = {0};
    if (dl_id < 0) return;
    core_get_item(g->core, (uint32_t)dl_id, &it);

    HMENU m = CreatePopupMenu();

    if (it.state == DL_DOWNLOADING || it.state == DL_LAUNCHING)
        AppendMenuA(m, MF_STRING, IDM_CTX_PAUSE,  i18n_str(STR_CTX_PAUSE));
    else if (it.state == DL_PAUSED || it.state == DL_FAILED)
        AppendMenuA(m, MF_STRING, IDM_CTX_RESUME, i18n_str(STR_CTX_RESUME));

    if (it.state==DL_DOWNLOADING||it.state==DL_LAUNCHING||
        it.state==DL_QUEUED||it.state==DL_PAUSED)
        AppendMenuA(m, MF_STRING, IDM_CTX_CANCEL, i18n_str(STR_CTX_CANCEL));

    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    if (it.state == DL_COMPLETED) {
        AppendMenuA(m, MF_STRING, IDM_CTX_OPEN_FILE,   i18n_str(STR_CTX_OPEN_FILE));
        AppendMenuA(m, MF_STRING, IDM_CTX_OPEN_FOLDER, i18n_str(STR_CTX_OPEN_FOLDER));
        AppendMenuA(m, MF_STRING, IDM_CTX_VERIFY_CHECKSUM, i18n_str(STR_CTX_VERIFY_CHECKSUM));
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuA(m, MF_STRING, IDM_CTX_COPY_URL, i18n_str(STR_CTX_COPY_URL));
    AppendMenuA(m, MF_STRING, IDM_CTX_SCHEDULE, i18n_str(STR_CTX_SCHEDULE));
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_CTX_REMOVE,   i18n_str(STR_CTX_REMOVE));

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g->main_wnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD|TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, g->main_wnd, NULL);
    DestroyMenu(m);

    uint32_t id = (uint32_t)dl_id;
    switch (cmd) {
    case IDM_CTX_PAUSE:   dispatch_async(g->core, id, 0); break;
    case IDM_CTX_RESUME:  core_resume(g->core, id); break;
    case IDM_CTX_CANCEL:
        if (MessageBoxA(g->main_wnd,"Cancel this download?","Confirm",
                         MB_YESNO|MB_ICONQUESTION)==IDYES)
            dispatch_async(g->core, id, 1);
        break;
    case IDM_CTX_REMOVE:
        if (MessageBoxA(g->main_wnd,"Remove from list?","Confirm",
                         MB_YESNO|MB_ICONQUESTION)==IDYES)
            dispatch_async(g->core, id, 2);
        break;
    case IDM_CTX_OPEN_FILE:
        ShellExecuteA(g->main_wnd,"open",it.out_path,NULL,NULL,SW_SHOW);
        break;
    case IDM_CTX_OPEN_FOLDER: {
        char folder[MAX_PATH*2];
        strncpy(folder, it.out_path, sizeof(folder)-1);
        folder[sizeof(folder)-1] = '\0';
        char *bs=strrchr(folder,'\\'); if(bs)*bs='\0';
        ShellExecuteA(g->main_wnd,"open",folder,NULL,NULL,SW_SHOW);
        break;
    }
    case IDM_CTX_COPY_URL:
        if (it.url[0] && OpenClipboard(g->main_wnd)) {
            EmptyClipboard();
            size_t len=strlen(it.url)+1;
            HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,len);
            if(hg){char*p=(char*)GlobalLock(hg);
                   if(p){memcpy(p,it.url,len);GlobalUnlock(hg);}
                   SetClipboardData(CF_TEXT,hg);}
            CloseClipboard();
        }
        break;
    case IDM_CTX_SCHEDULE:
        guo_show_schedule(g, dl_id);
        break;
    case IDM_CTX_VERIFY_CHECKSUM:
        show_checksum_dialog(g, it.out_path);
        break;
    }
}

/* ======================================================
   Add Dialog (modeless)
   ====================================================== */
typedef struct { char url[2048]; char path[MAX_PATH];
                 uint32_t segs; uint64_t speed; } AddResult;
static AddResult g_add_res;

static LRESULT CALLBACK add_wnd_proc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    (void)lp;
    GUO_Context *g = g_guo;  /* از singleton استفاده می‌کنیم */
    HFONT fnt=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
    switch(msg){
    case WM_CREATE:{
        if (!g) return -1;
        HWND h;
#define MK(cls,txt,sty,x,y,w,hh,id) h=CreateWindowExA(0,cls,txt,\
    WS_CHILD|WS_VISIBLE|(sty),x,y,w,hh,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE)
        MK("STATIC", "URL:", 0,                     8,  10, 40, 18, 0);
        MK("EDIT",   "",     ES_AUTOHSCROLL|WS_BORDER,52,  8,400, 22, 101);
        MK("STATIC", "Save to:", 0,                  8,  38, 60, 18, 0);
        MK("EDIT",   g->cfg->default_dir[0]?g->cfg->default_dir:"",
                             ES_AUTOHSCROLL|WS_BORDER,72, 36,320, 22, 102);
        MK("BUTTON","...",   BS_PUSHBUTTON,          398, 36, 54, 22, 103);
        MK("STATIC","Segments:",0,                    8,  66, 60, 18, 0);
        char sb[8]; snprintf(sb,sizeof(sb),"%u",g->cfg->default_segments);
        MK("EDIT",sb,ES_NUMBER|ES_AUTOHSCROLL|WS_BORDER,72,64,50,22,104);
        MK("BUTTON","Download",BS_DEFPUSHBUTTON,      8,  96,100,26,IDOK);
        MK("BUTTON","Cancel",  BS_PUSHBUTTON,        116, 96, 80,26,IDCANCEL);
#undef MK
        return 0;
    }
    case WM_COMMAND:{
        WORD cmd=LOWORD(wp);
        if(cmd==IDCANCEL){DestroyWindow(hwnd);return 0;}
        if(cmd==103){
            BROWSEINFOA bi={0};bi.hwndOwner=hwnd;
            bi.lpszTitle="Select folder";
            bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST p=SHBrowseForFolderA(&bi);
            if(p){char pa[MAX_PATH];
                  if(SHGetPathFromIDListA(p,pa))SetDlgItemTextA(hwnd,102,pa);
                  CoTaskMemFree(p);}
            return 0;
        }
        if(cmd==IDOK){
            GetDlgItemTextA(hwnd,101,g_add_res.url,sizeof(g_add_res.url));
            GetDlgItemTextA(hwnd,102,g_add_res.path,sizeof(g_add_res.path));
            char sb[8]; GetDlgItemTextA(hwnd,104,sb,sizeof(sb));
            g_add_res.segs=(uint32_t)atoi(sb);
            if(!g_add_res.segs||g_add_res.segs>32)g_add_res.segs=8;

            /* نام فایل از URL */
            if(g_add_res.url[0]){
                const char *sl=strrchr(g_add_res.url,'/');
                char fn[256]={0};
                if(sl&&*(sl+1))strncpy(fn,sl+1,255);
                else strcpy(fn,"download");
                char *q=strchr(fn,'?');if(q)*q='\0';
                if(!fn[0])strcpy(fn,"download");

                char fullpath[MAX_PATH*2] = {0};
                if(g_add_res.path[0])
                    safe_path_combine(fullpath, sizeof(fullpath), g_add_res.path, fn);
                else {
                    GetTempPathA(sizeof(fullpath), fullpath);
                    strncat(fullpath, fn, sizeof(fullpath)-strlen(fullpath)-1);
                }

                core_add(g_guo->core, g_add_res.url, fullpath,
                         g_add_res.segs, 0);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        if(g_guo) g_guo->add_hwnd=NULL;
        return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

/* ======================================================
   Verify Checksum Dialog (modeless)
   ====================================================== */
#define WM_APP_CHECKSUM_DONE (WM_APP + 1)
#define IDC_CKSUM_ALGO   7001
#define IDC_CKSUM_PATH   7002
#define IDC_CKSUM_EXPECT 7003
#define IDC_CKSUM_VERIFY 7004
#define IDC_CKSUM_RESULT 7005
#define IDC_CKSUM_STATUS 7006

typedef struct { char path[MAX_PATH*2]; } ChecksumCtx;

typedef struct {
    HWND hwnd;
    char path[MAX_PATH*2];
    ChecksumAlgo algo;
} ChecksumThreadCtx;

static DWORD WINAPI checksum_thread_proc(LPVOID param) {
    ChecksumThreadCtx *tc = (ChecksumThreadCtx *)param;
    char *hex = (char *)malloc(CHECKSUM_HEX_MAXLEN);
    BOOL ok = FALSE;
    if (hex) {
        ok = checksum_compute_file(tc->path, tc->algo, hex);
        if (!ok) hex[0] = '\0';
    }
    /* اگه دیالوگ قبل از تموم شدن محاسبه بسته شده باشه، این
       PostMessage فقط بی‌صدا fail می‌شه (HWND دیگه معتبر نیست) —
       نه crash، فقط همون malloc کوچیک هدر می‌ره که مشکلی نیست. */
    PostMessageA(tc->hwnd, WM_APP_CHECKSUM_DONE, (WPARAM)ok, (LPARAM)hex);
    free(tc);
    return 0;
}

static LRESULT CALLBACK checksum_wnd_proc(HWND hwnd, UINT msg,
                                            WPARAM wp, LPARAM lp) {
    ChecksumCtx *ctx = (ChecksumCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        ctx = (ChecksumCtx *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

#define MK(cls,t,sty,x,y,w,hh,id) {HWND h=CreateWindowExA(0,cls,t,\
        WS_CHILD|WS_VISIBLE|(sty),x,y,w,hh,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
        SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}

        MK("STATIC","File:",0,8,10,40,18,0)
        MK("STATIC",ctx->path,SS_PATHELLIPSIS|SS_NOPREFIX,52,10,412,18,IDC_CKSUM_PATH)

        MK("STATIC",i18n_str(STR_CHECKSUM_ALGO),0,8,40,90,18,0)
        HWND hCombo = CreateWindowExA(0,"COMBOBOX","",
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_TABSTOP,
            100,37,120,120,hwnd,(HMENU)(UINT_PTR)IDC_CKSUM_ALGO,NULL,NULL);
        SendMessageA(hCombo,WM_SETFONT,(WPARAM)fnt,TRUE);
        SendMessageA(hCombo,CB_ADDSTRING,0,(LPARAM)"MD5");
        SendMessageA(hCombo,CB_ADDSTRING,0,(LPARAM)"SHA-1");
        SendMessageA(hCombo,CB_ADDSTRING,0,(LPARAM)"SHA-256");
        SendMessageA(hCombo,CB_SETCURSEL,CHECKSUM_SHA256,0);

        MK("STATIC",i18n_str(STR_CHECKSUM_EXPECTED),0,8,70,140,18,0)
        MK("EDIT","",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,150,68,314,22,IDC_CKSUM_EXPECT)

        MK("BUTTON",i18n_str(STR_CHECKSUM_VERIFY_BTN),BS_DEFPUSHBUTTON|WS_TABSTOP,8,100,100,26,IDC_CKSUM_VERIFY)

        MK("STATIC",i18n_str(STR_CHECKSUM_COMPUTED),0,8,136,140,18,0)
        MK("EDIT","",ES_AUTOHSCROLL|ES_READONLY|WS_BORDER,150,134,314,22,IDC_CKSUM_RESULT)

        MK("STATIC","",0,8,164,456,20,IDC_CKSUM_STATUS)

        MK("BUTTON","Close",BS_PUSHBUTTON|WS_TABSTOP,8,196,100,26,IDCANCEL)
#undef MK
        return 0;
    }

    case WM_COMMAND: {
        WORD cmd = LOWORD(wp);
        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        if (cmd == IDC_CKSUM_VERIFY) {
            if (!ctx) return 0;
            HWND hCombo = GetDlgItem(hwnd, IDC_CKSUM_ALGO);
            int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
            if (sel < 0) sel = CHECKSUM_SHA256;

            SetDlgItemTextA(hwnd, IDC_CKSUM_RESULT, "");
            SetDlgItemTextA(hwnd, IDC_CKSUM_STATUS, i18n_str(STR_CHECKSUM_COMPUTING));
            EnableWindow(GetDlgItem(hwnd, IDC_CKSUM_VERIFY), FALSE);

            ChecksumThreadCtx *tc = (ChecksumThreadCtx *)malloc(sizeof(*tc));
            if (tc) {
                tc->hwnd = hwnd;
                strncpy(tc->path, ctx->path, sizeof(tc->path)-1);
                tc->path[sizeof(tc->path)-1] = '\0';
                tc->algo = (ChecksumAlgo)sel;
                HANDLE th = CreateThread(NULL, 0, checksum_thread_proc, tc, 0, NULL);
                if (th) CloseHandle(th);
                else free(tc);
            }
            return 0;
        }
        return 0;
    }

    case WM_APP_CHECKSUM_DONE: {
        BOOL ok = (BOOL)wp;
        char *hex = (char *)lp;
        EnableWindow(GetDlgItem(hwnd, IDC_CKSUM_VERIFY), TRUE);

        if (!ok || !hex || !hex[0]) {
            SetDlgItemTextA(hwnd, IDC_CKSUM_STATUS,
                "Could not read the file to compute its checksum.");
            if (hex) free(hex);
            return 0;
        }

        SetDlgItemTextA(hwnd, IDC_CKSUM_RESULT, hex);

        char expected[CHECKSUM_HEX_MAXLEN] = {0};
        GetDlgItemTextA(hwnd, IDC_CKSUM_EXPECT, expected, sizeof(expected));
        /* فاصله‌ی احتمالی اول/آخر (کپی‌پیست از یه صفحه‌ی وب) رو حذف کن */
        char *s = expected; while (*s == ' ') s++;
        char *e = s + strlen(s); while (e > s && *(e-1) == ' ') *(--e) = '\0';

        if (s[0]) {
            BOOL match = (_stricmp(s, hex) == 0);
            SetDlgItemTextA(hwnd, IDC_CKSUM_STATUS,
                match ? i18n_str(STR_CHECKSUM_MATCH)
                      : i18n_str(STR_CHECKSUM_MISMATCH));
        } else {
            SetDlgItemTextA(hwnd, IDC_CKSUM_STATUS, "");
        }
        free(hex);
        return 0;
    }

    case WM_DESTROY:
        if (ctx) free(ctx);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_checksum_dialog(GUO_Context *g, const char *filepath) {
    if (!g || !filepath || !filepath[0]) return;

    static BOOL class_registered = FALSE;
    if (!class_registered) {
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpszClassName = "LassChecksumDlg";
        wc.lpfnWndProc = checksum_wnd_proc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hInstance = g->hinstance;
        RegisterClassExA(&wc);
        class_registered = TRUE;
    }

    ChecksumCtx *ctx = (ChecksumCtx *)malloc(sizeof(*ctx));
    if (!ctx) return;
    strncpy(ctx->path, filepath, sizeof(ctx->path)-1);
    ctx->path[sizeof(ctx->path)-1] = '\0';

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME, "LassChecksumDlg",
        i18n_str(STR_CHECKSUM_TITLE),
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 490, 260,
        g->main_wnd, NULL, g->hinstance, ctx);
    if (!dlg) { free(ctx); return; }
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}

/* forward declaration — تعریف کامل پایین‌تر، بعد از batch dialog */
static void show_speed_sched_dialog(GUO_Context *g);

/* ======================================================
   Settings Dialog (modeless WndProc)
   ====================================================== */
static LRESULT CALLBACK settings_wnd_proc(HWND hwnd, UINT msg,
                                            WPARAM wp, LPARAM lp) {
    (void)lp;
    HFONT fnt=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
    GUO_Context *g=g_guo;

    switch(msg){
    case WM_CREATE:{
        int y=8;
#define SL(t,x,_y,w) do{HWND h=CreateWindowExA(0,"STATIC",t,WS_CHILD|WS_VISIBLE,\
    x,_y,w,17,hwnd,NULL,NULL,NULL);SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)
#define SE(id,x,_y,w,t) do{HWND h=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",t,\
    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,x,_y,w,21,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)
#define SC(id,t,x,_y,c) do{HWND h=CreateWindowExA(0,"BUTTON",t,\
    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,_y,310,19,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);\
    SendMessageA(h,BM_SETCHECK,(c)?BST_CHECKED:BST_UNCHECKED,0);}while(0)
#define SG(t,_y,h2) do{HWND h=CreateWindowExA(0,"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_GROUPBOX,\
    4,_y,454,h2,hwnd,NULL,NULL,NULL);SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)
#define SB(id,t,x,_y,w,def) do{HWND h=CreateWindowExA(0,"BUTTON",t,WS_CHILD|WS_VISIBLE|\
    (def?BS_DEFPUSHBUTTON:BS_PUSHBUTTON),x,_y,w,24,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)

        char buf[32];
        SG(i18n_str(STR_PREFS_GENERAL),y,92); y+=16;
        SL(i18n_str(STR_PREFS_MAX_DL),12,y,180);
        snprintf(buf,sizeof(buf),"%u",g->cfg->max_concurrent);
        SE(IDC_SET_MAXDL,196,y,60,buf); y+=24;
        SL(i18n_str(STR_PREFS_SEGMENTS),12,y,200);
        snprintf(buf,sizeof(buf),"%u",g->cfg->default_segments);
        SE(IDC_SET_SEGS,216,y,60,buf); y+=24;
        SL(i18n_str(STR_PREFS_DIR),12,y,120); y+=18;
        SE(IDC_SET_DIR,12,y,360,g->cfg->default_dir);
        SB(IDC_SET_BROWSE,"...",376,y,58,FALSE); y+=26;

        /* باگ واقعی که اینجا فیکس شد: g->cfg->run_on_startup فقط
           «نیت» ذخیره‌شده در lass.ini هست، نه وضعیت واقعی
           HKCU\...\Run — قبلاً این دوتا هیچ‌وقت با هم چک نمی‌شدن،
           پس اگه registry entry به هر دلیلی از بین می‌رفت، این
           چک‌باکس بی‌صدا «فعال» نشون می‌داد درحالی‌که برنامه دیگه
           واقعاً startup اجرا نمی‌شد. الان هر بار این دیالوگ باز
           میشه، حقیقتِ registry رو می‌خونیم و اگه با چیزی که فکر
           می‌کردیم فرق داشت، هم چک‌باکس هم cfg رو با واقعیت
           هماهنگ می‌کنیم (و لاگ می‌کنیم) — کاربر همیشه وضعیت واقعی
           رو می‌بینه، نه یه نیت قدیمی. */
        BOOL startup_actual = settings_get_startup();
        if (startup_actual != g->cfg->run_on_startup) {
            LOGW(LOG_MOD_SETTINGS,
                 "startup checkbox disagreed with registry (ini=%d, actual=%d) — "
                 "showing actual state",
                 g->cfg->run_on_startup, startup_actual);
            g->cfg->run_on_startup = startup_actual;
        }

        SG(i18n_str(STR_PREFS_BEHAVIOR),y,100); y+=16;
        SC(IDC_SET_TRAY,   i18n_str(STR_PREFS_TRAY),    12,y,g->cfg->minimize_to_tray);  y+=22;
        SC(IDC_SET_CLIP,   i18n_str(STR_PREFS_CLIP),    12,y,g->cfg->clipboard_monitor); y+=22;
        SC(IDC_SET_BASKET, i18n_str(STR_PREFS_BASKET),  12,y,g->cfg->basket_visible);    y+=22;
        SC(IDC_SET_STARTUP,i18n_str(STR_PREFS_STARTUP), 12,y,g->cfg->run_on_startup);    y+=20;

        SG(i18n_str(STR_PREFS_PROXY_GROUP),y,58); y+=16;
        SC(IDC_SET_PROXY,i18n_str(STR_PREFS_PROXY_ENABLE),12,y,g->cfg->proxy_enabled);
        /* قبلاً وقتی proxy_url خالی بود، متن نمونه‌ی
           "http://host:8080" به‌صورت متن واقعی و قابل‌ارسال توی
           فیلد گذاشته می‌شد (نه یه placeholder واقعی که با تایپ
           محو بشه) — یعنی اگه کاربر فقط تیک رو می‌زد و بدون
           دست‌زدن به فیلد Save می‌کرد، دقیقاً همون متن نمونه به
           عنوان proxy واقعی ذخیره می‌شد (host="host" واقعی!).
           فیلد رو واقعاً خالی می‌ذاریم — راهنمای فرمت همین‌جا
           پایین‌تر به‌صورت متن ثابت (SL) هست. */
        SE(IDC_SET_PROXYURL,110,y,324, g->cfg->proxy_url); y+=22;
        SL(i18n_str(STR_PREFS_PROXY_HINT),12,y,440); y+=22;

        SG(i18n_str(STR_PREFS_BROWSER_EXT),y,52); y+=16;
        char est[120];
        snprintf(est,sizeof(est),"Chrome: %s  Firefox: %s  Edge: %s",
            browser_ext_is_registered(BROWSER_CHROME) ?"OK":"--",
            browser_ext_is_registered(BROWSER_FIREFOX)?"OK":"--",
            browser_ext_is_registered(BROWSER_EDGE)   ?"OK":"--");
        SL(est,12,y,340); y+=18;
        SB(IDC_SET_EXT_REG,"Register All",12, y,110,FALSE);
        SB(IDC_SET_UNREG,  "Unregister", 130,y, 90,FALSE);
        SB(IDC_SET_EXT_TEST,"Test Connection", 228,y,120,FALSE); y+=28;

        SG(i18n_str(STR_PREFS_SPEED_GROUP),y,40); y+=16;
        SB(IDC_SET_SPEED_SCHED,i18n_str(STR_PREFS_SPEED_SCHED_BTN),12,y,150,FALSE); y+=28;

        SB(IDOK,    i18n_str(STR_PREFS_SAVE),  12, y,120,TRUE);
        SB(IDCANCEL,i18n_str(STR_GENERIC_CANCEL),140,y, 80,FALSE);

        char ini[MAX_PATH],lbl[MAX_PATH+12];
        settings_get_ini_path(ini,sizeof(ini));
        snprintf(lbl,sizeof(lbl),"INI: %s",ini);
        SL(lbl,4,y+28,454);
#undef SL
#undef SE
#undef SC
#undef SG
#undef SB
        return 0;
    }
    case WM_COMMAND:{
        WORD cmd=LOWORD(wp);
        switch(cmd){
        case IDC_SET_BROWSE:{
            BROWSEINFOA bi={0};bi.hwndOwner=hwnd;
            bi.lpszTitle="Select download folder";
            bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST p=SHBrowseForFolderA(&bi);
            if(p){char pa[MAX_PATH];
                  if(SHGetPathFromIDListA(p,pa))SetDlgItemTextA(hwnd,IDC_SET_DIR,pa);
                  CoTaskMemFree(p);}
            return 0;
        }
        case IDC_SET_EXT_REG:{
            BOOL ok=browser_ext_register(BROWSER_ALL);
            MessageBoxA(hwnd,ok?"Registered!\nLoad the extension manually from the "
                              "extensions/ folder — see extensions/INSTALL.md for "
                              "step-by-step instructions (this extension is not "
                              "published on any store)."
                              :"Failed. Try as Administrator.","Extension",MB_ICONINFORMATION);
            return 0;
        }
        case IDC_SET_UNREG:
            browser_ext_unregister(BROWSER_ALL);
            MessageBoxA(hwnd,"Unregistered.","Extension",MB_ICONINFORMATION);
            return 0;
        case IDC_SET_EXT_TEST:{
            IDM_PluginHooks *ext = plugins_get_browser_ext();
            char msg_chrome[768], msg_firefox[768], combined[1600];
            browser_ext_diagnose(ext, BROWSER_CHROME,  msg_chrome,  sizeof(msg_chrome));
            browser_ext_diagnose(ext, BROWSER_FIREFOX, msg_firefox, sizeof(msg_firefox));
            snprintf(combined, sizeof(combined), "%s\n\n%s", msg_chrome, msg_firefox);
            MessageBoxA(hwnd, combined, "Extension Connection Test", MB_ICONINFORMATION);
            return 0;
        }
        case IDC_SET_SPEED_SCHED:
            show_speed_sched_dialog(g_guo);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd); return 0;
        case IDOK:{
#define GCH(id) (SendMessageA(GetDlgItem(hwnd,(id)),BM_GETCHECK,0,0)==BST_CHECKED)
            char tb[MAX_PATH];
            GetDlgItemTextA(hwnd,IDC_SET_MAXDL,tb,sizeof(tb));
            {uint32_t v=(uint32_t)atoi(tb);if(v>=1&&v<=20)g->cfg->max_concurrent=v;}
            GetDlgItemTextA(hwnd,IDC_SET_SEGS,tb,sizeof(tb));
            {uint32_t v=(uint32_t)atoi(tb);if(v>=1&&v<=32)g->cfg->default_segments=v;}
            GetDlgItemTextA(hwnd,IDC_SET_DIR,g->cfg->default_dir,sizeof(g->cfg->default_dir));
            GetDlgItemTextA(hwnd,IDC_SET_PROXYURL,g->cfg->proxy_url,sizeof(g->cfg->proxy_url));
            g->cfg->clipboard_monitor=GCH(IDC_SET_CLIP);
            g->cfg->basket_visible   =GCH(IDC_SET_BASKET);
            g->cfg->minimize_to_tray =GCH(IDC_SET_TRAY);
            g->cfg->proxy_enabled    =GCH(IDC_SET_PROXY);
            BOOL ns=GCH(IDC_SET_STARTUP);
#undef GCH
            /* اگه "Enable proxy" تیک خورده ولی فیلد URL خالیه، proxy
               واقعاً هیچ‌وقت اعمال نمی‌شه (core_set_proxy فقط وقتی
               enabled=TRUE می‌ده که هم proxy_enabled هم proxy_url
               غیرخالی باشن) — قبلاً این حالت کاملاً ساکت بود: تیک
               ذخیره می‌شد، ولی proxy عملاً خاموش می‌موند، بدون هیچ
               اطلاع‌رسانی‌ای به کاربر. */
            {
                char trimmed[512];
                strncpy(trimmed, g->cfg->proxy_url, sizeof(trimmed) - 1);
                trimmed[sizeof(trimmed) - 1] = '\0';
                char *p = trimmed;
                while (*p == ' ' || *p == '\t') p++;
                if (g->cfg->proxy_enabled && p[0] == '\0') {
                    MessageBoxA(hwnd,
                        "Enable proxy is checked, but no proxy URL is set.\n"
                        "Proxy will stay OFF until you enter a URL "
                        "(e.g. http://host:8080 or socks5://host:port).",
                        "Lass - Proxy", MB_OK | MB_ICONWARNING);
                }
            }
            BOOL os=g->cfg->run_on_startup;
            g->cfg->run_on_startup=ns;
            if(ns!=os) settings_set_startup(ns);

            core_settings_apply(g->core, g->cfg);

            settings_save_window(g->cfg, g->main_wnd);
            settings_save(g->cfg);
            DestroyWindow(hwnd);
            return 0;
        }
        }
        break;
    }
    case WM_DESTROY:
        if(g) g->settings_hwnd=NULL;
        return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

/* ======================================================
   Batch Dialog
   ====================================================== */
/* ======================================================
   Speed Schedule Dialog — تنظیم سرعت بر اساس ساعت
   طراحی ساده: کاربر یه بازه‌ی "ساعات شلوغ" با یه سقف سرعت
   تعریف می‌کنه، بقیه‌ی ساعت‌ها به‌صورت پیش‌فرض نامحدوده.
   ====================================================== */
#define IDC_SS_ENABLED   6101
#define IDC_SS_FROM      6102
#define IDC_SS_TO        6103
#define IDC_SS_LIMIT     6104

static LRESULT CALLBACK speed_sched_wnd_proc(HWND hwnd, UINT msg,
                                               WPARAM wp, LPARAM lp) {
    (void)lp;
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        IDM_PluginHooks *ss = plugins_get_speed_scheduler();
        SpeedSchedConfig cfg = {0};
        if (ss) speed_sched_get_config(ss, &cfg);

        int from = 8, to = 18;
        uint64_t limit_kb = 512;
        BOOL found_range = FALSE;
        for (int h = 0; h < SPEED_SCHED_HOURS; h++) {
            if (cfg.limit_per_hour[h] > 0) {
                if (!found_range) { from = h; limit_kb = cfg.limit_per_hour[h] / 1024; }
                to = h;
                found_range = TRUE;
            }
        }

#define SL(t,x,_y,w) do{HWND h=CreateWindowExA(0,"STATIC",t,WS_CHILD|WS_VISIBLE,\
    x,_y,w,17,hwnd,NULL,NULL,NULL);SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)
#define SE(id,x,_y,w,t) do{HWND h=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",t,\
    WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_NUMBER,x,_y,w,21,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)
#define SC(id,t,x,_y,c) do{HWND h=CreateWindowExA(0,"BUTTON",t,\
    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,_y,300,19,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);\
    SendMessageA(h,BM_SETCHECK,(c)?BST_CHECKED:BST_UNCHECKED,0);}while(0)
#define SB(id,t,x,_y,w,def) do{HWND h=CreateWindowExA(0,"BUTTON",t,WS_CHILD|WS_VISIBLE|\
    (def?BS_DEFPUSHBUTTON:BS_PUSHBUTTON),x,_y,w,24,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}while(0)

        int y = 10;
        SL(i18n_str(STR_SPEEDSCHED_DESC),10,y,360); y += 24;
        SC(IDC_SS_ENABLED,i18n_str(STR_SPEEDSCHED_ENABLE),10,y,found_range && cfg.enabled); y += 26;

        char buf[16];
        SL(i18n_str(STR_SPEEDSCHED_FROM),10,y,120);
        snprintf(buf,sizeof(buf),"%d",from);
        SE(IDC_SS_FROM,140,y,50,buf); y += 26;

        SL(i18n_str(STR_SPEEDSCHED_TO),10,y,120);
        snprintf(buf,sizeof(buf),"%d",to);
        SE(IDC_SS_TO,140,y,50,buf); y += 26;

        SL(i18n_str(STR_SPEEDSCHED_LIMIT),10,y,120);
        snprintf(buf,sizeof(buf),"%llu",(unsigned long long)limit_kb);
        SE(IDC_SS_LIMIT,140,y,70,buf); y += 30;

        SL(i18n_str(STR_SPEEDSCHED_OUTSIDE),10,y,360); y += 28;

        SB(IDOK,    i18n_str(STR_GENERIC_SAVE),  10, y,90,TRUE);
        SB(IDCANCEL,i18n_str(STR_GENERIC_CANCEL),110,y,80,FALSE);

#undef SL
#undef SE
#undef SC
#undef SB
        return 0;
    }

    case WM_COMMAND: {
        WORD cmd = LOWORD(wp);
        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        if (cmd == IDOK) {
            IDM_PluginHooks *ss = plugins_get_speed_scheduler();
            if (ss) {
                char buf[16];
                BOOL enabled = (IsDlgButtonChecked(hwnd, IDC_SS_ENABLED) == BST_CHECKED);

                GetDlgItemTextA(hwnd, IDC_SS_FROM, buf, sizeof(buf));
                int from = atoi(buf);
                GetDlgItemTextA(hwnd, IDC_SS_TO, buf, sizeof(buf));
                int to = atoi(buf);
                GetDlgItemTextA(hwnd, IDC_SS_LIMIT, buf, sizeof(buf));
                uint64_t limit_bps = (uint64_t)_atoi64(buf) * 1024;

                if (from < 0) from = 0;
                if (from > 23) from = 23;
                if (to < 0) to = 0;
                if (to > 23) to = 23;
                if (limit_bps == 0) limit_bps = 512 * 1024;

                SpeedSchedConfig cfg = {0};
                cfg.enabled = enabled;
                speed_sched_set_range(&cfg, from, to, limit_bps);
                speed_sched_set_config(ss, &cfg);

                MessageBoxA(hwnd,
                    enabled ? "Speed schedule saved and active."
                            : "Speed schedule saved (disabled).",
                    "Lass", MB_ICONINFORMATION);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ======================================================
   Batch Dialog
   ======================================================
   مشکل گزارش‌شده: کاربر لینک‌ها رو از مرورگر کپی می‌کرد، دیالوگ Batch
   رو باز می‌کرد، ولی هیچ‌وقت نمی‌تونست paste کنه (حتی Ctrl+V) و فقط
   «No valid URLs found» می‌دید. چند ضعف واقعی در این دیالوگ بود:

   ۱) کنترل Edit چندخطی با CreateWindow ساخته میشه و سقف پیش‌فرض متنش
      حدود ۳۲۷۶۷ کاراکتره؛ paste یه لیست بلند (مثلاً ۲۰۰ لینک توکن‌دار)
      از این سقف رد میشه و *کل paste بی‌صدا رد میشه* (فقط یه beep).
      → EM_SETLIMITTEXT روی ۱۶ مگابایت.
   ۲) پنجره یه پنجره‌ی معمولیه نه dialog؛ فوکوس صفحه‌کلید هیچ‌وقت
      به Edit داده نمی‌شد و Tab/Esc/Ctrl+A کار نمی‌کرد.
      → فوکوس اولیه، IsDialogMessage در حلقه‌ی اصلی، و subclass برای Ctrl+A.
   ۳) دیالوگ هیچ‌وقت از کلیپ‌بورد چیزی نمی‌خوند و پارسر فقط خطوط
      دقیقاً-شروع-با-http رو قبول می‌کرد.
      → پرشدن خودکار از کلیپ‌بورد + دکمه‌ی «Paste from clipboard» که
        حتی اگه Ctrl+V به هر دلیلی کار نکنه جواب می‌ده + استخراج
        هوشمند لینک (engine_extract_urls) از هر متنی.
   ۴) هیچ لاگی نبود؛ اگه بازم مشکلی پیش بیاد، حالا lass.log دلیلش رو
      نشون میده (OpenClipboard شکست خورد؟ کلیپ‌بورد متن نداشت؟).
   ====================================================== */
#define IDC_BATCH_URLS    6001
#define IDC_BATCH_DIR     6002
#define IDC_BATCH_BROWSE  6003
#define IDC_BATCH_GROUP   6004
#define IDC_BATCH_PASTE   6005
#define IDC_BATCH_STATUS  6006
#define IDT_BATCH_DEBOUNCE 1 /* timer id — فقط داخل همین پنجره معنا داره */

typedef struct { GUO_Context *g; HWND hDir; HWND last_focus; } BatchCtx;

/* متن کلیپ‌بورد به UTF-8 (malloc). NULL اگه کلیپ‌بورد باز نشد یا متن نداشت.
   OpenClipboard وقتی برنامه‌ی دیگه‌ای (مرورگر، یا حتی خود پلاگین
   clipboard_monitor) چند میلی‌ثانیه کلیپ‌بورد رو باز نگه داشته شکست
   می‌خوره؛ روال استاندارد چند بار با فاصله‌ی کوتاه retry کردنه. */
static char *batch_clipboard_text(HWND owner) {
    BOOL  opened = FALSE;
    DWORD err = 0;
    for (int i = 0; i < 20 && !opened; i++) {
        if (OpenClipboard(owner)) opened = TRUE;
        else { err = GetLastError(); Sleep(25); }
    }
    if (!opened) {
        LOGW(LOG_MOD_GUO, "batch: OpenClipboard failed after retries (err=%lu)",
             (unsigned long)err);
        return NULL;
    }

    char *result = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            if (n > 0) {
                result = (char *)malloc((size_t)n);
                if (result && !WideCharToMultiByte(CP_UTF8, 0, w, -1, result, n, NULL, NULL)) {
                    free(result); result = NULL;
                }
            }
            GlobalUnlock(h);
        }
    } else if ((h = GetClipboardData(CF_TEXT)) != NULL) {
        const char *a = (const char *)GlobalLock(h);
        if (a) {
            size_t n = strlen(a);
            result = (char *)malloc(n + 1);
            if (result) memcpy(result, a, n + 1);
            GlobalUnlock(h);
        }
    } else {
        LOGI(LOG_MOD_GUO, "batch: clipboard has no text format (IsClipboardFormatAvailable"
             "(UNICODETEXT)=%d)", IsClipboardFormatAvailable(CF_UNICODETEXT) ? 1 : 0);
    }
    CloseClipboard();
    return result;
}

/* شمارنده‌ی زنده‌ی لینک‌های معتبر زیر کادر */
static void batch_update_status(HWND hwnd) {
    HWND ed = GetDlgItem(hwnd, IDC_BATCH_URLS);
    int  len = GetWindowTextLengthA(ed);
    char *t = (char *)malloc((size_t)len + 1);
    if (!t) return;
    GetWindowTextA(ed, t, len + 1);
    int n = engine_extract_urls(t, NULL, 0);
    free(t);
    char msg[128];
    if (n > 0) snprintf(msg, sizeof(msg), "%d valid link(s) detected", n);
    else       snprintf(msg, sizeof(msg),
                        "No links yet - paste with Ctrl+V or press \"Paste from clipboard\"");
    SetDlgItemTextA(hwnd, IDC_BATCH_STATUS, msg);
}

/* لینک‌های کلیپ‌بورد رو (یکتا، هر کدوم یک خط) به انتهای Edit اضافه می‌کنه.
   برمی‌گردونه: تعداد لینک اضافه‌شده؛ 0 = کلیپ‌بورد لینکی نداشت؛
   -1 = کلیپ‌بورد قابل خوندن نبود. */
static int batch_insert_clipboard_links(HWND edit) {
    char *txt = batch_clipboard_text(edit);
    if (!txt) return -1;

    size_t cap = strlen(txt) * 3 + 2;
    char *urls = (char *)malloc(cap);
    int   n = 0;
    if (urls) {
        n = engine_extract_urls(txt, urls, cap);
        if (n > 0) {
            /* '\n' → "\r\n" برای Edit */
            size_t ul = strlen(urls), nl = 0;
            for (size_t i = 0; i < ul; i++) if (urls[i] == '\n') nl++;
            char *crlf = (char *)malloc(ul + nl + 1);
            if (crlf) {
                size_t o = 0;
                for (size_t i = 0; i < ul; i++) {
                    if (urls[i] == '\n') crlf[o++] = '\r';
                    crlf[o++] = urls[i];
                }
                crlf[o] = '\0';

                /* اگه آخرین خط Edit خالی نیست، اول یه خط جدید بذار */
                int lc  = (int)SendMessageA(edit, EM_GETLINECOUNT, 0, 0);
                int idx = (int)SendMessageA(edit, EM_LINEINDEX, (WPARAM)(lc - 1), 0);
                int ll  = (int)SendMessageA(edit, EM_LINELENGTH, (WPARAM)idx, 0);
                int end = GetWindowTextLengthA(edit);
                SendMessageA(edit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
                if (ll > 0) SendMessageA(edit, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
                SendMessageA(edit, EM_REPLACESEL, TRUE, (LPARAM)crlf);
                free(crlf);
            }
        }
    }
    LOGI(LOG_MOD_GUO, "batch: pasted %d link(s) from clipboard (%zu chars of text)",
         n, strlen(txt));
    free(urls);
    free(txt);
    return n;
}

static WNDPROC g_batch_edit_orig = NULL;

/* subclass روی Edit: Ctrl+A (Edit چندخطی خودش پشتیبانی نمی‌کنه) و
   WM_PASTE — Ctrl+V، Shift+Insert و منوی راست‌کلیک همه به WM_PASTE
   می‌رسن. اگه کلیپ‌بورد لینک داشت تمیز و یکتا وارد میشه؛ وگرنه paste
   معمولی (مثلاً کاربر می‌خواد یه متن دیگه بچسبونه) انجام میشه. */
static LRESULT CALLBACK batch_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageA(h, EM_SETSEL, 0, (LPARAM)-1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == 1) return 0;                 /* Ctrl+A: بدون beep */
        break;
    case WM_PASTE:
        if (batch_insert_clipboard_links(h) > 0) return 0;
        break;                                /* لینکی نبود → paste پیش‌فرض */
    }
    return CallWindowProcA(g_batch_edit_orig, h, m, w, l);
}

static LRESULT CALLBACK batch_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    BatchCtx *ctx = (BatchCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        ctx = (BatchCtx *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

#define MK(cls,t,sty,x,y,w,hh,id) {HWND h=CreateWindowExA(0,cls,t,\
        WS_CHILD|WS_VISIBLE|(sty),x,y,w,hh,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
        SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}

        MK("STATIC","URLs - one per line (any text that contains links works):",0,8,8,360,17,0)
        MK("BUTTON","Paste from clipboard",BS_PUSHBUTTON|WS_TABSTOP,378,4,150,22,IDC_BATCH_PASTE)

        HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
            WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|
            WS_VSCROLL|WS_TABSTOP,
            8,30,520,168,hwnd,(HMENU)(UINT_PTR)IDC_BATCH_URLS,NULL,NULL);
        SendMessageA(ed,WM_SETFONT,(WPARAM)fnt,TRUE);
        /* سقف پیش‌فرض ~۳۲ هزار کاراکتره؛ بالاتر که بره paste بی‌صدا رد میشه */
        SendMessageA(ed,EM_SETLIMITTEXT,16u*1024u*1024u,0);
        if (!g_batch_edit_orig)
            g_batch_edit_orig = (WNDPROC)GetWindowLongPtrA(ed, GWLP_WNDPROC);
        SetWindowLongPtrA(ed, GWLP_WNDPROC, (LONG_PTR)batch_edit_proc);

        MK("STATIC","",0,8,202,520,17,IDC_BATCH_STATUS)
        MK("STATIC","Folder:",0,8,232,50,17,0)
        ctx->hDir = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",
            ctx->g->cfg->default_dir[0]?ctx->g->cfg->default_dir:"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,62,228,380,22,
            hwnd,(HMENU)(UINT_PTR)IDC_BATCH_DIR,NULL,NULL);
        SendMessageA(ctx->hDir,WM_SETFONT,(WPARAM)fnt,TRUE);
        MK("BUTTON","...",BS_PUSHBUTTON|WS_TABSTOP,446,228,64,22,IDC_BATCH_BROWSE)
        MK("STATIC","Group:",0,8,262,50,17,0)
        {   HWND ge = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","Batch",
                WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,62,258,160,22,
                hwnd,(HMENU)(UINT_PTR)IDC_BATCH_GROUP,NULL,NULL);
            SendMessageA(ge,WM_SETFONT,(WPARAM)fnt,TRUE); }
        MK("BUTTON","Add All",BS_DEFPUSHBUTTON|WS_TABSTOP,8,294,100,26,IDOK)
        MK("BUTTON","Cancel",BS_PUSHBUTTON|WS_TABSTOP,116,294,80,26,IDCANCEL)
#undef MK

        /* پر شدن خودکار از کلیپ‌بورد — دقیقاً چیزی که کاربر از
           «Add batch from clipboard» انتظار داره */
        int pre = batch_insert_clipboard_links(ed);
        (void)pre;
        batch_update_status(hwnd);
        ctx->last_focus = ed;
        return 0;
    }

    case WM_ACTIVATE:
        /* پنجره‌ی معمولی فوکوس رو به هیچ child ای نمیده؛ آخرین کنترلی که
           کاربر روش بود (یا اول Edit لینک‌ها) رو برگردون */
        if (ctx) {
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND f = GetFocus();
                if (f && IsChild(hwnd, f)) ctx->last_focus = f;
            } else if (ctx->last_focus && IsWindow(ctx->last_focus)) {
                SetFocus(ctx->last_focus);
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_BATCH_DEBOUNCE) {
            KillTimer(hwnd, IDT_BATCH_DEBOUNCE);
            batch_update_status(hwnd);
            return 0;
        }
        break;

    case WM_COMMAND: {
        if (!ctx) return 0;
        WORD cmd = LOWORD(wp);

        if (cmd == IDC_BATCH_URLS && HIWORD(wp) == EN_CHANGE) {
            /* debounce: به‌جای parse کردن کل بافر (که می‌تونه چند
               ده‌هزار کاراکتر باشه بعد از یه paste بزرگ) به‌ازای
               هر تک کاراکتر تایپ‌شده، ۳۰۰ms صبر می‌کنیم تا کاربر
               واقعاً مکث کنه. هر EN_CHANGE جدید timer رو ریست
               می‌کنه؛ batch_update_status واقعی فقط یه‌بار، توی
               WM_TIMER زیر، بعد از مکث صدا زده می‌شه. */
            SetTimer(hwnd, IDT_BATCH_DEBOUNCE, 300, NULL);
            return 0;
        }

        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }

        if (cmd == IDC_BATCH_PASTE) {
            HWND ed = GetDlgItem(hwnd, IDC_BATCH_URLS);
            int n = batch_insert_clipboard_links(ed);
            if (n < 0)
                MessageBoxA(hwnd, "Could not read the clipboard (another program may be "
                            "holding it). Try again in a moment.",
                            "Lass - Batch Add", MB_ICONWARNING);
            else if (n == 0)
                MessageBoxA(hwnd, "The clipboard does not contain any http:// or https:// "
                            "links.\nCopy the links first, then press this button.",
                            "Lass - Batch Add", MB_ICONINFORMATION);
            SetFocus(ed);
            return 0;
        }

        if (cmd == IDC_BATCH_BROWSE) {
            BROWSEINFOA bi = {0};
            bi.hwndOwner  = hwnd;
            bi.lpszTitle  = "Select folder";
            bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST p = SHBrowseForFolderA(&bi);
            if (p) {
                char pa[MAX_PATH];
                if (SHGetPathFromIDListA(p, pa)) SetWindowTextA(ctx->hDir, pa);
                CoTaskMemFree(p);
            }
            return 0;
        }

        if (cmd == IDOK) {
            int ulen = GetWindowTextLengthA(GetDlgItem(hwnd, IDC_BATCH_URLS)) + 1;
            char *urls = (char *)malloc((size_t)ulen + 1);
            if (urls) {
                GetDlgItemTextA(hwnd, IDC_BATCH_URLS, urls, ulen);
                char dir[MAX_PATH] = {0}, grp[64] = {0};
                GetWindowTextA(ctx->hDir, dir, sizeof(dir));
                GetDlgItemTextA(hwnd, IDC_BATCH_GROUP, grp, sizeof(grp));
                int added = core_batch_count(ctx->g->core, urls, dir, grp);
                free(urls);

                char msg[160];
                if (added > 0)
                    snprintf(msg, sizeof(msg),
                             "%d download(s) added to queue.", added);
                else
                    snprintf(msg, sizeof(msg),
                             "No valid links found.\nPaste links that start with "
                             "http:// or https:// (one per line, or separated by spaces).");
                MessageBoxA(hwnd, msg, "Lass - Batch Add",
                             added > 0 ? MB_ICONINFORMATION : MB_ICONWARNING);

                if (added > 0) DestroyWindow(hwnd);
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        if (ctx && ctx->g && ctx->g->batch_hwnd == hwnd) ctx->g->batch_hwnd = NULL;
        if (ctx) free(ctx);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_batch_dialog(GUO_Context *g) {
    if (!g) return;

    /* دیالوگ از قبل بازه: به‌جای ساخت نسخه‌ی دوم، همونو جلو بیار */
    if (g->batch_hwnd && IsWindow(g->batch_hwnd)) {
        ShowWindow(g->batch_hwnd, SW_SHOW);
        SetForegroundWindow(g->batch_hwnd);
        return;
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = "LassBatchWnd";
    wc.lpfnWndProc   = batch_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    RegisterClassExA(&wc); /* اگه قبلاً ثبت شده اشکالی نداره */

    BatchCtx *ctx = (BatchCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->g = g;

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "LassBatchWnd",
        "Add Batch URLs",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,544,380,
        g->main_wnd,NULL,GetModuleHandleA(NULL),ctx);
    if (!dlg) { free(ctx); return; }

    g->batch_hwnd = dlg;
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
}

/* ======================================================
   Download Sequence Dialog (modeless)
   الگوی کلاسیک IDM: یه URL با # به‌جای عدد، + بازه‌ی from..to.
   تعداد # های پشت‌سرهم = عرض zero-padding (مثلاً "img###.jpg"
   یعنی img001.jpg تا img999.jpg). از همون موتور Batch (که از
   قبل تست‌شده و کار می‌کنه) استفاده می‌کنه — فقط URLهای تولیدشده
   رو بهش می‌ده، دوباره منطق صف‌بندی رو پیاده نمی‌کنه.
   ====================================================== */
typedef struct { GUO_Context *g; HWND hDir; } SeqCtx;

static LRESULT CALLBACK seq_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp) {
    SeqCtx *ctx = (SeqCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        ctx = (SeqCtx *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

#define MK(cls,t,sty,x,y,w,hh,id) {HWND h=CreateWindowExA(0,cls,t,\
        WS_CHILD|WS_VISIBLE|(sty),x,y,w,hh,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
        SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}

        MK("STATIC",i18n_str(STR_SEQ_PATTERN),0,8,8,460,17,0)
        MK("EDIT","",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,8,26,520,22,7101)

        MK("STATIC",i18n_str(STR_SEQ_FROM),0,8,58,40,18,0)
        MK("EDIT","1",ES_NUMBER|ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,52,56,70,22,7102)
        MK("STATIC",i18n_str(STR_SEQ_TO),0,136,58,30,18,0)
        MK("EDIT","10",ES_NUMBER|ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,170,56,70,22,7103)

        MK("STATIC",i18n_str(STR_SEQ_FOLDER),0,8,88,50,18,0)
        ctx->hDir = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT",
            ctx->g->cfg->default_dir[0]?ctx->g->cfg->default_dir:"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,62,86,390,22,
            hwnd,(HMENU)(UINT_PTR)7104,NULL,NULL);
        SendMessageA(ctx->hDir,WM_SETFONT,(WPARAM)fnt,TRUE);
        MK("BUTTON","...",BS_PUSHBUTTON|WS_TABSTOP,456,86,64,22,7105)

        MK("STATIC",i18n_str(STR_SEQ_GROUP),0,8,118,50,18,0)
        MK("EDIT","Sequence",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,62,116,160,22,7106)

        MK("BUTTON",i18n_str(STR_SEQ_ADD_ALL),BS_DEFPUSHBUTTON|WS_TABSTOP,8,152,100,26,IDOK)
        MK("BUTTON","Cancel",BS_PUSHBUTTON|WS_TABSTOP,116,152,80,26,IDCANCEL)
#undef MK
        return 0;
    }

    case WM_COMMAND: {
        if (!ctx) return 0;
        WORD cmd = LOWORD(wp);
        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }

        if (cmd == 7105) {
            BROWSEINFOA bi = {0};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = "Select folder";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST p = SHBrowseForFolderA(&bi);
            if (p) {
                char pa[MAX_PATH];
                if (SHGetPathFromIDListA(p, pa)) SetWindowTextA(ctx->hDir, pa);
                CoTaskMemFree(p);
            }
            return 0;
        }

        if (cmd == IDOK) {
            char pattern[1024] = {0};
            GetDlgItemTextA(hwnd, 7101, pattern, sizeof(pattern));

            /* پیدا کردن اولین دنباله‌ی پشت‌سرهم از # */
            char *hstart = strchr(pattern, '#');
            if (!hstart) {
                MessageBoxA(hwnd, i18n_str(STR_SEQ_ERR_NO_HASH),
                            "Lass", MB_ICONWARNING);
                return 0;
            }
            int pad = 0;
            char *hend = hstart;
            while (*hend == '#') { pad++; hend++; }

            char fb[16]; GetDlgItemTextA(hwnd, 7102, fb, sizeof(fb));
            char tb[16]; GetDlgItemTextA(hwnd, 7103, tb, sizeof(tb));
            long from = atol(fb), to = atol(tb);

            if (from > to) {
                MessageBoxA(hwnd, i18n_str(STR_SEQ_ERR_RANGE),
                            "Lass", MB_ICONWARNING);
                return 0;
            }
            long count = to - from + 1;
            if (count > 2000) {
                MessageBoxA(hwnd, i18n_str(STR_SEQ_ERR_TOO_MANY),
                            "Lass", MB_ICONWARNING);
                return 0;
            }

            /* prefix/suffix دور # ها */
            size_t prefix_len = (size_t)(hstart - pattern);
            char prefix[1024] = {0}, suffix[1024] = {0};
            memcpy(prefix, pattern, prefix_len);
            strncpy(suffix, hend, sizeof(suffix)-1);

            /* هر URL می‌تونه تا ~1024+32 کاراکتر باشه؛ برای 2000 URL
               حداکثر بافر لازم حدود 2MB — malloc می‌کنیم، نه استک. */
            size_t cap = (size_t)count * 1100 + 64;
            char *urls = (char *)malloc(cap);
            if (!urls) { DestroyWindow(hwnd); return 0; }
            urls[0] = '\0';
            size_t off = 0;

            char numfmt[16];
            snprintf(numfmt, sizeof(numfmt), "%%0%dld", pad);

            for (long i = from; i <= to; i++) {
                char numbuf[32];
                snprintf(numbuf, sizeof(numbuf), numfmt, i);
                int n = snprintf(urls + off, cap - off, "%s%s%s\n",
                                  prefix, numbuf, suffix);
                if (n < 0 || (size_t)n >= cap - off) break; /* احتیاط، نباید برسه */
                off += (size_t)n;
            }

            char dir[MAX_PATH] = {0}, grp[64] = {0};
            GetWindowTextA(ctx->hDir, dir, sizeof(dir));
            GetDlgItemTextA(hwnd, 7106, grp, sizeof(grp));
            int added = core_batch_count(ctx->g->core, urls, dir, grp);
            free(urls);

            char msg[128];
            if (added > 0)
                snprintf(msg, sizeof(msg), "%d download(s) added to queue.", added);
            else
                snprintf(msg, sizeof(msg), "No valid URLs were generated.");
            MessageBoxA(hwnd, msg, "Lass - Download Sequence",
                         added > 0 ? MB_ICONINFORMATION : MB_ICONWARNING);

            if (added > 0) DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_DESTROY:
        if (ctx) free(ctx);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_seq_dialog(GUO_Context *g) {
    if (!g) return;

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = "LassSeqWnd";
    wc.lpfnWndProc   = seq_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    RegisterClassExA(&wc); /* اگه قبلاً ثبت شده اشکالی نداره */

    SeqCtx *ctx = (SeqCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->g = g;

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "LassSeqWnd",
        i18n_str(STR_SEQ_TITLE),
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,544,230,
        g->main_wnd,NULL,GetModuleHandleA(NULL),ctx);
    if (!dlg) { free(ctx); return; }

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
}

static void show_speed_sched_dialog(GUO_Context *g) {
    if (!g) return;

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = "LassSpeedSchedWnd";
    wc.lpfnWndProc   = speed_sched_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    RegisterClassExA(&wc);

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "LassSpeedSchedWnd",
        i18n_str(STR_SPEEDSCHED_TITLE),
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,400,230,
        g->main_wnd,NULL,GetModuleHandleA(NULL),NULL);
    if (!dlg) return;

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
}
/* ======================================================
   Schedule Dialog — WndProc مستقل (بدون modal loop دستی)
   ====================================================== */
typedef struct { GUO_Context *g; int dl_id; } SchedCtx;

static LRESULT CALLBACK sched_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    SchedCtx *ctx = (SchedCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);

        SYSTEMTIME st; GetLocalTime(&st);
        char hb[8], mb[8];
        snprintf(hb, sizeof(hb), "%d", st.wHour);
        snprintf(mb, sizeof(mb), "%d", st.wMinute);

#define MK(cls,t,sty,x,y,w,hh,id) {HWND h=CreateWindowExA(0,cls,t,\
    WS_CHILD|WS_VISIBLE|(sty),x,y,w,hh,hwnd,(HMENU)(UINT_PTR)(id),NULL,NULL);\
    SendMessageA(h,WM_SETFONT,(WPARAM)fnt,TRUE);}
        MK("STATIC", "Start download at:", 0,          10, 10, 200, 18, 0)
        MK("STATIC", "Hour (0-23):",        0,          10, 36,  90, 18, 0)
        MK("EDIT",   hb, ES_NUMBER|WS_BORDER|WS_TABSTOP,104, 34,  50, 22, 7001)
        MK("STATIC", "Minute (0-59):",      0,          10, 64, 100, 18, 0)
        MK("EDIT",   mb, ES_NUMBER|WS_BORDER|WS_TABSTOP,114, 62,  50, 22, 7002)
        MK("BUTTON", "Set",  BS_DEFPUSHBUTTON|WS_TABSTOP, 10, 96,  80, 26, IDOK)
        MK("BUTTON", "Cancel", BS_PUSHBUTTON|WS_TABSTOP,  98, 96,  80, 26, IDCANCEL)
#undef MK
        return 0;
    }

    case WM_COMMAND: {
        WORD cmd = LOWORD(wp);
        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        if (cmd == IDOK && ctx) {
            char hbuf[8]={0}, mbuf[8]={0};
            GetDlgItemTextA(hwnd, 7001, hbuf, sizeof(hbuf));
            GetDlgItemTextA(hwnd, 7002, mbuf, sizeof(mbuf));
            int h = atoi(hbuf), min = atoi(mbuf);
            if (h >= 0 && h <= 23 && min >= 0 && min <= 59) {
                core_schedule(ctx->g->core, (uint32_t)ctx->dl_id, h, min);
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "Download #%d scheduled for %02d:%02d",
                         ctx->dl_id, h, min);
                MessageBoxA(hwnd, msg, "Lass - Scheduled",
                             MB_ICONINFORMATION);
                DestroyWindow(hwnd);
            } else {
                MessageBoxA(hwnd, "Invalid time.\nHour: 0-23, Minute: 0-59",
                             "Error", MB_ICONERROR);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_DESTROY:
        if (ctx) free(ctx);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void guo_show_schedule(GUO_Context *g, int dl_id) {
    if (dl_id < 0 || !g) return;

    /* ثبت class */
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpszClassName = "LassSchedWnd";
    wc.lpfnWndProc   = sched_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hInstance     = GetModuleHandleA(NULL);
    RegisterClassExA(&wc); /* اگه قبلاً ثبت شده اشکالی نداره */

    SchedCtx *ctx = (SchedCtx *)malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->g     = g;
    ctx->dl_id = dl_id;

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "LassSchedWnd",
        "Schedule Download",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 280, 168,
        g->main_wnd, NULL,
        GetModuleHandleA(NULL), ctx);

    if (!dlg) { free(ctx); return; }

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
}

/* ======================================================
   Toolbar + ListView + Menubar
   ====================================================== */
/* ======================================================
   Category Filter Combo
   ====================================================== */
static HWND create_category_filter(HWND parent, HINSTANCE hi) {
    HWND combo = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", NULL,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_HASSTRINGS|WS_TABSTOP,
        0,0,0,0, parent, (HMENU)(UINT_PTR)IDC_CAT_FILTER, hi, NULL);
    if (!combo) return NULL;

    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(combo, WM_SETFONT, (WPARAM)fnt, TRUE);

    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"All Categories");
    IDM_PluginHooks *cat = plugins_get_categories();
    if (cat) {
        int cnt = categories_count(cat);
        for (int i = 0; i < cnt; i++)
            SendMessageA(combo, CB_ADDSTRING, 0,
                         (LPARAM)categories_name(cat, i));
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"Other");
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0); /* پیش‌فرض: All */
    return combo;
}

/* ── فیلتر کردن یه ردیف بر اساس category انتخابی ── */
static void apply_category_filter(GUO_Context *g) {
    if (!g || !g->listview) return;
    int sel = (int)SendMessageA(g->cat_combo, CB_GETCURSEL, 0, 0);
    /* sel: 0=All, 1..N=category index, N+1=Other */
    IDM_PluginHooks *cat = plugins_get_categories();
    int cnt = cat ? categories_count(cat) : 0;

    if (sel <= 0) g->cat_filter = -2;            /* All */
    else if (sel == cnt + 1) g->cat_filter = -1; /* Other */
    else g->cat_filter = sel - 1;                /* category index */

    /* باگ واقعی که اینجا فیکس شد: یه حلقه‌ی کامل روی همه‌ی ردیف‌های
       ListView بود که هیچ کاری نمی‌کرد — LVM_GETITEMA + core_get_item
       + categories_classify رو صدا می‌زد و نتیجه (show) رو با
       (void)show دور می‌ریخت. باقیمونده‌ی یه refactor قدیمی: منطق
       واقعی فیلتر همیشه توی rebuild زیر همین بوده (core_list_items +
       categories_classify دوباره). این حلقه فقط هر بار فیلتر عوض
       می‌شد (یا حتی فقط combo باز می‌شد) کار اضافه‌ی O(n) انجام
       می‌داد بدون هیچ اثری. ListView استاندارد امکان hide تکی ردیف
       نداره، پس روش واقعی همینه: کل لیست رو بر اساس فیلتر rebuild
       کن — که دقیقاً همون کاریه که زیر همین انجام می‌شه. */

    /* rebuild کامل لیست بر اساس فیلتر فعلی — heap چون LDM_Item بزرگه */
    ListView_DeleteAllItems(g->listview);
    LDM_Item *items = (LDM_Item *)calloc(256, sizeof(LDM_Item));
    if (!items) return;
    int n = core_list_items(g->core, items, 256);
    for (int i = 0; i < n; i++) {
        if (g->cat_filter == -2) { lv_add(g->listview, &items[i]); continue; }
        int idx = cat ? categories_classify(cat, items[i].url, items[i].filename) : -1;
        if (idx == g->cat_filter) lv_add(g->listview, &items[i]);
    }
    free(items);
}


/* ======================================================
   Toolbar
   ====================================================== */
static HWND create_toolbar(HWND parent, HINSTANCE hi) {
    InitCommonControls();
    HWND tb=CreateWindowExA(0,TOOLBARCLASSNAMEA,NULL,
        WS_CHILD|WS_VISIBLE|TBSTYLE_FLAT|TBSTYLE_TOOLTIPS|CCS_NODIVIDER,
        0,0,0,0,parent,(HMENU)(UINT_PTR)IDC_TOOLBAR,hi,NULL);
    SendMessageA(tb,TB_BUTTONSTRUCTSIZE,sizeof(TBBUTTON),0);

    /* ── آیکون‌های سفارشی (طراحی شیشه‌ای الهام‌گرفته از Vista Aero) ──
       ترتیب اینجا باید دقیقاً با ترتیب btns[] پایین‌تر یکی باشه —
       iBitmap هر دکمه دقیقاً همون index‌ای هست که این آیکون توی
       ImageList گرفته (0..7). */
    const int RESIDS[] = {
        IDI_TB_ADD, IDI_TB_PAUSE, IDI_TB_RESUME, IDI_TB_CANCEL,
        IDI_TB_REMOVE, IDI_TB_OPEN_FOLDER, IDI_TB_SPEED_GRAPH,
        IDI_TB_CLEAR_DONE,
    };
    const int N_ICONS = (int)(sizeof(RESIDS) / sizeof(RESIDS[0]));
    const int ICON_PX = 24;

    HIMAGELIST himl = ImageList_Create(ICON_PX, ICON_PX,
                                        ILC_COLOR32 | ILC_MASK, N_ICONS, 0);
    if (himl) {
        for (int i = 0; i < N_ICONS; i++) {
            HICON hicon = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(RESIDS[i]),
                IMAGE_ICON, ICON_PX, ICON_PX, LR_DEFAULTCOLOR);
            if (hicon) {
                ImageList_AddIcon(himl, hicon);
                /* LoadImage (برخلاف LoadIcon) مالکیت handle رو به caller
                   می‌ده — ImageList_AddIcon خودش یه کپی داخلی می‌سازه،
                   پس نسخه‌ی اصلی رو باید آزاد کنیم. */
                DestroyIcon(hicon);
            } else {
                /* اگه بارگذاری شکست خورد، یه slot خالی نگه می‌داریم تا
                   index بقیه‌ی آیکون‌ها جابه‌جا نشه. */
                ImageList_AddIcon(himl, LoadIconA(NULL, IDI_APPLICATION));
            }
        }
        SendMessageA(tb, TB_SETIMAGELIST, 0, (LPARAM)himl);
    }

    TBBUTTON btns[]={
        {0,ID_BTN_ADD,   TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_ADD)},
        {0,0,TBSTATE_ENABLED,BTNS_SEP,{0},0,0},
        {1,ID_BTN_PAUSE, TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_PAUSE)},
        {2,ID_BTN_RESUME,TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_RESUME)},
        {3,ID_BTN_CANCEL,TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_CANCEL)},
        {0,0,TBSTATE_ENABLED,BTNS_SEP,{0},0,0},
        {4,ID_BTN_REMOVE,TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_REMOVE)},
        {5,ID_BTN_OPEN,  TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_OPEN_FOLDER)},
        {0,0,TBSTATE_ENABLED,BTNS_SEP,{0},0,0},
        {6,ID_BTN_GRAPH, TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_SPEED_GRAPH)},
        {0,0,TBSTATE_ENABLED,BTNS_SEP,{0},0,0},
        {7,ID_BTN_CLEAR, TBSTATE_ENABLED,BTNS_AUTOSIZE|BTNS_BUTTON,{0},0,(INT_PTR)i18n_str(STR_TB_CLEAR_DONE)},
    };
    /* توجه: این جدول TB_ADDSTRINGA هیچ‌جا با index استفاده نمی‌شه —
       هر TBBUTTON بالا مستقیم pointer متن خودش رو داره (iString).
       این رشته‌ی ترکیبی به‌نظر می‌رسه از قبل dead بوده؛ دست‌نخورده
       گذاشتمش چون ترجمه‌ش (که استفاده نمی‌شه) ریسکی اضافه نمی‌کنه
       ولی هم زمان لازم نیست. */
    SendMessageA(tb,TB_ADDSTRINGA,0,(LPARAM)
        "Add URL\0Pause\0Resume\0Cancel\0Remove\0Open folder\0"
        "Speed Graph\0Basket\0Clip Monitor\0Clear Done\0\0");
    SendMessageA(tb,TB_ADDBUTTONSA,sizeof(btns)/sizeof(*btns),(LPARAM)btns);
    SendMessageA(tb,TB_AUTOSIZE,0,0);
    return tb;
}

static HWND create_listview(HWND parent, HINSTANCE hi) {
    HWND lv=CreateWindowExA(WS_EX_CLIENTEDGE,WC_LISTVIEWA,NULL,
        WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,
        0,0,0,0,parent,(HMENU)(UINT_PTR)IDC_LISTVIEW,hi,NULL);
    ListView_SetExtendedListViewStyle(lv,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);

    struct{const char*n;int w;}cols[]={
        {i18n_str(STR_COL_NUM),36},{i18n_str(STR_COL_FILENAME),220},
        {i18n_str(STR_COL_SIZE),75},{i18n_str(STR_COL_PROGRESS),70},
        {i18n_str(STR_COL_SPEED),80},{i18n_str(STR_COL_STATUS),90},
        {i18n_str(STR_COL_CATEGORY),80}};
    for(int i=0;i<COL_COUNT;i++){
        LVCOLUMNA c;
        memset(&c, 0, sizeof(c));
        c.mask     = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
        c.cx       = cols[i].w;
        c.pszText  = (char*)cols[i].n;
        c.iSubItem = i;
        SendMessageA(lv,LVM_INSERTCOLUMNA,(WPARAM)i,(LPARAM)&c);
    }
    return lv;
}

static HMENU create_menubar(void) {
    HMENU bar=CreateMenu();
    HMENU mF=CreatePopupMenu();
    AppendMenuA(mF,MF_STRING,IDM_FILE_ADD,    i18n_str(STR_MENU_ADD_URL));
    AppendMenuA(mF,MF_STRING,IDM_FILE_ADDGROUP,i18n_str(STR_MENU_ADD_BATCH));
    AppendMenuA(mF,MF_STRING,IDM_FILE_ADDSEQ,  i18n_str(STR_MENU_ADD_SEQ));
    AppendMenuA(mF,MF_SEPARATOR,0,NULL);
    AppendMenuA(mF,MF_STRING,IDM_FILE_EXIT,   i18n_str(STR_MENU_EXIT));
    AppendMenuA(bar,MF_POPUP,(UINT_PTR)mF,i18n_str(STR_MENU_FILE));

    HMENU mV=CreatePopupMenu();
    AppendMenuA(mV,MF_STRING,IDM_VIEW_GRAPH, i18n_str(STR_MENU_SPEED_GRAPH));
    AppendMenuA(mV,MF_STRING,IDM_VIEW_BASKET,i18n_str(STR_MENU_BASKET));
    AppendMenuA(bar,MF_POPUP,(UINT_PTR)mV,i18n_str(STR_MENU_VIEW));

    HMENU mS=CreatePopupMenu();
    AppendMenuA(mS,MF_STRING,IDM_SETTINGS_DLG,    i18n_str(STR_MENU_PREFERENCES));
    AppendMenuA(mS,MF_SEPARATOR,0,NULL);
    AppendMenuA(mS,MF_STRING,IDM_SETTINGS_CLIPMON, i18n_str(STR_MENU_CLIP_MONITOR));
    AppendMenuA(bar,MF_POPUP,(UINT_PTR)mS,i18n_str(STR_MENU_SETTINGS));

    HMENU mH=CreatePopupMenu();
    AppendMenuA(mH,MF_STRING,IDM_HELP_ABOUT,i18n_str(STR_MENU_ABOUT));
    AppendMenuA(bar,MF_POPUP,(UINT_PTR)mH,i18n_str(STR_MENU_HELP));
    return bar;
}

/* ======================================================
   Main WndProc
   ====================================================== */
static LRESULT CALLBACK main_wnd_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    GUO_Context *g=g_guo;

    switch(msg){
    case WM_SIZE:{
        RECT rc; GetClientRect(hwnd,&rc);
        int cw=rc.right, ch=rc.bottom;
        RECT tbr; GetWindowRect(g->toolbar,&tbr);
        int tbh=tbr.bottom-tbr.top;
        RECT sbr; GetWindowRect(g->statusbar,&sbr);
        int sbh=sbr.bottom-sbr.top;
        int fbh = g->cat_combo ? 28 : 0; /* ارتفاع نوار فیلتر */
        MoveWindow(g->toolbar,  0,0,   cw,tbh,TRUE);
        MoveWindow(g->statusbar,0,ch-sbh,cw,sbh,TRUE);
        if (g->cat_combo)
            MoveWindow(g->cat_combo, 4, tbh+4, 180, 22, TRUE);
        MoveWindow(g->listview, 0,tbh+fbh,  cw,ch-tbh-fbh-sbh,TRUE);
        SendMessageA(g->toolbar,TB_AUTOSIZE,0,0);
        return 0;
    }

    case WM_TIMER:
        if(wp==IDC_TIMER){
            LDM_Stats st=core_get_stats(g->core);
            char spd[32],buf[80];
            fmt_speed(st.total_speed_bps,spd,sizeof(spd));
            snprintf(buf,sizeof(buf),"  Active: %d  |  Speed: %s",
                     st.active_count,spd);
            SendMessageA(g->statusbar,SB_SETTEXTA,0,(LPARAM)buf);

            /* آپدیت همه ردیف‌های در حال دانلود */
            int cnt=ListView_GetItemCount(g->listview);
            for(int i=0;i<cnt;i++){
                LVITEMA li;
                memset(&li, 0, sizeof(li));
                li.mask  = LVIF_PARAM;
                li.iItem = i;
                SendMessageA(g->listview,LVM_GETITEMA,0,(LPARAM)&li);
                LDM_Item it;
                if(core_get_item(g->core,(uint32_t)li.lParam,&it))
                    if(it.state==DL_DOWNLOADING||it.state==DL_LAUNCHING)
                        lv_update(g->listview,&it);
            }
        }
        return 0;

    case WM_COMMAND:{
        WORD cmd=LOWORD(wp);

        /* combo box: تغییر فیلتر category */
        if (cmd == IDC_CAT_FILTER && HIWORD(wp) == CBN_SELCHANGE) {
            apply_category_filter(g);
            return 0;
        }

        switch(cmd){
        case IDM_FILE_ADD:
        case ID_BTN_ADD:
        case ID_TRAY_ADD:    guo_show_add_dialog(g); break;
        case IDM_FILE_ADDGROUP: show_batch_dialog(g); break;
        case IDM_FILE_ADDSEQ:   show_seq_dialog(g); break;
        /* باگ واقعی که اینجا فیکس شد: قبلاً این case به‌جای
           IDM_FILE_EXIT (که از قبل #define شده بود ولی هیچ‌جا
           استفاده نمی‌شد!)، از WM_QUIT (0x0012 — یه پیام سیستمی،
           نه یه menu ID) استفاده می‌کرد. فقط چون هیچ menu ID
           دیگه‌ای تصادفاً با 0x0012 برخورد نمی‌کرد کار می‌کرد —
           یه bomb واقعی برای هر تغییر آینده در شماره‌ی ID ها. */
        case IDM_FILE_EXIT:
        case ID_TRAY_EXIT:   tray_remove(g); DestroyWindow(hwnd); break;
        case IDM_SETTINGS_DLG: guo_show_settings(g); break;
        case IDM_HELP_ABOUT:
            MessageBoxA(hwnd,
                "Lass Download Manager v" LASS_VERSION_STRING "\n"
                "\n"
                "A multi-threaded download manager for Windows.\n"
                "Built with WinAPI and WinINet - no external dependencies.\n"
                "\n"
                "Architecture:\n"
                "  Engine + GUO/Core layer + 18 plugins\n"
                "  Segmented downloading, Smart Resume,\n"
                "  Video Sniffer, Browser Extension, .tels resilience\n"
                "\n"
                "AI Authorship Disclosure\n"
                "  Code generated with AI assistance (Claude by Anthropic).\n"
                "  Architecture, design decisions, and product direction\n"
                "  by the project author.\n"
                "  Disclosed in compliance with EU AI Act (Aug 2, 2026).\n"
                "  See AI_DISCLOSURE.md in the project root for details.\n"
                "\n"
                "License: Proprietary\n"
                "(c) 2025-2026  Lass Project",
                "About Lass Download Manager",MB_ICONINFORMATION);
            break;

        case ID_BTN_PAUSE:{
            int id=lv_selected_id(g->listview);
            if(id>=0) dispatch_async(g->core,(uint32_t)id,0);
            break;
        }
        case ID_BTN_RESUME:{
            int id=lv_selected_id(g->listview);
            if(id>=0) core_resume(g->core,(uint32_t)id);
            break;
        }
        case ID_BTN_CANCEL:{
            int id=lv_selected_id(g->listview);
            if(id>=0&&MessageBoxA(hwnd,"Cancel?","Confirm",MB_YESNO)==IDYES)
                dispatch_async(g->core,(uint32_t)id,1);
            break;
        }
        case ID_BTN_REMOVE:{
            int id=lv_selected_id(g->listview);
            if(id>=0&&MessageBoxA(hwnd,"Remove?","Confirm",MB_YESNO)==IDYES)
                dispatch_async(g->core,(uint32_t)id,2);
            break;
        }
        case ID_BTN_OPEN:{
            int id=lv_selected_id(g->listview);
            if(id>=0){
                LDM_Item it; core_get_item(g->core,(uint32_t)id,&it);
                char folder[MAX_PATH*2];
                strncpy(folder, it.out_path, sizeof(folder)-1);
                folder[sizeof(folder)-1] = '\0';
                char *bs=strrchr(folder,'\\');if(bs)*bs='\0';
                ShellExecuteA(hwnd,"open",folder,NULL,NULL,SW_SHOW);
            }
            break;
        }
        case ID_BTN_GRAPH:{
            IDM_PluginHooks *sg=plugins_get_speed_graph();
            if(sg) speed_graph_show(sg,!speed_graph_visible(sg));
            break;
        }
        case IDM_VIEW_GRAPH:{
            IDM_PluginHooks *sg=plugins_get_speed_graph();
            if(sg) speed_graph_show(sg,!speed_graph_visible(sg));
            break;
        }
        case IDM_VIEW_BASKET:{
            IDM_PluginHooks *bk=plugins_get_basket();
            if(bk) basket_show(bk,!basket_visible(bk));
            break;
        }
        case IDM_SETTINGS_CLIPMON:{
            IDM_PluginHooks *cb=plugins_get_clipboard();
            if(cb){
                BOOL en=!clipboard_plugin_is_enabled(cb);
                clipboard_plugin_set_enabled(cb,en);
                g->cfg->clipboard_monitor=en;
                HMENU hm=GetMenu(hwnd);
                HMENU hs=GetSubMenu(hm,2);
                CheckMenuItem(hs,IDM_SETTINGS_CLIPMON,
                    MF_BYCOMMAND|(en?MF_CHECKED:MF_UNCHECKED));
            }
            break;
        }
        case ID_BTN_CLEAR:
            core_clear_done(g->core);
            break;
        case ID_TRAY_SHOW:
            if(IsWindowVisible(hwnd)&&!IsIconic(hwnd))
                ShowWindow(hwnd,SW_HIDE);
            else{ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);}
            break;
        }
        return 0;
    }

    case WM_NOTIFY:{
        NMHDR *nm=(NMHDR*)lp;
        if(nm->idFrom==IDC_LISTVIEW){
            if(nm->code==NM_DBLCLK){
                int id=lv_selected_id(g->listview);
                if(id>=0){LDM_Item it;
                           core_get_item(g->core,(uint32_t)id,&it);
                           if(it.state==DL_COMPLETED)
                               ShellExecuteA(hwnd,"open",it.out_path,NULL,NULL,SW_SHOW);}
            } else if(nm->code==NM_RCLICK){
                ctx_menu(g, lv_selected_id(g->listview));
            }
        }
        break;
    }

    case WM_TRAY_ICON:
        if(LOWORD(lp)==WM_RBUTTONUP||LOWORD(lp)==WM_CONTEXTMENU)
            tray_menu(g);
        else if(LOWORD(lp)==WM_LBUTTONUP||LOWORD(lp)==WM_LBUTTONDBLCLK){
            /* تک‌کلیک یا دابل‌کلیک — هر دو پنجره رو نشون بدن */
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else                ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_SYSCOMMAND:
        if((wp&0xFFF0)==SC_MINIMIZE && g->cfg->minimize_to_tray){
            ShowWindow(hwnd,SW_HIDE); return 0;
        }
        break;

    case WM_QUERYENDSESSION:
        /* باگ واقعی که در نصب/آپدیت واقعی دیده شد: وقتی Lass باز بود
           و installer سعی می‌کرد قبل از آپدیت خودکار ببندش (از طریق
           Windows Restart Manager)، Setup روی "Shutting down
           applications using our files" هنگ می‌کرد و در نهایت با
           exit code 1 شکست می‌خورد — تا وقتی کاربر یا دستی از تری
           Exit می‌زد یا از Task Manager می‌کشتش.

           علتش: Restart Manager برای بستن مؤدبانه‌ی یه اپ، WM_CLOSE
           نمی‌فرسته — دقیقاً همین پیام (WM_QUERYENDSESSION) رو می‌فرسته
           (همون پیامی که موقع shutdown/logoff واقعی ویندوز هم میاد).
           قبل از این تغییر، این پیام هیچ case جداگانه‌ای نداشت، پس
           می‌رفت به DefWindowProcA که فقط TRUE برمی‌گردونه بدون این‌که
           واقعاً پنجره/process رو ببنده؛ یعنی exe هیچ‌وقت آزاد نمی‌شد.

           بر خلاف WM_CLOSE معمولی (که فقط از کلیک کاربر روی X میاد و
           باید minimize-to-tray رو رعایت کنه)، WM_QUERYENDSESSION فقط
           و فقط از طرف خودِ ویندوز/Restart Manager میاد، هیچ‌وقت از
           یه کلیک عادی کاربر نه — پس این‌جا باید همیشه واقعاً بسته
           بشیم (دقیقاً همون مسیر ID_TRAY_EXIT)، صرف‌نظر از تنظیم
           minimize_to_tray. */
        tray_remove(g);
        DestroyWindow(hwnd);
        return TRUE;

    case WM_CLOSE:
        if(g->cfg->minimize_to_tray){
            ShowWindow(hwnd,SW_HIDE);
            /* یه بار در هر session به کاربر اطلاع بده که برنامه
               هنوز در background فعاله — این شامل زمان‌بندی‌های
               دانلود، دانلودهای در حال انجام، و شمارش معکوس‌های
               Speed Schedule هم میشه. بدون این پیام، خیلی از
               کاربرها فکر می‌کنن با زدن X برنامه کامل بسته شده. */
            {
                static BOOL shown_once = FALSE;
                if (!shown_once) {
                    shown_once = TRUE;
                    NOTIFYICONDATAA nid = {0};
                    nid.cbSize = sizeof(nid);
                    nid.hWnd = hwnd;
                    nid.uID = 1;
                    nid.uFlags = NIF_INFO;
                    nid.dwInfoFlags = NIIF_INFO;
                    strncpy(nid.szInfoTitle, "Lass is still running",
                            sizeof(nid.szInfoTitle)-1);
                    strncpy(nid.szInfo,
                            "Downloads and schedules continue in the "
                            "background. Right-click the tray icon to exit.",
                            sizeof(nid.szInfo)-1);
                    Shell_NotifyIconA(NIM_MODIFY, &nid);
                }
            }
            return 0;
        }
        tray_remove(g); DestroyWindow(hwnd); return 0;

    case WM_DESTROY:
        settings_save_window(g->cfg,hwnd);
        {IDM_PluginHooks *cb=plugins_get_clipboard();
         if(cb) g->cfg->clipboard_monitor=clipboard_plugin_is_enabled(cb);}
        {IDM_PluginHooks *bk=plugins_get_basket();
         if(bk) g->cfg->basket_visible=basket_visible(bk);}
        settings_save(g->cfg);
        tray_remove(g);
        KillTimer(hwnd,IDC_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd,msg,wp,lp);
}

/* ======================================================
   Public API
   ====================================================== */
/* ======================================================
   Video Quality Picker Dialog — نمایش کیفیت‌های پیدا‌شده
   ====================================================== */
#define IDC_VS_LIST    8001
#define IDC_VS_BEST    8002
#define IDC_VS_TITLE   8003

typedef struct { GUO_Context *g; VS_VideoInfo info; } VS_DlgCtx;

static LRESULT CALLBACK vs_dlg_proc(HWND hwnd, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    VS_DlgCtx *ctx = (VS_DlgCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    HFONT fnt = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        ctx = (VS_DlgCtx *)cs->lpCreateParams;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

        /* عنوان ویدیو */
        HWND ht = CreateWindowExA(0, "STATIC", ctx->info.title[0]
                   ? ctx->info.title : "Video detected",
                   WS_CHILD|WS_VISIBLE|SS_ENDELLIPSIS,
                   10, 8, 440, 18, hwnd, (HMENU)(UINT_PTR)IDC_VS_TITLE,
                   NULL, NULL);
        SendMessageA(ht, WM_SETFONT, (WPARAM)fnt, TRUE);

        /* نام سایت */
        const char *src_names[] = {"","Direct Link","HLS Stream","DASH Stream",
                                    "YouTube","آپارات","Instagram","Twitter/X",
                                    "Vimeo","Video Page"};
        char src_lbl[64];
        int sidx = (int)ctx->info.source;
        if (sidx >= 0 && sidx < 10)
            snprintf(src_lbl, sizeof(src_lbl), "Source: %s", src_names[sidx]);
        HWND hs = CreateWindowExA(0, "STATIC", src_lbl,
                   WS_CHILD|WS_VISIBLE,
                   10, 28, 200, 17, hwnd, NULL, NULL, NULL);
        SendMessageA(hs, WM_SETFONT, (WPARAM)fnt, TRUE);

        /* ListBox کیفیت‌ها */
        HWND hl = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                   WS_CHILD|WS_VISIBLE|LBS_NOTIFY|WS_VSCROLL,
                   10, 50, 440, 130, hwnd,
                   (HMENU)(UINT_PTR)IDC_VS_LIST, NULL, NULL);
        SendMessageA(hl, WM_SETFONT, (WPARAM)fnt, TRUE);

        for (int i = 0; i < ctx->info.quality_count; i++) {
            char entry[VS_MAX_LABEL + 20];
            snprintf(entry, sizeof(entry), "%s  (%s)",
                     ctx->info.qualities[i].label,
                     ctx->info.qualities[i].ext);
            SendMessageA(hl, LB_ADDSTRING, 0, (LPARAM)entry);
        }
        /* انتخاب پیش‌فرض: بهترین کیفیت */
        SendMessageA(hl, LB_SETCURSEL,
                     (WPARAM)ctx->info.best_idx, 0);

        /* دکمه‌ها */
        HWND hb = CreateWindowExA(0, "BUTTON", "Download Selected",
                   WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                   10, 192, 160, 26, hwnd, (HMENU)(UINT_PTR)IDOK, NULL, NULL);
        SendMessageA(hb, WM_SETFONT, (WPARAM)fnt, TRUE);

        HWND hb2 = CreateWindowExA(0, "BUTTON", "Download Best",
                    WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                    178, 192, 120, 26, hwnd,
                    (HMENU)(UINT_PTR)IDC_VS_BEST, NULL, NULL);
        SendMessageA(hb2, WM_SETFONT, (WPARAM)fnt, TRUE);

        HWND hc = CreateWindowExA(0, "BUTTON", "Cancel",
                   WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                   306, 192, 80, 26, hwnd,
                   (HMENU)(UINT_PTR)IDCANCEL, NULL, NULL);
        SendMessageA(hc, WM_SETFONT, (WPARAM)fnt, TRUE);

        return 0;
    }

    case WM_COMMAND: {
        if (!ctx) return 0;
        WORD cmd = LOWORD(wp);

        if (cmd == IDCANCEL) { DestroyWindow(hwnd); return 0; }

        int sel_idx = -1;
        if (cmd == IDC_VS_BEST) {
            sel_idx = vs_best_quality(&ctx->info);
        } else if (cmd == IDOK) {
            HWND hl = GetDlgItem(hwnd, IDC_VS_LIST);
            sel_idx = (int)SendMessageA(hl, LB_GETCURSEL, 0, 0);
            if (sel_idx == LB_ERR) sel_idx = ctx->info.best_idx;
        }

        if (sel_idx >= 0 && sel_idx < ctx->info.quality_count) {
            const VS_Quality *q = &ctx->info.qualities[sel_idx];

            /* ساختن out_path */
            char fname[256] = {0};
            const char *title = ctx->info.title[0]
                                ? ctx->info.title : "video";
            /* sanitize عنوان برای نام فایل */
            int fi = 0;
            for (int ti = 0; title[ti] && fi < 80; ti++) {
                char c = title[ti];
                if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||
                    c=='"'||c=='<'||c=='>'||c=='|') c='_';
                fname[fi++] = c;
            }
            fname[fi] = '\0';
            if (!fi) strcpy(fname, "video");
            strncat(fname, q->ext, sizeof(fname)-strlen(fname)-1);

            char path[MAX_PATH*2] = {0};
            if (ctx->g->cfg->default_dir[0])
                safe_path_combine(path, sizeof(path), ctx->g->cfg->default_dir, fname);
            else {
                GetTempPathA(sizeof(path), path);
                strncat(path, fname, sizeof(path)-strlen(path)-1);
            }

            core_add(ctx->g->core, q->url, path,
                     q->is_hls || q->is_dash ? 1 : 4, 0);

            char msg[256 + VS_MAX_LABEL + 64];
            snprintf(msg, sizeof(msg),
                     "Added to queue:\n%s\n\nQuality: %s",
                     fname, q->label);
            MessageBoxA(hwnd, msg, "Lass - Video Download",
                         MB_ICONINFORMATION);
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        if (ctx) free(ctx);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* GUO callback از vs_sniff_async */
static void guo_on_video_found(const VS_VideoInfo *info, void *ud) {
    GUO_Context *g = (GUO_Context *)ud;
    if (!g || !info || info->quality_count == 0) return;

    /* اگه فقط یه کیفیت هست و لینک مستقیمه — مستقیم اضافه کن */
    if (info->quality_count == 1 && info->source == VS_SRC_DIRECT) {
        const VS_Quality *q = &info->qualities[0];
        char fname[256] = {0};
        const char *sl = strrchr(q->url, '/');
        strncpy(fname, (sl&&*(sl+1)) ? sl+1 : "video", 255);
        char *qu = strchr(fname,'?'); if(qu)*qu='\0';
        if (!fname[0]) strcpy(fname,"video");

        char path[MAX_PATH*2]={0};
        if (g->cfg->default_dir[0])
            safe_path_combine(path, sizeof(path), g->cfg->default_dir, fname);
        else { GetTempPathA(sizeof(path),path);
               strncat(path,fname,sizeof(path)-strlen(path)-1); }

        core_add(g->core, q->url, path,
                 q->is_hls||q->is_dash ? 1 : 4, 0);

        /* notification */
        IDM_PluginHooks *np = plugins_get_notify();
        if (np) notify_send(np, NOTIFY_COMPLETE, fname,
                             "Video added to queue");
        return;
    }

    /* چند کیفیت — نشون بده dialog انتخاب */
    WNDCLASSEXA wc = {0};
    wc.cbSize=sizeof(wc); wc.lpszClassName="LassVSPicker";
    wc.lpfnWndProc=vs_dlg_proc;
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
    wc.hInstance=GetModuleHandleA(NULL);
    RegisterClassExA(&wc);

    VS_DlgCtx *ctx = (VS_DlgCtx *)malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->g    = g;
    ctx->info = *info;  /* کپی کامل */

    HWND dlg = CreateWindowExA(
        WS_EX_TOPMOST|WS_EX_DLGMODALFRAME,
        "LassVSPicker",
        "Lass - Download Video",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT, 474, 240,
        g->main_wnd, NULL, GetModuleHandleA(NULL), ctx);
    if (!dlg) { free(ctx); return; }
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}

BOOL guo_init(GUO_Context *g, HINSTANCE hInst, LDM_Core *core,
               LassSettings *cfg) {
    memset(g,0,sizeof(*g));
    g->hinstance = hInst;
    g->core      = core;
    g->cfg       = cfg;
    g_guo        = g;

    /* ثبت callback به core */
    core_set_notify_cb(core, guo_on_notify, g);

    /* ثبت window classes */
    WNDCLASSEXA wc={0};
    wc.cbSize=sizeof(wc);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
    wc.hInstance=hInst;
    wc.style=CS_HREDRAW|CS_VREDRAW|CS_DBLCLKS;

    /* آیکون اصلی برنامه — برای taskbar/Alt+Tab/desktop shortcut */
    HICON hAppIcon = LoadIconA(hInst, MAKEINTRESOURCEA(IDI_MAIN_ICON));
    wc.hIcon   = hAppIcon ? hAppIcon : LoadIconA(NULL, IDI_APPLICATION);
    wc.hIconSm = hAppIcon ? hAppIcon : LoadIconA(NULL, IDI_APPLICATION);

    wc.lpszClassName=WC_MAIN; wc.lpfnWndProc=main_wnd_proc;
    RegisterClassExA(&wc);

    wc.lpszClassName=WC_ADDDLG; wc.lpfnWndProc=add_wnd_proc;
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassExA(&wc);

    wc.lpszClassName=WC_SETTINGS; wc.lpfnWndProc=settings_wnd_proc;
    RegisterClassExA(&wc);

    /* Batch و Schedule classes */
    wc.lpszClassName="LassBatchDlg"; wc.lpfnWndProc=DefWindowProcA;
    RegisterClassExA(&wc);
    wc.lpszClassName="LassSchedDlg"; wc.lpfnWndProc=DefWindowProcA;
    RegisterClassExA(&wc);

    /* پنجره اصلی */
    g->main_wnd = CreateWindowExA(
        WS_EX_APPWINDOW, WC_MAIN,
        "Lass Download Manager v" LASS_VERSION_STRING,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,820,540,
        NULL,NULL,hInst,NULL);
    if(!g->main_wnd) return FALSE;

    SetMenu(g->main_wnd, create_menubar());

    /* مقداردهی اولیه‌ی checkbox منو بر اساس تنظیمات واقعی —
       بدون این، حتی اگه clipboard_monitor از قبل فعال بود،
       منو همیشه unchecked نشون می‌داد و کاربر گیج می‌شد. */
    {
        HMENU hm = GetMenu(g->main_wnd);
        HMENU hs = GetSubMenu(hm, 2); /* Settings menu */
        CheckMenuItem(hs, IDM_SETTINGS_CLIPMON,
            MF_BYCOMMAND | (cfg->clipboard_monitor ? MF_CHECKED : MF_UNCHECKED));
    }
    g->toolbar    = create_toolbar(g->main_wnd, hInst);
    g->cat_combo  = create_category_filter(g->main_wnd, hInst);
    g->cat_filter = -2; /* All */
    g->listview   = create_listview(g->main_wnd, hInst);
    g->statusbar  = CreateWindowExA(0,STATUSCLASSNAMEA,NULL,
        WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
        0,0,0,0,g->main_wnd,(HMENU)(UINT_PTR)IDC_STATUSBAR,hInst,NULL);

    /* tray */
    tray_add(g);

    /* notify plugin: آپدیت tray hwnd واقعی */
    {
        IDM_PluginHooks *np = plugins_get_notify();
        if (np) notify_set_tray(np, g->main_wnd, 9001);
    }

    /* video sniffer: callback به GUO وصل کن */
    {
        IDM_PluginHooks *vs = plugins_get_video_sniffer();
        if (vs) vs_set_callback(vs, guo_on_video_found, g);
    }

    /* موقعیت پنجره */
    settings_apply_window(cfg, g->main_wnd);

    /* timer */
    SetTimer(g->main_wnd, IDC_TIMER, 500, NULL);

    /* اعمال تنظیمات به core */
    core_settings_apply(core, cfg);

    /* ── populate ListView با دانلودهای session قبل ──
       queue_persist_load در core_create دانلودها رو به engine اضافه
       کرده، ولی UI هیچ‌وقت notify نشده. اینجا همه‌شون رو به
       ListView اضافه می‌کنیم و یه پیام به کاربر نشون میدیم. */
    {
        LDM_Item items[512];
        int cnt = core_list_items(core, items, 512);
        if (cnt > 0) {
            /* پر کردن ListView */
            ListView_DeleteAllItems(g->listview);
            for (int i = 0; i < cnt; i++)
                lv_add(g->listview, &items[i]);

            /* نمایش اطلاع‌رسانی در status bar */
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "  %d download(s) restored from last session - "
                     "click Resume to continue.", cnt);
            SendMessageA(g->statusbar, SB_SETTEXTA, 0, (LPARAM)msg);

            LOGI(LOG_MOD_GUO, "Restored %d download(s) to ListView", cnt);
        }
    }

    /* ── بررسی مرورگرهای نصب‌شده و پیشنهاد extension ──
       اگه Chrome یا Firefox نصبه ولی extension register نشده،
       یه MessageBox ساده نشون بده که کاربر بتونه تصمیم بگیره.
       این فقط یه‌بار نشون داده میشه (در رجیستری ذخیره میشه). */
    {
        HKEY hk;
        BOOL already_dismissed = FALSE;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                           "Software\\Lass\\ExtNotifyDone",
                           0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            already_dismissed = TRUE;
            RegCloseKey(hk);
        }

        if (!already_dismissed) {
            IDM_PluginHooks *bp = plugins_get_browser_ext();
            if (bp) {
                BOOL chrome_there  = browser_ext_is_browser_installed(BROWSER_CHROME);
                BOOL firefox_there = browser_ext_is_browser_installed(BROWSER_FIREFOX);
                BOOL chrome_reg    = browser_ext_is_registered(BROWSER_CHROME);
                BOOL firefox_reg   = browser_ext_is_registered(BROWSER_FIREFOX);

                BOOL need_notify = (chrome_there && !chrome_reg)
                                || (firefox_there && !firefox_reg);
                if (need_notify) {
                    char ext_msg[512], ext_dir[MAX_PATH];
                    browser_ext_get_extension_dir(ext_dir, sizeof(ext_dir));

                    snprintf(ext_msg, sizeof(ext_msg),
                        "Lass has browser extensions available!\n\n"
                        "%s%s"
                        "Extensions folder:\n%s\n\n"
                        "Go to Settings -> Browser Extension -> Register All\n"
                        "to enable automatic download capture.\n\n"
                        "(This message won't show again.)",
                        chrome_there  && !chrome_reg  ? "- Google Chrome detected\n"  : "",
                        firefox_there && !firefox_reg ? "- Mozilla Firefox detected\n" : "",
                        ext_dir);

                    MessageBoxA(g->main_wnd, ext_msg,
                                "Lass - Browser Extension Available",
                                MB_ICONINFORMATION | MB_OK);

                    /* ذخیره در رجیستری تا دیگه نپرسه */
                    HKEY hk2; DWORD disp;
                    RegCreateKeyExA(HKEY_CURRENT_USER,
                                    "Software\\Lass\\ExtNotifyDone",
                                    0, NULL, 0, KEY_SET_VALUE,
                                    NULL, &hk2, &disp);
                    if (hk2) RegCloseKey(hk2);
                }
            }
        }
    }

    return TRUE;
}

void guo_destroy(GUO_Context *g) {
    if(!g||!g->main_wnd) return;
    tray_remove(g);
    if(g->add_hwnd&&IsWindow(g->add_hwnd))    DestroyWindow(g->add_hwnd);
    if(g->settings_hwnd&&IsWindow(g->settings_hwnd)) DestroyWindow(g->settings_hwnd);
    if(g->batch_hwnd&&IsWindow(g->batch_hwnd)) DestroyWindow(g->batch_hwnd);
    g_guo=NULL;
}

int guo_run(GUO_Context *g) {
    BOOL minimized = g->cfg->start_minimized;
    if (minimized && g->cfg->minimize_to_tray) {
        /* مخفی کامل به tray — نه فقط minimize روی taskbar */
        ShowWindow(g->main_wnd, SW_HIDE);
    } else if (minimized) {
        ShowWindow(g->main_wnd, SW_SHOWMINIMIZED);
    } else {
        ShowWindow(g->main_wnd, SW_SHOWNORMAL);
        UpdateWindow(g->main_wnd);
        SetForegroundWindow(g->main_wnd);
    }

    MSG m;
    while(GetMessageA(&m,NULL,0,0)){
        /* اگه settings یا add dialog باز هستن IsDialogMessage */
        if(g->settings_hwnd && IsWindow(g->settings_hwnd) &&
           IsDialogMessageA(g->settings_hwnd,&m)) continue;
        if(g->add_hwnd && IsWindow(g->add_hwnd) &&
           IsDialogMessageA(g->add_hwnd,&m)) continue;
        if(g->batch_hwnd && IsWindow(g->batch_hwnd) &&
           IsDialogMessageA(g->batch_hwnd,&m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}

void guo_show(GUO_Context *g, BOOL visible) {
    if(!g||!g->main_wnd) return;
    ShowWindow(g->main_wnd, visible?SW_SHOW:SW_HIDE);
    if(visible) SetForegroundWindow(g->main_wnd);
}

void guo_show_add_dialog(GUO_Context *g) {
    if(!g) return;
    if(g->add_hwnd&&IsWindow(g->add_hwnd)){
        SetForegroundWindow(g->add_hwnd); return;
    }
    g->add_hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME, WC_ADDDLG,
        "Add Download",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,470,140,
        g->main_wnd,NULL,g->hinstance,NULL);
    if(g->add_hwnd){ShowWindow(g->add_hwnd,SW_SHOW);SetForegroundWindow(g->add_hwnd);}
}

void guo_show_settings(GUO_Context *g) {
    if(!g) return;
    if(g->settings_hwnd&&IsWindow(g->settings_hwnd)){
        SetForegroundWindow(g->settings_hwnd); return;
    }
    g->settings_hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME, WC_SETTINGS,
        i18n_str(STR_PREFS_TITLE),
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        CW_USEDEFAULT,CW_USEDEFAULT,468,500,
        g->main_wnd,NULL,g->hinstance,NULL);
    if(g->settings_hwnd){
        ShowWindow(g->settings_hwnd,SW_SHOW);
        SetForegroundWindow(g->settings_hwnd);
    }
}
