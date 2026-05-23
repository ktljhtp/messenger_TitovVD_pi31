#include "session.h"
#include "auth.h"
#include "logger.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

// ──────────────────────────────────────────
//  AES ключ и IV — в реальном проекте генерируются
//  при установке соединения и передаются через RSA.
//  Здесь используем фиксированные для упрощения.
// ──────────────────────────────────────────
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
//  save_to_history — дозаписывает Message в history.dat
// ──────────────────────────────────────────
static void save_to_history(const Message *msg)
{
    FILE *f = fopen("data/history.dat", "ab");
    if (!f) return;
    fwrite(msg, sizeof(Message), 1, f);
    fclose(f);
}

// ──────────────────────────────────────────
//  route_message
//
//  MSG_PERSONAL (0): открывает /tmp/fifo_<dest.receiver>
//    и записывает Message.
//  MSG_GROUP (2): читает data/groups.dat, для каждого
//    участника группы открывает его FIFO и записывает.
//  MSG_SYSTEM (1): не маршрутизируется здесь.
//
//  Возвращает 0 при успехе, -1 при ошибке.
// ──────────────────────────────────────────
int route_message(const Message *msg)
{
    if (msg->msg_type == MSG_PERSONAL || msg->msg_type == MSG_SYSTEM) {
        // Личное сообщение → один получатель
        char fifo_path[FIFO_MAXPATH];
        snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, msg->dest.receiver);

        // Открываем FIFO без блокировки (O_NONBLOCK):
        // если получатель не в сети — FIFO не существует → вернём -1
        int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            log_event("Получатель %s не в сети (FIFO недоступен)",
                      msg->dest.receiver);
            return -1;
        }

        ssize_t written = write(fd, msg, sizeof(Message));
        close(fd);

        if (written != (ssize_t)sizeof(Message)) {
            log_event("Ошибка записи в FIFO получателя %s",
                      msg->dest.receiver);
            return -1;
        }
        return 0;

    } else if (msg->msg_type == MSG_GROUP) {
        // Групповое сообщение → все участники из groups.dat
        FILE *f = fopen(GROUPS_DAT, "r");
        if (!f) {
            log_event("ОШИБКА: не удалось открыть %s", GROUPS_DAT);
            return -1;
        }

        char line[512];
        int delivered = 0;

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';

            // Первый токен — имя группы
            char *group = strtok(line, ":");
            if (!group || strcmp(group, msg->dest.group_name) != 0) {
                continue; // не та группа
            }

            // Остальные токены — логины участников
            char *member;
            while ((member = strtok(nullptr, ":")) != nullptr) {
                // Не отправляем отправителю самому себе
                if (strcmp(member, msg->sender) == 0) continue;

                char fifo_path[FIFO_MAXPATH];
                snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, member);

                int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if (fd < 0) continue; // участник не в сети — пропускаем

                write(fd, msg, sizeof(Message));
                close(fd);
                delivered++;
            }
            break; // нашли группу — выходим
        }
        fclose(f);

        log_event("Групповое сообщение в '%s' от %s: доставлено %d участникам",
                  msg->dest.group_name, msg->sender, delivered);
        return (delivered > 0) ? 0 : -1;
    }

    return -1;
}

// ──────────────────────────────────────────
//  handle_client
//
//  Выполняется в дочернем процессе (после fork).
//  Полный жизненный цикл одной сессии:
//    1. Аутентификация
//    2. Создание FIFO
//    3. Цикл select(): сокет (входящие от клиента)
//                    + FIFO  (входящие от других)
//    4. Завершение: unlink FIFO, закрыть сокет
// ──────────────────────────────────────────
void handle_client(int client_fd, struct sockaddr_in addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));

    log_event("Новое подключение с %s", client_ip);

    // ── 1. Аутентификация ──
    char login[32] = {0};
    if (authenticate(client_fd, login) < 0) {
        log_event("Отказ в доступе для клиента %s", client_ip);
        close(client_fd);
        return;
    }
    log_event("Пользователь '%s' вошёл (%s)", login, client_ip);

    // ── 2. Создаём FIFO для этого пользователя ──
    char fifo_path[FIFO_MAXPATH];
    snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, login);

    // Удаляем старый FIFO если остался от прошлой сессии
    unlink(fifo_path);

    if (mkfifo(fifo_path, 0666) < 0) {
        log_event("ОШИБКА mkfifo для %s: %s", login, strerror(errno));
        close(client_fd);
        return;
    }

    // Открываем FIFO на чтение (неблокирующий режим)
    // O_RDWR чтобы open не блокировался в ожидании writer'а
    int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        log_event("ОШИБКА open FIFO для %s: %s", login, strerror(errno));
        unlink(fifo_path);
        close(client_fd);
        return;
    }

    // ── 3. Основной цикл: select на сокет и FIFO ──
    Message msg;
    fd_set read_fds;
    int maxfd = (client_fd > fifo_fd ? client_fd : fifo_fd) + 1;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds); // данные от клиента
        FD_SET(fifo_fd,   &read_fds); // данные от других пользователей

        int ready = select(maxfd, &read_fds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) continue; // прерван сигналом — нормально
            log_event("select() ошибка для '%s': %s", login, strerror(errno));
            break;
        }

        // ── Данные от клиента (входящее сообщение) ──
        if (FD_ISSET(client_fd, &read_fds)) {
            ssize_t n = recv(client_fd, &msg, sizeof(Message), MSG_WAITALL);
            if (n <= 0) {
                // Клиент отключился
                log_event("Пользователь '%s' отключился", login);
                break;
            }

            // Расшифровываем текст сообщения
            uint8_t plaintext[1040] = {0};
            int plen = decrypt_message(
                (uint8_t *)msg.text, (int)strlen(msg.text),
                SESSION_KEY, SESSION_IV, plaintext
            );
            if (plen > 0) {
                memset(msg.text, 0, sizeof(msg.text));
                memcpy(msg.text, plaintext,
                       plen < (int)sizeof(msg.text) - 1 ? plen : (int)sizeof(msg.text) - 1);
            }

            log_event("Сообщение от '%s' тип=%d", login, msg.msg_type);

            // Сохраняем в историю
            save_to_history(&msg);

            // Маршрутизируем
            if (route_message(&msg) < 0) {
                // Получатель не в сети — уведомляем отправителя
                Message sys = {};
                sys.msg_type = MSG_SYSTEM;
                strncpy(sys.sender, "server", sizeof(sys.sender) - 1);
                strncpy(sys.dest.receiver, login, sizeof(sys.dest.receiver) - 1);
                sys.timestamp = time(nullptr);

                if (msg.msg_type == MSG_PERSONAL) {
                    snprintf(sys.text, sizeof(sys.text),
                             "Пользователь '%s' не в сети",
                             msg.dest.receiver);
                } else {
                    snprintf(sys.text, sizeof(sys.text),
                             "Группа '%s' недоступна или нет участников онлайн",
                             msg.dest.group_name);
                }
                send(client_fd, &sys, sizeof(Message), 0);
            }
        }

        // ── Данные из FIFO (сообщение от другого пользователя) ──
        if (FD_ISSET(fifo_fd, &read_fds)) {
            Message incoming;
            ssize_t n = read(fifo_fd, &incoming, sizeof(Message));
            if (n == (ssize_t)sizeof(Message)) {
                // Пересылаем клиенту напрямую
                send(client_fd, &incoming, sizeof(Message), 0);
            }
        }
    }

    // ── 4. Завершение сессии ──
    close(fifo_fd);
    unlink(fifo_path);
    close(client_fd);
    log_event("Сессия '%s' завершена", login);
}
