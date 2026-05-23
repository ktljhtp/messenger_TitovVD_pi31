#pragma once

#include <netinet/in.h>
#include "../common/message.h"

// ──────────────────────────────────────────
//  Модуль сессии
//
//  handle_client — точка входа дочернего процесса.
//  route_message — маршрутизация пакета через FIFO.
// ──────────────────────────────────────────

// Путь к файлу групп
#define GROUPS_DAT   "data/groups.dat"

// Шаблон имени FIFO для каждого пользователя
#define FIFO_PATH    "/tmp/fifo_%s"
#define FIFO_MAXPATH 64

// handle_client — обслуживает одного клиента в дочернем процессе:
//   1. Аутентификация
//   2. Создание FIFO /tmp/fifo_<логин>
//   3. Цикл: recv от клиента → route_message,
//            одновременно читает из собственного FIFO → send клиенту
//   4. При разрыве — удаляет FIFO, завершается
void handle_client(int client_fd, struct sockaddr_in addr);

// route_message — открывает FIFO получателя (или всех участников группы)
//   и записывает туда структуру Message.
//   Возвращает 0 при успехе, -1 если получатель не в сети.
int route_message(const Message *msg);
