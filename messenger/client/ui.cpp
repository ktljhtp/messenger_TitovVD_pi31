#include "ui.h"
#include "../common/message.h"

#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ──────────────────────────────────────────
//  Внутренние переменные модуля
// ──────────────────────────────────────────
static WINDOW *chat_win  = nullptr; // окно истории переписки (верх)
static WINDOW *input_win = nullptr; // окно ввода (низ)
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// Цвета для ncurses
#define COLOR_CHAT_BORDER 1
#define COLOR_SENDER      2
#define COLOR_SYSTEM_MSG  3
#define COLOR_TIMESTAMP   4
#define COLOR_INPUT_LABEL 5

// ──────────────────────────────────────────
//  ui_init
//
//  Инициализирует ncurses, создаёт два окна.
//  chat_win  : строки 0 .. (LINES*0.8 - 1)
//  input_win : строки (LINES*0.8) .. (LINES-1)
// ──────────────────────────────────────────
void ui_init()
{
    initscr();           // инициализация ncurses
    cbreak();            // символы сразу без буфера
    noecho();            // не отображать нажатия автоматически
    keypad(stdscr, TRUE); // поддержка стрелок и спец-клавиш

    // Включаем цвета если терминал поддерживает
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COLOR_CHAT_BORDER, COLOR_CYAN,    -1);
        init_pair(COLOR_SENDER,      COLOR_GREEN,   -1);
        init_pair(COLOR_SYSTEM_MSG,  COLOR_YELLOW,  -1);
        init_pair(COLOR_TIMESTAMP,   COLOR_WHITE,   -1);
        init_pair(COLOR_INPUT_LABEL, COLOR_MAGENTA, -1);
    }

    // Вычисляем размеры окон
    int total_lines = LINES;
    int total_cols  = COLS;
    int chat_height = (total_lines * 4) / 5; // 80% под чат
    int input_height = total_lines - chat_height; // 20% под ввод

    // Создаём окно чата с рамкой
    chat_win = newwin(chat_height, total_cols, 0, 0);
    scrollok(chat_win, TRUE);      // разрешаем прокрутку
    idlok(chat_win, TRUE);         // оптимизация прокрутки
    wattron(chat_win, COLOR_PAIR(COLOR_CHAT_BORDER));
    box(chat_win, 0, 0);
    wattroff(chat_win, COLOR_PAIR(COLOR_CHAT_BORDER));

    // Заголовок
    wattron(chat_win, A_BOLD | COLOR_PAIR(COLOR_CHAT_BORDER));
    mvwprintw(chat_win, 0, 2, " МЕССЕНДЖЕР ");
    wattroff(chat_win, A_BOLD | COLOR_PAIR(COLOR_CHAT_BORDER));
    wrefresh(chat_win);

    // Создаём окно ввода с рамкой
    input_win = newwin(input_height, total_cols, chat_height, 0);
    wattron(input_win, COLOR_PAIR(COLOR_INPUT_LABEL));
    box(input_win, 0, 0);
    mvwprintw(input_win, 0, 2, " ВВОД ");
    wattroff(input_win, COLOR_PAIR(COLOR_INPUT_LABEL));

    // Подсказка по командам
    wattron(input_win, COLOR_PAIR(COLOR_SYSTEM_MSG));
    mvwprintw(input_win, 1, 2,
              "/msg <логин> <текст>  |  /group <группа> <текст>  |  /quit");
    wattroff(input_win, COLOR_PAIR(COLOR_SYSTEM_MSG));
    wrefresh(input_win);
}

// ──────────────────────────────────────────
//  ui_print_message
//
//  Добавляет строку в окно чата.
//  Формат: [ЧЧ:ММ:СС] sender → dest: текст
//  Прокручивает окно вверх при заполнении.
//  Потокобезопасна.
// ──────────────────────────────────────────
void ui_print_message(const Message *msg)
{
    pthread_mutex_lock(&ui_mutex);

    // Форматируем время из timestamp
    char timebuf[10];
    struct tm *t = localtime(&msg->timestamp);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", t);

    // Определяем строку получателя
    char dest_str[48];
    if (msg->msg_type == MSG_GROUP) {
        snprintf(dest_str, sizeof(dest_str), "[группа: %s]", msg->dest.group_name);
    } else {
        snprintf(dest_str, sizeof(dest_str), "%s", msg->dest.receiver);
    }

    // Печатаем в окно чата (с отступом от рамки)
    // Временная метка
    wattron(chat_win, COLOR_PAIR(COLOR_TIMESTAMP));
    wprintw(chat_win, "\n [%s] ", timebuf);
    wattroff(chat_win, COLOR_PAIR(COLOR_TIMESTAMP));

    // Имя отправителя
    wattron(chat_win, A_BOLD | COLOR_PAIR(COLOR_SENDER));
    wprintw(chat_win, "%s", msg->sender);
    wattroff(chat_win, A_BOLD | COLOR_PAIR(COLOR_SENDER));

    // Стрелка и получатель
    wprintw(chat_win, " → %s: ", dest_str);

    // Текст сообщения
    wprintw(chat_win, "%s", msg->text);

    wrefresh(chat_win);

    pthread_mutex_unlock(&ui_mutex);
}

// ──────────────────────────────────────────
//  ui_print_system
//
//  Системные уведомления — жёлтым цветом.
// ──────────────────────────────────────────
void ui_print_system(const char *text)
{
    pthread_mutex_lock(&ui_mutex);

    wattron(chat_win, COLOR_PAIR(COLOR_SYSTEM_MSG) | A_BOLD);
    wprintw(chat_win, "\n [!] %s", text);
    wattroff(chat_win, COLOR_PAIR(COLOR_SYSTEM_MSG) | A_BOLD);
    wrefresh(chat_win);

    pthread_mutex_unlock(&ui_mutex);
}

// ──────────────────────────────────────────
//  ui_read_input
//
//  Читает строку из нижнего окна посимвольно.
//  Поддерживает:
//    - обычные символы (добавляются в буфер)
//    - Backspace / DEL (удаляет последний символ)
//    - Enter (завершает ввод)
//    - Ctrl+C / Ctrl+D (возвращает -1 → выход)
//
//  Возвращает длину строки или -1.
// ──────────────────────────────────────────
int ui_read_input(char *buf, int maxlen)
{
    int pos = 0;
    memset(buf, 0, maxlen);

    // Строка ввода — третья строка input_win (после рамки и подсказки)
    int input_row = 2;
    int input_col = 2;

    // Ставим курсор в поле ввода
    wmove(input_win, input_row, input_col);
    wclrtoeol(input_win); // очищаем строку ввода
    // Восстанавливаем правую рамку
    mvwaddch(input_win, input_row, COLS - 1, ACS_VLINE);
    wrefresh(input_win);

    keypad(input_win, TRUE);
    curs_set(1); // показываем курсор

    while (1) {
        int ch = wgetch(input_win);

        if (ch == '\n' || ch == KEY_ENTER) {
            // Подтверждение ввода
            buf[pos] = '\0';
            break;
        }

        if (ch == 3 || ch == 4) {
            // Ctrl+C или Ctrl+D — сигнал выхода
            curs_set(0);
            return -1;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            // Backspace — удаляем последний символ
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                // Обновляем отображение
                int cur_col = input_col + pos;
                mvwaddch(input_win, input_row, cur_col, ' ');
                wmove(input_win, input_row, cur_col);
                wrefresh(input_win);
            }
            continue;
        }

        if (ch >= 32 && ch < 256 && pos < maxlen - 1) {
            // Обычный печатаемый символ
            buf[pos++] = (char)ch;
            waddch(input_win, (chtype)ch);
            wrefresh(input_win);
        }
    }

    curs_set(0); // скрываем курсор

    // Очищаем строку ввода после отправки
    pthread_mutex_lock(&ui_mutex);
    wmove(input_win, input_row, input_col);
    wclrtoeol(input_win);
    mvwaddch(input_win, input_row, COLS - 1, ACS_VLINE);
    wrefresh(input_win);
    pthread_mutex_unlock(&ui_mutex);

    return pos;
}

// ──────────────────────────────────────────
//  ui_cleanup
//
//  Завершает ncurses, восстанавливает терминал.
//  Вызывать перед выходом из программы.
// ──────────────────────────────────────────
void ui_cleanup()
{
    if (chat_win)  { delwin(chat_win);  chat_win  = nullptr; }
    if (input_win) { delwin(input_win); input_win = nullptr; }
    endwin();
}
