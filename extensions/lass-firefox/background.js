/* background.js — Lass Download Manager Chrome Extension v5
   Service Worker (Manifest V3)
   ─────────────────────────────────────────────────────────
   این extension با Lass Download Manager از طریق Native Messaging
   ارتباط برقرار می‌کنه. هر دانلودی که در Chrome شروع بشه،
   ابتدا به Lass فرستاده میشه (اگه extension فعال باشه).
*/

const NATIVE_HOST = "com.lass.downloadmanager";
const VERSION     = "5.0";

/* ── تنظیمات قابل ذخیره ── */
let cfg = {
    enabled:        true,
    min_size:       5 * 1024 * 1024,  /* 5MB — فایل‌های کوچک‌تر ignore */
    excluded_exts:  [],                /* پسوندهایی که capture نشن */
    video_capture:  true,
    audio_capture:  true
};

/* ── بارگذاری تنظیمات از storage ── */
chrome.storage.local.get("lass_cfg", (r) => {
    if (r.lass_cfg) cfg = { ...cfg, ...r.lass_cfg };
});

/* ── پسوندهایی که معمولاً نباید به Lass بره ── */
const EXCLUDED_MIME = new Set([
    "text/html", "text/css", "text/javascript",
    "application/javascript", "application/json",
    "image/gif", "image/png", "image/jpeg",
    "image/svg+xml", "image/webp"
]);

/* ── MIME های ویدیو/صدا ── */
const VIDEO_MIME = new Set([
    "video/mp4", "video/x-matroska", "video/webm", "video/avi",
    "video/x-msvideo", "video/quicktime", "video/x-flv",
    "application/x-mpegurl", "application/dash+xml"
]);

const AUDIO_MIME = new Set([
    "audio/mpeg", "audio/flac", "audio/ogg", "audio/wav",
    "audio/aac", "audio/mp4", "audio/webm"
]);

/* ═══════════════════════════════════════════════════════
   تشخیص دانلود از webRequest headers
   ═══════════════════════════════════════════════════════ */
chrome.webRequest.onHeadersReceived.addListener(
    (details) => {
        if (!cfg.enabled) return;
        if (details.type !== "main_frame" && details.type !== "xmlhttprequest"
            && details.type !== "other") return;

        const headers = details.responseHeaders || [];
        let contentType = "";
        let contentDisp = "";
        let contentLen  = 0;

        for (const h of headers) {
            const name = h.name.toLowerCase();
            if (name === "content-type")        contentType = h.value;
            if (name === "content-disposition") contentDisp = h.value;
            if (name === "content-length")      contentLen  = parseInt(h.value) || 0;
        }

        /* چک کردن Content-Disposition: attachment */
        const isAttachment = /attachment/i.test(contentDisp);

        /* استخراج نام فایل */
        let filename = "";
        const fnMatch = contentDisp.match(/filename\*?=["']?([^"';\n]+)/i);
        if (fnMatch) filename = decodeURIComponent(fnMatch[1].replace(/UTF-8''/i, "").trim());

        if (!filename) {
            try {
                const url = new URL(details.url);
                filename = url.pathname.split("/").pop() || "download";
            } catch {}
        }

        /* تصمیم‌گیری — این listener فقط استریم‌های ویدیو/صدا رو
           می‌گیره (چیزهایی که هیچ‌وقت از downloads API رد نمی‌شن،
           مثل یه HLS/m3u8 که مستقیم توی صفحه پخش می‌شه، نه دانلود
           می‌شه). دانلودهای واقعی (Content-Disposition: attachment)
           دیگه این‌جا capture نمی‌شن — الان مسئولیتشون با
           chrome.downloads.onCreated پایین‌تره، چون اون واقعاً
           می‌تونه جلوی دانلودِ خودِ مرورگر رو بگیره (cancel کنه)،
           در حالی که این listener فقط observational هست (مخصوصاً
           توی Chrome MV3 که blocking webRequest دیگه برای
           extensionهای معمولی وجود نداره) و هیچ‌وقت نمی‌تونست
           جلوی دانلود دوتایی (هم مرورگر، هم Lass) رو بگیره. */
        const mime = contentType.split(";")[0].trim().toLowerCase();
        const isVideo = VIDEO_MIME.has(mime) || /\.m3u8|\.mpd|\.ts$/.test(details.url);
        const isAudio = AUDIO_MIME.has(mime);

        let shouldCapture = false;
        if (isVideo && cfg.video_capture) shouldCapture = true;
        if (isAudio && cfg.audio_capture) shouldCapture = true;
        if (contentLen > 0 && contentLen < cfg.min_size && !isVideo)
            shouldCapture = false;

        if (shouldCapture) {
            sendToLass({
                action:   "download",
                url:      details.url,
                filename: filename,
                referer:  details.initiator || "",
                mime:     mime,
                size:     contentLen
            });
        }
    },
    { urls: ["<all_urls>"] },
    ["responseHeaders"]
);

/* ═══════════════════════════════════════════════════════
   دانلود واقعی مرورگر — این قسمتیه که قبلاً اصلاً وجود نداشت.
   قبل از این، webRequest.onHeadersReceived فقط sendToLass رو صدا
   می‌زد ولی هیچ‌جا جلوی خودِ دانلود مرورگر رو نمی‌گرفت — یعنی
   حتی وقتی native messaging درست کار می‌کرد، هم Firefox/Chrome
   خودشون فایل رو دانلود می‌کردن، هم Lass جدا شروع می‌کرد (دو بار
   دانلود، نه یه handoff واقعی). این‌جا با downloads.onCreated
   (که برخلاف blocking webRequest، توی Chrome MV3 هم در دسترسه)
   همون لحظه‌ای که مرورگر می‌خواد دانلود رو شروع کنه می‌گیریمش،
   لغوش می‌کنیم، و URL رو به Lass می‌فرستیم — یعنی از دید کاربر
   انگار دانلود مستقیم رفت توی Lass. */
chrome.downloads.onCreated.addListener((item) => {
    if (!cfg.enabled) return;
    if (!item.url || item.url.startsWith("blob:")) return; /* blob قابل ارسال به native host نیست */

    const mime = (item.mime || "").split(";")[0].trim().toLowerCase();
    if (EXCLUDED_MIME.has(mime)) return;

    chrome.downloads.cancel(item.id, () => {
        /* خطای احتمالی cancel (مثلاً دانلود خیلی سریع تموم شده) رو
           عمداً نادیده می‌گیریم — چیز مهمی برای کاربر نیست */
        void chrome.runtime.lastError;
        chrome.downloads.erase({ id: item.id });
    });

    sendToLass({
        action:   "download",
        url:      item.url,
        filename: (item.filename || "").split(/[\\/]/).pop() || "",
        referer:  item.referrer || "",
        mime:     mime,
        size:     item.fileSize > 0 ? item.fileSize : 0
    });
});

/* ═══════════════════════════════════════════════════════
   Context Menu
   ═══════════════════════════════════════════════════════ */
chrome.runtime.onInstalled.addListener(() => {
    chrome.contextMenus.create({
        id:       "lass_download_link",
        title:    "Download with Lass",
        contexts: ["link", "video", "audio", "image"]
    });
    chrome.contextMenus.create({
        id:       "lass_download_page_links",
        title:    "Download all links with Lass (Batch)",
        contexts: ["page"]
    });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
    if (info.menuItemId === "lass_download_link") {
        sendToLass({
            action:   "download",
            url:      info.linkUrl || info.srcUrl || info.frameUrl,
            filename: "",
            referer:  tab.url || ""
        });
    } else if (info.menuItemId === "lass_download_page_links") {
        chrome.tabs.sendMessage(tab.id, { action: "get_page_links" }, (links) => {
            if (links && links.length > 0) {
                sendToLass({
                    action: "batch",
                    urls:   links.join("\n"),
                    dir:    "",
                    group:  tab.title || "Batch"
                });
            }
        });
    }
});

/* ═══════════════════════════════════════════════════════
   Native Messaging
   ═══════════════════════════════════════════════════════ */
function sendToLass(msg) {
    const port = chrome.runtime.connectNative(NATIVE_HOST);
    port.onDisconnect.addListener(() => {
        if (chrome.runtime.lastError) {
            console.warn("[Lass] Native host not running:", chrome.runtime.lastError.message);
            showNotification("Lass is not running. Please open Lass Download Manager.");
        }
    });
    port.postMessage(msg);
    /* بعد از ارسال disconnect می‌کنیم — one-shot */
    setTimeout(() => port.disconnect(), 500);
}

function showNotification(msg) {
    chrome.notifications.create({
        type:    "basic",
        iconUrl: "icons/icon48.png",
        title:   "Lass Download Manager",
        message: msg
    });
}

/* ═══════════════════════════════════════════════════════
   پیام از popup
   ═══════════════════════════════════════════════════════ */
chrome.runtime.onMessage.addListener((msg, sender, reply) => {
    if (msg.action === "ping") {
        const port = chrome.runtime.connectNative(NATIVE_HOST);
        port.onMessage.addListener((r) => { reply({ ok: true, version: r.version }); });
        port.onDisconnect.addListener(() => { reply({ ok: false }); });
        return true; /* async reply */
    }
    if (msg.action === "get_config")    { reply(cfg); return false; }
    if (msg.action === "set_config") {
        cfg = { ...cfg, ...msg.data };
        chrome.storage.local.set({ lass_cfg: cfg });
        reply({ ok: true });
        return false;
    }
});
