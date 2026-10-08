/* content.js — page link collector */
chrome.runtime.onMessage.addListener((msg, sender, reply) => {
    if (msg.action === "get_page_links") {
        const links = [];
        const seen  = new Set();
        document.querySelectorAll("a[href]").forEach(a => {
            const href = a.href;
            if (!seen.has(href) && /^https?:\/\//.test(href)) {
                seen.add(href);
                links.push(href);
            }
        });
        reply(links);
    }
});
