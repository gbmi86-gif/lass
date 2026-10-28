/* tests/test_hls_server.h
   سرور HTTP مسیرمحور روی loopback برای تست HLS (فقط برای تست).
   هر مسیر بدنه‌ی ثابت دارد و می‌تواند تا چند بار کد خطا بدهد.
   تعداد درخواست‌های هر مسیر شمرده می‌شود تا تست ثابت کند کدام segment ها
   واقعاً دوباره گرفته شده‌اند.
*/
#ifndef LASS_TEST_HLS_SERVER_H
#define LASS_TEST_HLS_SERVER_H

#include <windows.h>
#include <stdint.h>

BOOL hls_test_server_start(uint16_t port);
void hls_test_server_stop(void);
/* بدنه‌ی مسیر؛ بدنه کپی نمی‌شود، پس باید تا پایان تست زنده بماند */
void hls_test_server_set(const char *path, const uint8_t *body, uint32_t len);
/* مسیر را تا times بار با کد status خطا می‌دهد؛ times<=0 یعنی همیشه */
void hls_test_server_fail(const char *path, int status, int times);
void hls_test_server_clear_fail(const char *path);
/* تعداد درخواست‌های مسیر (شامل پاسخ‌های خطا) */
int  hls_test_server_hits(const char *path);
void hls_test_server_reset_hits(void);

#endif /* LASS_TEST_HLS_SERVER_H */
