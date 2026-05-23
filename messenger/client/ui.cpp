#include "ui.h"
#include "../common/message.h"

#include <locale.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// ──────────────────────────────────────────
//  Простой UI без ncurses:
//  - вывод сообщений напрямую в stdout
//  - ввод через сырой терминал (raw mode)
//  - никаких окон и рамок — не ломается
//    при изменении размера терминала
// ──────────────────────────────────────────

static pthread_mutex_t ui_mutex   = PTHREAD_MUTEX_INITIALIZER;
static struct termios  orig_termios;          // оригинальные настройки терминала
static char            input_buf[1100] = {0}; // текущая строка ввода
static int             input_len = 0;         // длина строки ввода
static int             raw_mode  = 0;

// ──────────────────────────────────────────
//  Управляющие последовательности ANSI
// ──────────────────────────────────────────
#define ANSI_RESET    "\033[0m"
#define ANSI_BOLD     "\033[1m"
#define ANSI_GREEN    "\033[32m"
#define ANSI_YELLOW   "\033[33m"
#define ANSI_CYAN     "\033[36m"
#define ANSI_WHITE    "\033[37m"
#define ANSI_GRAY     "\033[90m"

// Очистить текущую строку и вернуть курсор в начало
#define CLEAR_LINE    "\r\033[2K"

// ──────────────────────────────────────────
//  enable_raw / disable_raw
//  Переводим терминал в raw-режим:
//  символы приходят сразу без буферизации,
//  эхо отключено — управляем сами.
// ──────────────────────────────────────────
static void enable_raw()
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG); // без буфера, без эха, без Ctrl+C сигнала
    raw.c_iflag &= ~(IXON);                 // без Ctrl+S/Ctrl+Q
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode = 1;
}

static void disable_raw()
{
    if (raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode = 0;
    }
}

// ──────────────────────────────────────────
//  redraw_input_line
//  Перерисовывает строку ввода после того
//  как в чат пришло новое сообщение.
//  Вызывать под ui_mutex.
// ──────────────────────────────────────────
static void redraw_input_line()
{
    // Очищаем строку, печатаем приглашение и текущий буфер ввода
    printf(CLEAR_LINE ANSI_CYAN "> " ANSI_RESET "%.*s", input_len, input_buf);
    fflush(stdout);
}

// ──────────────────────────────────────────
//  ui_init
// ──────────────────────────────────────────
void ui_init()
{
    setlocale(LC_ALL, "");

    // Шапка
    printf("\n" ANSI_BOLD ANSI_CYAN
           "╔══════════════════════════════════╗\n"
           "║         МЕССЕНДЖЕР               ║\n"
           "╚══════════════════════════════════╝\n"
           ANSI_RESET);
    printf(ANSI_GRAY
           "  /msg <логин> <текст>   — личное сообщение\n"
           "  /group <группа> <текст> — групповое\n"
           "  /users  /history <логин>  /quit\n"
           ANSI_RESET "\n");
    fflush(stdout);
}

// ──────────────────────────────────────────
//  ui_print_message
//  Печатает сообщение поверх строки ввода:
//  1. стираем строку ввода
//  2. печатаем сообщение
//  3. восстанавливаем строку ввода
// ──────────────────────────────────────────
void ui_print_message(const Message *msg)
{
    char timebuf[10];
    struct tm *t = localtime(&msg->timestamp);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);

    pthread_mutex_lock(&ui_mutex);

    printf(CLEAR_LINE);  // стираем строку ввода

    // Время
    printf(ANSI_GRAY "[%s] " ANSI_RESET, timebuf);

    // Отправитель
    printf(ANSI_BOLD ANSI_GREEN "%s" ANSI_RESET, msg->sender);

    // Получатель
    if (msg->msg_type == MSG_GROUP)
        printf(ANSI_GRAY " -> [%s]" ANSI_RESET ": ", msg->dest.group_name);
    else
        printf(ANSI_GRAY " -> %s" ANSI_RESET ": ", msg->dest.receiver);

    // Текст
    printf("%s\n", msg->text);

    redraw_input_line();

    pthread_mutex_unlock(&ui_mutex);
}

// ──────────────────────────────────────────
//  ui_print_system
// ──────────────────────────────────────────
void ui_print_system(const char *text)
{
    pthread_mutex_lock(&ui_mutex);

    printf(CLEAR_LINE ANSI_YELLOW " [!] %s" ANSI_RESET "\n", text);
    redraw_input_line();

    pthread_mutex_unlock(&ui_mutex);
}

// ──────────────────────────────────────────
//  ui_read_input
//
//  Читает строку в raw-режиме посимвольно.
//  Корректно обрабатывает:
//    - UTF-8 (кириллица — 2 байта)
//    - Backspace (удаляет целый UTF-8 символ)
//    - Ctrl+C / Ctrl+D → возврат -1
//    - Enter → возврат длины строки
//    - стрелки / F-клавиши → игнорируем ESC-последовательности
// ──────────────────────────────────────────
int ui_read_input(char *buf, int maxlen)
{
    enable_raw();

    // Сбрасываем внутренний буфер
    memset(input_buf, 0, sizeof(input_buf));
    input_len = 0;

    pthread_mutex_lock(&ui_mutex);
    redraw_input_line();
    pthread_mutex_unlock(&ui_mutex);

    while (1) {
        unsigned char ch;
        int n = (int)read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            disable_raw();
            return -1;
        }

        // Ctrl+C или Ctrl+D
        if (ch == 3 || ch == 4) {
            disable_raw();
            printf("\n");
            return -1;
        }

        // Enter
        if (ch == '\r' || ch == '\n') {
            disable_raw();
            // Копируем в выходной буфер
            int copy = input_len < maxlen - 1 ? input_len : maxlen - 1;
            memcpy(buf, input_buf, copy);
            buf[copy] = '\0';
            printf("\n");
            fflush(stdout);
            // Сбрасываем внутренний буфер
            memset(input_buf, 0, sizeof(input_buf));
            input_len = 0;
            return copy;
        }

        // ESC-последовательность (стрелки, F-клавиши)
        // Формат: ESC [ X  или  ESC O X
        if (ch == 27) {
            // Ставим неблокирующий read с таймаутом чтобы съесть остаток
            struct termios tmp = orig_termios;
            tmp.c_lflag &= ~(ICANON | ECHO);
            tmp.c_cc[VMIN]  = 0;
            tmp.c_cc[VTIME] = 1; // 100мс
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &tmp);
            unsigned char esc[8];
            read(STDIN_FILENO, esc, sizeof(esc)); // съедаем остаток
            // Возвращаем raw
            enable_raw();
            continue;
        }

        // Backspace (DEL=127 или BS=8)
        if (ch == 127 || ch == 8) {
            if (input_len > 0) {
                // Удаляем последний UTF-8 символ:
                // байты продолжения имеют вид 10xxxxxx
                input_len--;
                while (input_len > 0 && (input_buf[input_len] & 0xC0) == 0x80)
                    input_len--;
                input_buf[input_len] = '\0';

                pthread_mutex_lock(&ui_mutex);
                redraw_input_line();
                pthread_mutex_unlock(&ui_mutex);
            }
            continue;
        }

        // Однобайтный печатаемый ASCII (0x20..0x7E)
        if (ch >= 0x20 && ch <= 0x7E) {
            if (input_len < (int)sizeof(input_buf) - 1 && input_len < maxlen - 1) {
                input_buf[input_len++] = (char)ch;
                input_buf[input_len]   = '\0';
                pthread_mutex_lock(&ui_mutex);
                redraw_input_line();
                pthread_mutex_unlock(&ui_mutex);
            }
            continue;
        }

        // Многобайтный UTF-8 (кириллица и др.)
        // Первый байт: 0xC0..0xDF → 2 байта, 0xE0..0xEF → 3, 0xF0..0xF7 → 4
        if (ch >= 0xC0 && ch <= 0xF7) {
            int extra = (ch >= 0xF0) ? 3 : (ch >= 0xE0) ? 2 : 1;
            if (input_len + extra + 1 >= (int)sizeof(input_buf) ||
                input_len + extra + 1 >= maxlen) continue;

            input_buf[input_len++] = (char)ch;

            // Читаем байты продолжения одним вызовом
            unsigned char cont[3] = {0};
            int got = (int)read(STDIN_FILENO, cont, extra);
            for (int i = 0; i < got; i++)
                input_buf[input_len++] = (char)cont[i];
            input_buf[input_len] = '\0';

            pthread_mutex_lock(&ui_mutex);
            redraw_input_line();
            pthread_mutex_unlock(&ui_mutex);
            continue;
        }
    }
}

// ──────────────────────────────────────────
//  ui_cleanup
// ──────────────────────────────────────────
void ui_cleanup()
{
    disable_raw();
    printf(ANSI_RESET "\n");
    fflush(stdout);
}