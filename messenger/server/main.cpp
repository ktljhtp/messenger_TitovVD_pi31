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
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

// ──────────────────────────────────────────
//  Глобальные переменные
//
//  active_children — счётчик живых дочерних процессов
//  children_pids   — массив PID для рассылки SIGHUP при SIGTERM
// ──────────────────────────────────────────
static volatile int active_children = 0;
static pid_t children_pids[MAX_CLIENTS] = {0};
static int   server_fd  = -1; // глобально для доступа из обработчиков
static int   child_client_fd = -1; // fd клиента в дочернем процессе

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
//  sigusr1_handler — перечитывание конфигурации
//
//  Сигнал: kill -USR1 <PID сервера>
//  Действие: сервер перечитывает data/users.dat и
//  data/groups.dat без перезапуска.
//  Изменения применяются для следующих подключений.
//  Уже подключённые клиенты не затрагиваются.
// ──────────────────────────────────────────
static volatile int reload_requested = 0;

static void sigusr1_handler(int /*sig*/)
{
    reload_requested = 1; // атомарный флаг — обрабатываем в основном цикле
}

static void do_reload()
{
    reload_requested = 0;
    // Проверяем что файлы конфигурации доступны
    FILE *fu = fopen("data/users.dat", "r");
    FILE *fg = fopen("data/groups.dat", "r");

    if (fu && fg) {
        fclose(fu);
        fclose(fg);
        log_event("RELOAD: конфигурация перечитана (users.dat, groups.dat)");
        printf("\033[32m[server] Конфигурация перечитана (SIGUSR1)\033[0m\n");
    } else {
        log_event("RELOAD: ошибка — файлы конфигурации недоступны");
        printf("\033[31m[server] Ошибка перечитывания конфигурации\033[0m\n");
        if (fu) fclose(fu);
        if (fg) fclose(fg);
    }
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
    // Отправляем клиенту правильную структуру Message с системным уведомлением.
    // Клиент ожидает ровно sizeof(Message) байт — нельзя слать произвольную строку.
    if (child_client_fd >= 0) {
        Message msg = {};
        msg.msg_type  = MSG_SYSTEM;
        msg.timestamp = time(nullptr);
        strncpy(msg.sender,        "server",           sizeof(msg.sender) - 1);
        strncpy(msg.dest.receiver, "client",           sizeof(msg.dest.receiver) - 1);
        strncpy(msg.text,          "Сервер пал, милорд. Соединение разорвано.",
                sizeof(msg.text) - 1);

        // MSG_NOSIGNAL — не генерировать SIGPIPE если клиент уже закрыл сокет
        send(child_client_fd, &msg, sizeof(Message), MSG_NOSIGNAL);
        close(child_client_fd);
        child_client_fd = -1;
    }
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


// ──────────────────────────────────────────
//  broadcast_reauth
//
//  Вызывается при старте сервера.
//  Сканирует /tmp/fifo_* — это FIFO активных
//  клиентских сессий от предыдущего запуска.
//  Отправляет каждому Message с требованием
//  переавторизации, затем удаляет FIFO.
//
//  После этого клиенты увидят сообщение и
//  получат EOF — их receiver_thread завершится.
// ──────────────────────────────────────────
static void broadcast_reauth()
{
    DIR *dir = opendir("/tmp");
    if (!dir) return;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Ищем файлы вида fifo_<логин>
        if (strncmp(entry->d_name, "fifo_", 5) != 0) continue;

        char path[300];
        snprintf(path, sizeof(path), "/tmp/%s", entry->d_name);

        // Проверяем что это именно FIFO
        struct stat st;
        if (stat(path, &st) < 0 || !S_ISFIFO(st.st_mode)) continue;

        // Открываем FIFO без блокировки
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            // Нет читателя — клиент уже отключён, просто удаляем
            unlink(path);
            continue;
        }

        // Формируем системное сообщение
        Message msg = {};
        msg.msg_type  = MSG_SYSTEM;
        msg.timestamp = time(nullptr);
        strncpy(msg.sender,        "server",  sizeof(msg.sender) - 1);
        strncpy(msg.dest.receiver, entry->d_name + 5, // имя без "fifo_"
                sizeof(msg.dest.receiver) - 1);
        strncpy(msg.text,
                "Сервер был перезапущен. Требуется переавторизация.",
                sizeof(msg.text) - 1);

        write(fd, &msg, sizeof(Message));
        close(fd);
        unlink(path); // удаляем FIFO — клиент получит EOF и отключится
        count++;
    }
    closedir(dir);

    if (count > 0) {
        printf("\033[33m[server] Отправлено уведомление о перезапуске %d клиентам\033[0m\n",
               count);
        log_event("Старт: уведомлено %d активных клиентов о перезапуске", count);
    }
}

static void clear_online_locks()
{
    mkdir("data/online", 0755);
    DIR *dir = opendir("data/online");
    if (!dir) return;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lock") != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "data/online/%s", entry->d_name);
        unlink(path);
        count++;
    }
    closedir(dir);
    if (count > 0) {
        printf("\033[90m[server] Очищено %d устаревших lock-файлов\033[0m\n", count);
        log_event("Старт: очищено %d устаревших lock-файлов", count);
    }
}

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

    // Уведомляем клиентов предыдущей сессии о перезапуске
    broadcast_reauth();

    // Чистим устаревшие lock-файлы от предыдущего запуска
    clear_online_locks();

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

    // SIGUSR1 — перечитывание конфигурации без перезапуска
    struct sigaction sa_usr1 = {};
    sa_usr1.sa_handler = sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = 0; // без SA_RESTART — чтобы accept прерывался и мы проверили флаг
    sigaction(SIGUSR1, &sa_usr1, nullptr);

    // Создаём слушающий сокет
    server_fd = create_server_socket(port);
    if (server_fd < 0) {
        log_event("КРИТИЧНО: не удалось создать сокет");
        log_close();
        return 1;
    }

    log_event("Сервер готов, ожидаем подключений...");
    printf("[server] Запущен на порту %d. Лог: data/server.log\n", port);
    printf("\033[90m[server] Reload конфига: kill -USR1 %d\033[0m\n", getpid());

    // ── Основной цикл accept() ──
    while (1) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // Прерван сигналом — проверяем нужен ли reload
                if (reload_requested) do_reload();
                continue;
            }
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

            // Сохраняем fd клиента глобально для sighup_child_handler
            child_client_fd = client_fd;

            // В дочернем устанавливаем обработчик SIGHUP
            signal(SIGHUP, sighup_child_handler);
            // Дочернему SIGUSR1 не нужен — сбрасываем на дефолт
            signal(SIGUSR1, SIG_DFL);

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