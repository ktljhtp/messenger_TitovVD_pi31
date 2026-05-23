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

typedef struct {
    int  server_fd;
    char login[32];
} SenderArgs;

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

// Шифрует text и кладёт в msg: первые 4 байта = длина, дальше шифротекст
static int fill_encrypted(Message *msg, const char *text)
{
    uint8_t cipher[MSG_TEXT_MAX + 16] = {0};
    int clen = encrypt_message(
        (const uint8_t *)text, (int)strlen(text),
        SESSION_KEY, SESSION_IV, cipher
    );
    if (clen <= 0 || clen > MSG_TEXT_MAX) {
        ui_print_system("Ошибка шифрования");
        return -1;
    }
    msg_set_clen(msg, clen);
    memcpy(msg_cipher_ptr(msg), cipher, clen);
    return 0;
}

static int parse_and_send(const char *input, int server_fd, const char *login)
{
    if (strlen(input) == 0) return 0;

    Message msg = {};
    msg.timestamp = time(nullptr);
    strncpy(msg.sender, login, sizeof(msg.sender) - 1);

    char buf[1100];
    strncpy(buf, input, sizeof(buf) - 1);

    if (strcmp(buf, "/quit") == 0) return -1;

    if (strncmp(buf, "/msg ", 5) == 0) {
        char *rest  = buf + 5;
        char *space = strchr(rest, ' ');
        if (!space) { ui_print_system("Использование: /msg <логин> <текст>"); return 0; }
        *space = '\0';
        msg.msg_type = MSG_PERSONAL;
        strncpy(msg.dest.receiver, rest, sizeof(msg.dest.receiver) - 1);
        if (fill_encrypted(&msg, space + 1) < 0) return 0;

    } else if (strncmp(buf, "/group ", 7) == 0) {
        char *rest  = buf + 7;
        char *space = strchr(rest, ' ');
        if (!space) { ui_print_system("Использование: /group <группа> <текст>"); return 0; }
        *space = '\0';
        msg.msg_type = MSG_GROUP;
        strncpy(msg.dest.group_name, rest, sizeof(msg.dest.group_name) - 1);
        if (fill_encrypted(&msg, space + 1) < 0) return 0;

    } else if (strncmp(buf, "/history ", 9) == 0) {
        msg.msg_type = MSG_SYSTEM;
        strncpy(msg.dest.receiver, buf + 9, sizeof(msg.dest.receiver) - 1);
        snprintf(msg.text, sizeof(msg.text), "HISTORY:%s", buf + 9);

    } else if (strcmp(buf, "/users") == 0) {
        msg.msg_type = MSG_SYSTEM;
        strncpy(msg.dest.receiver, "server", sizeof(msg.dest.receiver) - 1);
        strncpy(msg.text, "USERS", sizeof(msg.text) - 1);

    } else if (buf[0] == '/') {
        ui_print_system("Неизвестная команда. Доступны: /msg /group /history /users /quit");
        return 0;
    } else {
        ui_print_system("Введите команду. Пример: /msg alice Привет!");
        return 0;
    }

    ssize_t sent = send(server_fd, &msg, sizeof(Message), 0);
    if (sent != (ssize_t)sizeof(Message)) {
        ui_print_system("Ошибка отправки — соединение разорвано");
        return -1;
    }
    return 0;
}

void *sender_thread(void *arg)
{
    SenderArgs *a = (SenderArgs *)arg;
    char input[1100];

    ui_print_system("Введите /msg <логин> <текст> для отправки сообщения");

    while (1) {
        int len = ui_read_input(input, sizeof(input));
        if (len < 0) break;
        if (parse_and_send(input, a->server_fd, a->login) < 0) break;
    }

    ui_print_system("Выход...");
    shutdown(a->server_fd, SHUT_RDWR);
    return nullptr;
}