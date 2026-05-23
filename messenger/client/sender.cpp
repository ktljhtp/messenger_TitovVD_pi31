#include "ui.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <pthread.h>

// ──────────────────────────────────────────
//  Аргументы потока отправки
// ──────────────────────────────────────────
typedef struct {
    int   server_fd; // сокет соединения с сервером
    char  login[32]; // логин текущего пользователя
} SenderArgs;

// Тот же ключ и IV что на сервере (в реальном проекте — обмен через RSA)
static const uint8_t SESSION_KEY[AES_KEY_LEN] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
};
static const uint8_t SESSION_IV[AES_IV_LEN] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
};

// ──────────────────────────────────────────
//  parse_and_send
//
//  Разбирает введённую строку, формирует Message,
//  шифрует текст и отправляет серверу.
//
//  Поддерживаемые команды:
//    /msg <логин> <текст>     → MSG_PERSONAL
//    /group <группа> <текст>  → MSG_GROUP
//    /history <логин>         → MSG_SYSTEM (запрос истории)
//    /users                   → MSG_SYSTEM (список онлайн)
//    /quit                    → возвращает -1 (выход)
//
//  Возвращает 0 при успехе, -1 при команде /quit или ошибке.
// ──────────────────────────────────────────
static int parse_and_send(const char *input, int server_fd, const char *login)
{
    if (strlen(input) == 0) return 0;

    Message msg = {};                              // обнуляем всю структуру
    msg.timestamp = time(nullptr);
    strncpy(msg.sender, login, sizeof(msg.sender) - 1);

    char buf[1100];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // ── /quit ──
    if (strcmp(buf, "/quit") == 0) {
        return -1;
    }

    // ── /msg <логин> <текст> ──
    if (strncmp(buf, "/msg ", 5) == 0) {
        char *rest = buf + 5;                      // пропускаем "/msg "
        char *space = strchr(rest, ' ');
        if (!space) {
            ui_print_system("Использование: /msg <логин> <текст>");
            return 0;
        }
        *space = '\0';                             // разделяем логин и текст
        char *receiver = rest;
        char *text     = space + 1;

        msg.msg_type = MSG_PERSONAL;
        strncpy(msg.dest.receiver, receiver, sizeof(msg.dest.receiver) - 1);

        // Шифруем текст
        uint8_t cipher[1040] = {0};
        int clen = encrypt_message(
            (uint8_t *)text, (int)strlen(text),
            SESSION_KEY, SESSION_IV, cipher
        );
        if (clen < 0) {
            ui_print_system("Ошибка шифрования сообщения");
            return 0;
        }
        memcpy(msg.text, cipher, clen < (int)sizeof(msg.text) ? clen : (int)sizeof(msg.text) - 1);

    // ── /group <группа> <текст> ──
    } else if (strncmp(buf, "/group ", 7) == 0) {
        char *rest  = buf + 7;
        char *space = strchr(rest, ' ');
        if (!space) {
            ui_print_system("Использование: /group <группа> <текст>");
            return 0;
        }
        *space = '\0';
        char *group_name = rest;
        char *text       = space + 1;

        msg.msg_type = MSG_GROUP;
        // ВАЖНО: заполняем dest.group_name, а не dest.receiver!
        strncpy(msg.dest.group_name, group_name, sizeof(msg.dest.group_name) - 1);

        uint8_t cipher[1040] = {0};
        int clen = encrypt_message(
            (uint8_t *)text, (int)strlen(text),
            SESSION_KEY, SESSION_IV, cipher
        );
        if (clen < 0) {
            ui_print_system("Ошибка шифрования сообщения");
            return 0;
        }
        memcpy(msg.text, cipher, clen < (int)sizeof(msg.text) ? clen : (int)sizeof(msg.text) - 1);

    // ── /history <логин> ──
    } else if (strncmp(buf, "/history ", 9) == 0) {
        char *target = buf + 9;
        msg.msg_type = MSG_SYSTEM;
        strncpy(msg.dest.receiver, target, sizeof(msg.dest.receiver) - 1);
        snprintf(msg.text, sizeof(msg.text), "HISTORY:%s", target);

    // ── /users ──
    } else if (strcmp(buf, "/users") == 0) {
        msg.msg_type = MSG_SYSTEM;
        strncpy(msg.dest.receiver, "server", sizeof(msg.dest.receiver) - 1);
        strncpy(msg.text, "USERS", sizeof(msg.text) - 1);

    // ── Неизвестная команда или текст без команды ──
    } else if (buf[0] == '/') {
        ui_print_system("Неизвестная команда. Доступны: /msg /group /history /users /quit");
        return 0;
    } else {
        ui_print_system("Введите команду. Пример: /msg alice Привет!");
        return 0;
    }

    // Отправляем всю структуру целиком
    ssize_t sent = send(server_fd, &msg, sizeof(Message), 0);
    if (sent != (ssize_t)sizeof(Message)) {
        ui_print_system("Ошибка отправки — соединение разорвано");
        return -1;
    }

    return 0;
}

// ──────────────────────────────────────────
//  sender_thread
//
//  Функция потока.
//  Цикл: читает строку из UI → парсит → отправляет.
//  Завершается при /quit или ошибке соединения.
// ──────────────────────────────────────────
void *sender_thread(void *arg)
{
    SenderArgs *a = (SenderArgs *)arg;
    char input[1100];

    ui_print_system("Введите /msg <логин> <текст> для отправки сообщения");

    while (1) {
        int len = ui_read_input(input, sizeof(input));

        if (len < 0) {
            // Ctrl+C или Ctrl+D — выход
            break;
        }

        if (parse_and_send(input, a->server_fd, a->login) < 0) {
            // /quit или ошибка соединения
            break;
        }
    }

    ui_print_system("Выход... Нажмите любую клавишу");
    shutdown(a->server_fd, SHUT_RDWR); // разбудит receiver_thread
    return nullptr;
}
