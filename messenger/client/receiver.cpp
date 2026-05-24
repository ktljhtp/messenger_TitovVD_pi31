#include "ui.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <string.h>
#include <sys/socket.h>
#include <stdio.h>

typedef struct { int server_fd; } ReceiverArgs;

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

void *receiver_thread(void *arg)
{
    ReceiverArgs *a = (ReceiverArgs *)arg;
    Message msg;

    while (1) {
        ssize_t n = recv(a->server_fd, &msg, sizeof(Message), MSG_WAITALL);
        if (n <= 0) break;
        if (n != (ssize_t)sizeof(Message)) continue;

        if (msg.msg_type == MSG_SYSTEM) {
            // Переводим серверные коды в понятные сообщения
            const char *text = msg.text;
            const char *display = nullptr;
            char tmp[512] = {0};

            if      (strcmp(text, "OK_NEWUSER")    == 0) display = "✓ Пользователь создан";
            else if (strcmp(text, "OK_NEWGROUP")   == 0) display = "✓ Группа создана";
            else if (strcmp(text, "OK_ADDMEMBER")  == 0) display = "✓ Участник добавлен в группу";
            else if (strcmp(text, "ERR_EXISTS")    == 0) display = "✗ Уже существует";
            else if (strcmp(text, "ERR_NOGROUP")   == 0) display = "✗ Группа не найдена";
            else if (strcmp(text, "ERR_BADFORMAT") == 0) display = "✗ Неверный формат команды";
            else if (strcmp(text, "ERR_SERVER")    == 0) display = "✗ Ошибка сервера";
            else if (strncmp(text, "USERS:", 6) == 0) {
                // Форматируем список пользователей
                snprintf(tmp, sizeof(tmp), "Пользователи: %s", text + 6);
                // Заменяем : на ", "
                for (char *p = tmp + 14; *p; p++) if (*p == ':') { *p = ' '; }
                display = tmp;
            }
            else if (strncmp(text, "GROUPS:", 7) == 0) {
                snprintf(tmp, sizeof(tmp), "Группы: %s", text + 7);
                for (char *p = tmp + 8; *p; p++) if (*p == ':') { *p = ' '; }
                display = tmp;
            }
            else display = text; // неизвестное — показываем как есть

            printf("\n\033[33m [!] %s\033[0m\n", display);
            fflush(stdout);
            continue;
        }

        // Расшифровываем текст
        int clen = msg_get_clen(&msg);
        if (clen <= 0 || clen > MSG_TEXT_MAX) continue;

        uint8_t plaintext[MSG_TEXT_MAX + 16] = {0};
        int plen = decrypt_message(
            msg_cipher_cptr(&msg), clen,
            SESSION_KEY, SESSION_IV, plaintext
        );

        if (plen <= 0) continue;

        // Подставляем расшифрованный текст
        memset(msg.text, 0, sizeof(msg.text));
        int copy = plen < (int)sizeof(msg.text) - 1 ? plen : (int)sizeof(msg.text) - 1;
        memcpy(msg.text, plaintext, copy);

        // Передаём в UI — он сам разберёт в какой чат положить
        ui_receive_message(&msg);
    }

    return nullptr;
}