/* test_ftpd.c — سرور FTP حداقلی (فقط loopback) برای تست کلاینت FTP */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "test_ftpd.h"

static volatile LONG g_stop   = 0;
static volatile LONG g_active = 0;
static uint64_t      g_fsize  = 0;
static SOCKET        g_listen = INVALID_SOCKET;
static HANDLE        g_accept_thread = NULL;

uint8_t test_ftpd_byte(uint64_t pos) {
    return (uint8_t)((pos * 31u + 7u) & 0xFFu);
}

static BOOL srv_send(SOCKET s, const char *data, size_t len) {
    while (len > 0) {
        int w = send(s, data, (int)len, 0);
        if (w <= 0) return FALSE;
        data += w;
        len  -= (size_t)w;
    }
    return TRUE;
}

static void srv_reply(SOCKET s, const char *line) {
    srv_send(s, line, strlen(line));
}

/* یک خط کامل (بدون CRLF) را می‌خواند */
static BOOL srv_line(SOCKET s, char *out, size_t sz) {
    size_t n = 0;
    for (;;) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return FALSE;
        if (c == '\n') break;
        if (c != '\r' && n < sz - 1) out[n++] = c;
    }
    out[n] = '\0';
    return TRUE;
}

static SOCKET open_pasv_listener(uint16_t *port_out) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    int alen = sizeof(a);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(s, 1) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    *port_out = ntohs(a.sin_port);
    return s;
}

static DWORD WINAPI conn_thread(LPVOID param) {
    SOCKET c = (SOCKET)(UINT_PTR)param;
    InterlockedIncrement(&g_active);
    DWORD tmo = 30000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    uint64_t rest = 0;
    SOCKET pasv = INVALID_SOCKET;
    srv_reply(c, "220 Lass test FTP\r\n");

    char line[512];
    while (!g_stop && srv_line(c, line, sizeof(line))) {
        if (_strnicmp(line, "USER", 4) == 0) {
            srv_reply(c, "331 password required\r\n");
        } else if (_strnicmp(line, "PASS", 4) == 0) {
            srv_reply(c, "230 logged in\r\n");
        } else if (_strnicmp(line, "TYPE", 4) == 0) {
            srv_reply(c, "200 type set\r\n");
        } else if (_strnicmp(line, "SIZE", 4) == 0) {
            if (strcmp(line + 5, "/file.bin") == 0) {
                char b[64];
                snprintf(b, sizeof(b), "213 %llu\r\n", (unsigned long long)g_fsize);
                srv_reply(c, b);
            } else {
                srv_reply(c, "550 no such file\r\n");
            }
        } else if (_strnicmp(line, "REST", 4) == 0) {
            rest = (uint64_t)strtoull(line + 5, NULL, 10);
            srv_reply(c, "350 restarting\r\n");
        } else if (_strnicmp(line, "PASV", 4) == 0) {
            if (pasv != INVALID_SOCKET) closesocket(pasv);
            uint16_t port = 0;
            pasv = open_pasv_listener(&port);
            if (pasv == INVALID_SOCKET) {
                srv_reply(c, "425 cannot open data connection\r\n");
            } else {
                char b[128];
                snprintf(b, sizeof(b), "227 Entering Passive Mode (127,0,0,1,%u,%u)\r\n",
                         (unsigned)(port / 256), (unsigned)(port % 256));
                srv_reply(c, b);
            }
        } else if (_strnicmp(line, "RETR", 4) == 0) {
            if (strcmp(line + 5, "/file.bin") != 0) {
                srv_reply(c, "550 no such file\r\n");
                continue;
            }
            if (pasv == INVALID_SOCKET) {
                srv_reply(c, "425 use PASV first\r\n");
                continue;
            }
            srv_reply(c, "150 opening data connection\r\n");
            SOCKET d = INVALID_SOCKET;
            fd_set rfd;
            FD_ZERO(&rfd);
            FD_SET(pasv, &rfd);
            struct timeval tv = {5, 0};
            if (select(0, &rfd, NULL, NULL, &tv) == 1) d = accept(pasv, NULL, NULL);
            if (d != INVALID_SOCKET) {
                static uint8_t buf[16384];
                uint64_t pos = rest;
                while (pos < g_fsize && !g_stop) {
                    size_t n = (size_t)(g_fsize - pos < sizeof(buf) ? g_fsize - pos : sizeof(buf));
                    for (size_t i = 0; i < n; i++) buf[i] = test_ftpd_byte(pos + (uint64_t)i);
                    if (!srv_send(d, (const char *)buf, n)) break;
                    pos += (uint64_t)n;
                }
                closesocket(d);
            }
            closesocket(pasv);
            pasv = INVALID_SOCKET;
            rest = 0;
            srv_reply(c, "226 transfer complete\r\n");
        } else if (_strnicmp(line, "QUIT", 4) == 0) {
            srv_reply(c, "221 bye\r\n");
            break;
        } else {
            srv_reply(c, "200 ok\r\n");
        }
    }
    if (pasv != INVALID_SOCKET) closesocket(pasv);
    closesocket(c);
    InterlockedDecrement(&g_active);
    return 0;
}

static DWORD WINAPI accept_loop(LPVOID unused) {
    (void)unused;
    while (!g_stop) {
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(g_listen, &rfd);
        struct timeval tv = {0, 200000};
        if (select(0, &rfd, NULL, NULL, &tv) <= 0) continue;
        SOCKET c = accept(g_listen, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        HANDLE t = CreateThread(NULL, 0, conn_thread, (LPVOID)(UINT_PTR)c, 0, NULL);
        if (t) CloseHandle(t);
        else closesocket(c);
    }
    return 0;
}

bool test_ftpd_start(uint16_t port, uint64_t file_size) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_fsize = file_size;
    InterlockedExchange(&g_stop, 0);
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return false; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);
    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(g_listen, 16) != 0) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    g_accept_thread = CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
    if (!g_accept_thread) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    return true;
}

void test_ftpd_stop(void) {
    InterlockedExchange(&g_stop, 1);
    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_accept_thread) {
        WaitForSingleObject(g_accept_thread, 5000);
        CloseHandle(g_accept_thread);
        g_accept_thread = NULL;
    }
    for (int i = 0; i < 500 && g_active > 0; i++) Sleep(10);
    WSACleanup();
}
