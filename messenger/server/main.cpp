#include "session.h"
#include "logger.h"
#include "../common/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ──────────────────────────────────────────
//  Глобальные переменные
//
//  active_children — счётчик живых дочерних процессов
//  children_pids   — массив PID для рассылки SIGHUP при SIGTERM
// ──────────────────────────────────────────
static volatile int active_children = 0;
static pid_t children_pids[MAX_CLIENTS] = {0};
static int   server_fd = -1; // глобально для доступа из обработчиков

// ──────────────────────────────────────────
//  add_child / remove_child — управление массивом PID
// ──────────────────────────────────────────
static void add_child(pid_t pid)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (children_pids[i] == 0) {
            children_pids[i] = pid;
            active_children++;
            return;
        }
    }
}

static void remove_child(pid_t pid)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (children_pids[i] == pid) {
            children_pids[i] = 0;
            active_children--;
            return;
        }
    }
}

// ──────────────────────────────────────────
//  sigchld_handler
//
//  Вызывается когда дочерний процесс завершился.
//  waitpid(WNOHANG) подбирает зомби-процессы,
//  уменьшает счётчик active_children.
// ──────────────────────────────────────────
static void sigchld_handler(int /*sig*/)
{
    int saved_errno = errno;
    pid_t pid;
    int status;

    // Цикл — могли завершиться несколько процессов одновременно
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        remove_child(pid);
        log_event("Дочерний процесс PID=%d завершился (активных: %d)",
                  (int)pid, active_children);
    }
    errno = saved_errno;
}

// ──────────────────────────────────────────
//  sigterm_handler
//
//  Корректное завершение сервера:
//  1. Рассылает SIGHUP всем дочерним процессам
//     (клиенты получат уведомление "сервер остановлен")
//  2. Закрывает listening-сокет
//  3. Завершает процесс
// ──────────────────────────────────────────
static void sigterm_handler(int /*sig*/)
{
    log_event("Получен SIGTERM — завершаем сервер");

    // Уведомляем дочерние процессы
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (children_pids[i] > 0) {
            kill(children_pids[i], SIGHUP);
        }
    }

    // Даём время на завершение
    sleep(1);

    if (server_fd >= 0) close(server_fd);
    log_close();
    exit(0);
}

// ──────────────────────────────────────────
//  sighup_child_handler
//
//  Обработчик SIGHUP в дочернем процессе:
//  уведомляет клиента и завершается.
//  (Устанавливается после fork() в дочернем)
// ──────────────────────────────────────────
static void sighup_child_handler(int /*sig*/)
{
    // Этот обработчик устанавливается в дочернем процессе
    // Реальное уведомление клиента делается в session.cpp
    // Здесь просто выходим — cleanup произойдёт в handle_client
    _exit(0);
}

// ──────────────────────────────────────────
//  create_server_socket
//
//  Создаёт TCP сокет, привязывает к port,
//  переводит в режим прослушивания.
//  Возвращает fd или -1 при ошибке.
// ──────────────────────────────────────────
static int create_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[server] socket");
        return -1;
    }

    // Разрешаем повторное использование порта после перезапуска
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // слушаем на всех интерфейсах
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[server] bind");
        close(fd);
        return -1;
    }

    // Очередь до 10 ожидающих подключений
    if (listen(fd, MAX_CLIENTS) < 0) {
        perror("[server] listen");
        close(fd);
        return -1;
    }

    return fd;
}

// ──────────────────────────────────────────
//  main
// ──────────────────────────────────────────
int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Использование: %s [порт]\n", argv[0]);
            return 1;
        }
    }

    // Открываем лог
    log_open("data/server.log");
    log_event("Сервер запускается на порту %d", port);

    // Устанавливаем обработчики сигналов
    struct sigaction sa_chld = {};
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART; // не прерывать accept()
    sigaction(SIGCHLD, &sa_chld, nullptr);

    struct sigaction sa_term = {};
    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, nullptr);
    sigaction(SIGINT,  &sa_term, nullptr); // Ctrl+C тоже корректно завершает

    // Создаём слушающий сокет
    server_fd = create_server_socket(port);
    if (server_fd < 0) {
        log_event("КРИТИЧНО: не удалось создать сокет");
        log_close();
        return 1;
    }

    log_event("Сервер готов, ожидаем подключений...");
    printf("[server] Запущен на порту %d. Лог: data/server.log\n", port);

    // ── Основной цикл accept() ──
    while (1) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue; // прерван сигналом — нормально
            log_event("accept() ошибка: %s", strerror(errno));
            continue;
        }

        // ── Проверяем лимит подключений ──
        if (active_children >= MAX_CLIENTS) {
            // Уведомляем клиента и закрываем соединение
            const char *busy_msg = "BUSY: сервер занят, попробуйте позже\n";
            send(client_fd, busy_msg, strlen(busy_msg), 0);
            log_event("Лимит подключений достигнут (%d/%d) — отказ клиенту",
                      active_children, MAX_CLIENTS);
            close(client_fd);
            continue;
        }

        // ── fork(): создаём дочерний процесс ──
        pid_t pid = fork();

        if (pid < 0) {
            log_event("fork() ошибка: %s", strerror(errno));
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            // ── Дочерний процесс ──
            // Закрываем слушающий сокет (нам он не нужен)
            close(server_fd);

            // В дочернем устанавливаем обработчик SIGHUP
            signal(SIGHUP, sighup_child_handler);

            // Обслуживаем клиента (блокирующий вызов до разрыва)
            handle_client(client_fd, client_addr);

            log_close();
            _exit(0); // дочерний процесс завершается
        }

        // ── Родительский процесс ──
        close(client_fd); // клиентский сокет нужен только дочернему
        add_child(pid);
        log_event("Новый дочерний процесс PID=%d (активных: %d/%d)",
                  (int)pid, active_children, MAX_CLIENTS);
    }

    // Сюда не доходим в нормальной работе
    close(server_fd);
    log_close();
    return 0;
}
