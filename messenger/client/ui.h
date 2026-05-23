#pragma once

#include "../common/message.h"

// Инициализировать UI (печатает шапку, настраивает локаль)
void ui_init();

// Вывести входящее сообщение в чат
void ui_print_message(const Message *msg);

// Вывести системное уведомление
void ui_print_system(const char *text);

// Прочитать строку ввода
// Возвращает длину строки или -1 при Ctrl+C/Ctrl+D
int ui_read_input(char *buf, int maxlen);

// Завершить UI, восстановить терминал
void ui_cleanup();