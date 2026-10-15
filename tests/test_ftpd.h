/* test_ftpd.h — سرور FTP حداقلی برای تست کلاینت (فقط loopback) */
#ifndef LASS_TEST_FTPD_H
#define LASS_TEST_FTPD_H

#include <stdint.h>
#include <stdbool.h>

/* سرور روی 127.0.0.1:port راه می‌افتد و فایل /file.bin را با اندازه‌ی size سرو می‌کند */
bool    test_ftpd_start(uint16_t port, uint64_t file_size);
void    test_ftpd_stop(void);
/* بایت مورد انتظار در موقعیت pos از فایل تست */
uint8_t test_ftpd_byte(uint64_t pos);

#endif
