#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static FILE *log_file = nullptr;

// ──────────────────────────────────────────
//  log_open — открывает файл для дозаписи
//  Вызывать один раз в server/main.cpp
// ──────────────────────────────────────────
void log_open(const char *path)
{
    log_file = fopen(path, "a");
    if (!log_file) {
        perror("[logger] fopen");
    }
}

// ──────────────────────────────────────────
//  log_close — закрывает файл
// ──────────────────────────────────────────
void log_close()
{
    if (log_file) {
        fclose(log_file);
        log_file = nullptr;
    }
}

// ──────────────────────────────────────────
//  log_event — запись строки с временной меткой
//
//  Формат строки в файле:
//    [2026-05-18 14:32:01] Клиент alice подключился
// ──────────────────────────────────────────
void log_event(const char *format, ...)
{
    if (!log_file) return;

    // Временная метка
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    fprintf(log_file, "[%s] ", timebuf);

    // Форматированная строка (как printf)
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file); // сразу сбрасываем на диск
}
