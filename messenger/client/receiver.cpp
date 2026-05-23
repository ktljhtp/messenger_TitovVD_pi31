#include "ui.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

// ──────────────────────────────────────────
//  Аргументы потока приёма
// ──────────────────────────────────────────
typedef struct {
    int server_fd; // сокет соединения с сервером
} ReceiverArgs;

// Тот же ключ и IV что на сервере
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
//  receiver_thread
//
//  Блокируется на recv() — ждёт полную структуру
//  Message (MSG_WAITALL гарантирует приём всех байт).
//
//  При получении:
//    - если MSG_SYSTEM → ui_print_system (текст без расшифровки)
//    - иначе → расшифровываем text, вызываем ui_print_message
//
//  При разрыве (recv вернул 0 или -1) — выходим.
// ──────────────────────────────────────────
void *receiver_thread(void *arg)
{
    ReceiverArgs *a = (ReceiverArgs *)arg;
    Message msg;

    while (1) {
        // Читаем ровно sizeof(Message) байт за раз
        ssize_t n = recv(a->server_fd, &msg, sizeof(Message), MSG_WAITALL);

        if (n <= 0) {
            // 0 = сервер закрыл соединение
            // -1 = ошибка (в т.ч. shutdown из sender_thread при /quit)
            ui_print_system("Соединение с сервером разорвано");
            break;
        }

        if (n != (ssize_t)sizeof(Message)) {
            // Получили неполный пакет — пропускаем
            ui_print_system("Получен неполный пакет — пропуск");
            continue;
        }

        if (msg.msg_type == MSG_SYSTEM) {
            // Системные сообщения от сервера (уведомления, ошибки)
            // передаём напрямую без расшифровки
            ui_print_system(msg.text);
        } else {
            // Личное или групповое — расшифровываем текст
            uint8_t plaintext[1040] = {0};
            int plen = decrypt_message(
                (uint8_t *)msg.text, (int)strnlen(msg.text, sizeof(msg.text)),
                SESSION_KEY, SESSION_IV, plaintext
            );

            if (plen > 0) {
                // Подставляем расшифрованный текст обратно в структуру
                memset(msg.text, 0, sizeof(msg.text));
                memcpy(msg.text, plaintext,
                       plen < (int)sizeof(msg.text) - 1
                       ? plen : (int)sizeof(msg.text) - 1);
            }
            // Даже если расшифровка не удалась — показываем то что есть
            ui_print_message(&msg);
        }
    }

    return nullptr;
}
