/* popup.js */
const statusEl = document.getElementById("status");

function setStatus(ok, msg) {
    statusEl.className = "status " + (ok ? "ok" : "err");
    statusEl.textContent = ok ? "✓ " + msg : "✗ " + msg;
}

/* load config */
chrome.runtime.sendMessage({ action: "get_config" }, (cfg) => {
    document.getElementById("chk_enabled").checked = cfg.enabled;
    document.getElementById("chk_video").checked   = cfg.video_capture;
    document.getElementById("chk_audio").checked   = cfg.audio_capture;
    document.getElementById("min_size").value       = Math.round(cfg.min_size / 1024 / 1024);
});

/* ping Lass */
chrome.runtime.sendMessage({ action: "ping" }, (r) => {
    if (r && r.ok) setStatus(true, "Connected to Lass v" + (r.version || "?"));
    else           setStatus(false, "Lass is not running");
});

/* save */
document.getElementById("btn_save").addEventListener("click", () => {
    const mb = parseFloat(document.getElementById("min_size").value) || 5;
    chrome.runtime.sendMessage({
        action: "set_config",
        data: {
            enabled:       document.getElementById("chk_enabled").checked,
            video_capture: document.getElementById("chk_video").checked,
            audio_capture: document.getElementById("chk_audio").checked,
            min_size:      mb * 1024 * 1024
        }
    }, () => window.close());
});
