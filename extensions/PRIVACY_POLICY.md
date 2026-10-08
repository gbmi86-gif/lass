# Privacy Policy — Lass Download Manager (Browser Extension)

**Last updated:** September 2026

This policy covers the Lass Download Manager browser extension for Google
Chrome and Mozilla Firefox (the "Extension"). It does not cover the Lass
Download Manager desktop application beyond what is described below.

## Summary

The Extension does not collect, store, transmit, or sell any personal data
to the developer, to Google, to Mozilla, or to any third party. It has no
analytics, no telemetry, and no remote server of any kind. Its only job is
to notice download links on pages you visit and hand them off — entirely on
your own computer — to the Lass Download Manager desktop application, if
and when it is running.

## What the Extension does

When you click a download link, or choose "Download with Lass" from the
right-click menu, the Extension sends the URL (and, where available, the
suggested filename and the referring page) to the Lass Download Manager
desktop application installed on the same computer. This is done entirely
through your operating system's local interprocess communication (a named
pipe on Windows) — the data never leaves your computer, and no server
operated by the developer, Google, or Mozilla is involved at any point.

If the Lass Download Manager desktop application is not running, the
Extension does nothing with the link beyond letting the browser's normal
download behavior proceed; no data is queued, cached, or sent anywhere.

## Permissions and why the Extension requests them

| Permission | Why it's needed |
|---|---|
| `nativeMessaging` | To talk to the locally-installed Lass Download Manager desktop application. This is the only channel the Extension uses to send any information anywhere, and it never leaves your computer. |
| `downloads` | To detect when a browser download starts, so it can be handed to Lass Download Manager instead (or alongside) the browser's own download manager. |
| `webRequest` / `webRequestBlocking` (Firefox) | To recognize downloadable links (e.g. direct file links) that the `downloads` API alone would not catch. |
| `<all_urls>` (host permission) | Download links can appear on any website, so the Extension needs to be able to recognize them regardless of which site you're on. This permission is used only to detect links; the Extension does not read, log, or transmit page content. |
| `contextMenus` | To add the "Download with Lass" option to the browser's right-click menu. |
| `notifications` | To show a local notification when a download has been handed off (e.g. success or "Lass is not running"). These notifications are generated and shown entirely on your device. |
| `storage` | To remember the Extension's own settings (e.g. whether it's enabled) locally in your browser. This data stays in your browser's local storage and is never transmitted anywhere. |

## Data retention

Because the Extension does not collect or transmit any personal data, there
is nothing for the developer to retain, and nothing to delete on request —
there is simply no data leaving your device to begin with. Any local
settings the Extension stores (via the `storage` permission) can be removed
at any time by uninstalling the Extension.

## Children's privacy

The Extension is not directed at children and does not knowingly collect
any information from anyone, regardless of age, because it does not collect
information from anyone at all.

## Changes to this policy

If this policy changes, the "Last updated" date above will change
accordingly. Continued use of the Extension after an update constitutes
acceptance of the revised policy.

## Contact

Questions about this policy or the Extension can be sent to:
**[YOUR CONTACT EMAIL HERE]**

Source code for the Extension: **[YOUR REPOSITORY/PROJECT LINK HERE]**
