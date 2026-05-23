#include "ui.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

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
        if (n <= 0) { ui_print_system("Соединение с сервером разорвано"); break; }
        if (n != (ssize_t)sizeof(Message)) { ui_print_system("Неполный пакет"); continue; }

        if (msg.msg_type == MSG_SYSTEM) {
            // Системные сообщения — plaintext в text[] с нуля
            ui_print_system(msg.text);
            continue;
        }

        // Читаем длину шифротекста из первых 4 байт
        int clen = msg_get_clen(&msg);
        if (clen <= 0 || clen > MSG_TEXT_MAX) {
            ui_print_system("Получено сообщение с неверной длиной шифротекста");
            continue;
        }

        uint8_t plaintext[MSG_TEXT_MAX + 16] = {0};
        int plen = decrypt_message(
            msg_cipher_cptr(&msg), clen,
            SESSION_KEY, SESSION_IV, plaintext
        );

        if (plen > 0) {
            // Заменяем text[] расшифрованной строкой для передачи в ui
            memset(msg.text, 0, sizeof(msg.text));
            int copy = plen < (int)sizeof(msg.text) - 1 ? plen : (int)sizeof(msg.text) - 1;
            memcpy(msg.text, plaintext, copy);
        } else {
            snprintf(msg.text, sizeof(msg.text), "[ошибка расшифровки]");
        }

        ui_print_message(&msg);
    }
    return nullptr;
}