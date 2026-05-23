#include "ui.h"
#include "../common/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h> 

// ──────────────────────────────────────────
//  Аргументы потоков — объявлены здесь,
//  определены в sender.cpp и receiver.cpp
// ──────────────────────────────────────────
typedef struct {
    int  server_fd;
    char login[32];
} SenderArgs;

typedef struct {
    int server_fd;
} ReceiverArgs;

// Прототипы функций потоков
void *sender_thread(void *arg);
void *receiver_thread(void *arg);

// ──────────────────────────────────────────
//  connect_to_server
//
//  Создаёт TCP сокет и подключается к серверу.
//  Возвращает fd или -1 при ошибке.
// ──────────────────────────────────────────
static int connect_to_server(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[client] socket");
        return -1;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[client] Неверный IP-адрес: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[client] connect");
        close(fd);
        return -1;
    }

    return fd;
}

// ──────────────────────────────────────────
//  recv_line_fd — вспомогательная функция:
//  читает строку из сокета до '\n'
// ──────────────────────────────────────────
static int recv_line_fd(int fd, char *buf, int maxlen)
{
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        int n = (int)recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

// ──────────────────────────────────────────
//  do_auth
//
//  Проходит аутентификацию с сервером.
//  Протокол (согласован с server/auth.cpp):
//    ← "LOGIN"    → отправляем логин
//    ← "PASSWORD" → отправляем пароль
//    ← "OK" / "FAIL" / "BUSY"
//
//  Возвращает 0 при успехе, -1 при ошибке.
// ──────────────────────────────────────────
static int do_auth(int fd, const char *login, const char *password)
{
    char response[64];

    // Ждём "LOGIN"
    if (recv_line_fd(fd, response, sizeof(response)) < 0) return -1;
    if (strncmp(response, "LOGIN", 5) != 0) {
        if (strncmp(response, "BUSY", 4) == 0) {
            fprintf(stderr, "[client] Сервер занят, попробуйте позже\n");
        }
        return -1;
    }

    // Отправляем логин
    char line[64];
    snprintf(line, sizeof(line), "%s\n", login);
    send(fd, line, strlen(line), 0);

    // Ждём "PASSWORD"
    if (recv_line_fd(fd, response, sizeof(response)) < 0) return -1;
    if (strncmp(response, "PASSWORD", 8) != 0) return -1;

    // Отправляем пароль
    snprintf(line, sizeof(line), "%s\n", password);
    send(fd, line, strlen(line), 0);

    // Ждём "OK" или "FAIL"
    if (recv_line_fd(fd, response, sizeof(response)) < 0) return -1;
    if (strncmp(response, "OK", 2) == 0) return 0;

    fprintf(stderr, "[client] Неверный логин или пароль\n");
    return -1;
}

// ──────────────────────────────────────────
//  main
// ──────────────────────────────────────────
int main(int argc, char *argv[])
{
    // ── Разбор аргументов командной строки ──
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <IP-сервера> <порт>\n", argv[0]);
        fprintf(stderr, "Пример: %s 192.168.1.10 8080\n", argv[0]);
        return 1;
    }

    const char *server_ip   = argv[1];
    int         server_port = atoi(argv[2]);

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "[client] Неверный порт: %s\n", argv[2]);
        return 1;
    }

    // ── Запрос логина и пароля (до ncurses — в обычном терминале) ──
    char login[32]    = {0};
    char password[64] = {0};

    printf("Подключение к %s:%d\n", server_ip, server_port);
    printf("Логин   : ");
    fflush(stdout);
    if (!fgets(login, sizeof(login), stdin)) return 1;
    login[strcspn(login, "\n")] = '\0';

    printf("Пароль  : ");
    fflush(stdout);
    // Отключаем эхо для пароля
    system("stty -echo");
    if (!fgets(password, sizeof(password), stdin)) {
        system("stty echo");
        return 1;
    }
    system("stty echo");
    printf("\n");
    password[strcspn(password, "\n")] = '\0';

    // ── Подключение к серверу ──
    int server_fd = connect_to_server(server_ip, server_port);
    if (server_fd < 0) {
        fprintf(stderr, "[client] Не удалось подключиться к серверу\n");
        return 1;
    }

    // ── Аутентификация ──
    if (do_auth(server_fd, login, password) < 0) {
        close(server_fd);
        return 1;
    }

    printf("Успешный вход. Запуск интерфейса...\n");
    sleep(1); // небольшая пауза перед запуском ncurses

    // ── Инициализация ncurses UI ──
    ui_init();
    ui_print_system("Добро пожаловать! Введите /msg <логин> <текст> для начала");

    // ── Подготовка аргументов для потоков ──
    static SenderArgs   s_args;
    static ReceiverArgs r_args;

    s_args.server_fd = server_fd;
    strncpy(s_args.login, login, sizeof(s_args.login) - 1);

    r_args.server_fd = server_fd;

    // ── Запуск двух потоков ──
    pthread_t sender_tid, receiver_tid;

    if (pthread_create(&receiver_tid, nullptr, receiver_thread, &r_args) != 0) {
        ui_cleanup();
        fprintf(stderr, "[client] Ошибка создания receiver_thread\n");
        close(server_fd);
        return 1;
    }

    if (pthread_create(&sender_tid, nullptr, sender_thread, &s_args) != 0) {
        ui_cleanup();
        fprintf(stderr, "[client] Ошибка создания sender_thread\n");
        close(server_fd);
        return 1;
    }

    // ── Ждём завершения потоков ──
    // sender_thread завершается по /quit или Ctrl+C
    // receiver_thread завершается при разрыве соединения
    pthread_join(sender_tid, nullptr);
    pthread_join(receiver_tid, nullptr);

    // ── Завершение ──
    ui_cleanup();
    close(server_fd);

    printf("До свидания!\n");
    return 0;
}
