/* transport.c — رجوع کن به transport.h */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "transport.h"
#include "engine.h"
#include "../plugins/plugin_ftp.h"
#include "../plugins/security_fixes.h"

struct DlStream {
    BOOL       is_ftp;
    HTTP_Conn *http;
    FtpStream *ftp;
};

BOOL dl_url_supported(const char *url) {
    if (!url) return FALSE;
    if (ftp_is_ftp_url(url)) return ftp_url_supported(url);
    return validate_url(url, IDM_MAX_URL);
}

IDM_Result dl_discover(const char *url, const char *referer,
                       struct IDM_Download *dl, HTTP_DiscoveryInfo *out) {
    if (!out) return IDM_ERR_INVALID;
    if (!ftp_is_ftp_url(url)) return http_discover(url, referer, dl, out);

    memset(out, 0, sizeof(*out));
    uint64_t size = 0;
    BOOL ranges = FALSE;
    char fname[IDM_MAX_FILENAME] = {0};
    IDM_Result r = ftp_discover(url, &size, fname, sizeof(fname), &ranges);
    if (r != IDM_OK) return r;
    out->content_length = size;
    out->accept_ranges  = ranges;
    out->is_https       = FALSE;
    snprintf(out->filename, sizeof(out->filename), "%s", fname);
    return IDM_OK;
}

DlStream *dl_stream_open_range(struct IDM_Download *dl, uint64_t start, uint64_t end) {
    if (!dl) return NULL;
    DlStream *s = (DlStream *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (ftp_is_ftp_url(dl->url)) {
        s->is_ftp = TRUE;
        s->ftp = ftp_range_open(dl_conn_url(dl), start, end);
        if (!s->ftp) { free(s); return NULL; }
    } else {
        s->http = http_open_range(dl->url, dl->referer, dl, start, end);
        if (!s->http) { free(s); return NULL; }
    }
    return s;
}

int dl_stream_read(DlStream *s, void *buf, int buf_size) {
    if (!s) return -1;
    return s->is_ftp ? ftp_range_read(s->ftp, buf, buf_size)
                     : http_read(s->http, buf, buf_size);
}

BOOL dl_stream_got_partial(const DlStream *s) {
    if (!s) return FALSE;
    return s->is_ftp ? ftp_range_got_partial(s->ftp)
                     : (BOOL)http_conn_got_partial(s->http);
}

void dl_stream_close(DlStream *s) {
    if (!s) return;
    if (s->is_ftp) ftp_range_close(s->ftp);
    else           http_close(s->http);
    free(s);
}

const char *dl_conn_url(const struct IDM_Download *dl) {
    if (!dl) return "";
    return dl->secret_url[0] ? dl->secret_url : dl->url;
}
