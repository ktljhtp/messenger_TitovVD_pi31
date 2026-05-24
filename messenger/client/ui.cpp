#include "session.h"
#include "auth.h"
#include "logger.h"
#include "../common/message.h"

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
//  Вспомогательная: отправить системный ответ клиенту
// ──────────────────────────────────────────
static void send_sys(int client_fd, const char *login, const char *text)
{
    Message sys = {};
    sys.msg_type  = MSG_SYSTEM;
    sys.timestamp = time(nullptr);
    strncpy(sys.sender,        "server", sizeof(sys.sender) - 1);
    strncpy(sys.dest.receiver, login,    sizeof(sys.dest.receiver) - 1);
    snprintf(sys.text, sizeof(sys.text), "%s", text);
    send(client_fd, &sys, sizeof(Message), 0);
}

// ──────────────────────────────────────────
//  save_to_history
// ──────────────────────────────────────────
static void save_to_history(const Message *msg)
{
    FILE *f = fopen("data/history.dat", "ab");
    if (!f) return;
    fwrite(msg, sizeof(Message), 1, f);
    fclose(f);
}

// ──────────────────────────────────────────
//  cmd_newuser
//
//  Формат команды в msg.text: NEWUSER:логин:пароль:имя
//  Добавляет строку в data/users.dat
//  Ответы: OK_NEWUSER / ERR_EXISTS / ERR_BADFORMAT
// ──────────────────────────────────────────
static void cmd_newuser(int client_fd, const char *login,
                        const char *payload)
{
    // Парсим: логин:пароль:имя
    char new_login[32] = {0};
    char password[64]  = {0};
    char disp_name[64] = {0};

    // payload = "логин:пароль:имя"
    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);

    char *p1 = strchr(buf, ':');
    if (!p1) { send_sys(client_fd, login, "ERR_BADFORMAT"); return; }
    *p1 = '\0';
    char *p2 = strchr(p1 + 1, ':');
    if (!p2) { send_sys(client_fd, login, "ERR_BADFORMAT"); return; }
    *p2 = '\0';

    strncpy(new_login,  buf,      sizeof(new_login)  - 1);
    strncpy(password,   p1 + 1,   sizeof(password)   - 1);
    strncpy(disp_name,  p2 + 1,   sizeof(disp_name)  - 1);

    // Проверяем что логин не занят
    FILE *f = fopen(USERS_DAT, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char existing[32] = {0};
            sscanf(line, "%31[^:]", existing);
            if (strcmp(existing, new_login) == 0) {
                fclose(f);
                send_sys(client_fd, login, "ERR_EXISTS");
                log_event("Попытка создать дубль пользователя '%s' от '%s'",
                          new_login, login);
                return;
            }
        }
        fclose(f);
    }

    // Хешируем пароль
    const char *hashed = hash_password(password);

    // Дозаписываем в users.dat
    f = fopen(USERS_DAT, "a");
    if (!f) { send_sys(client_fd, login, "ERR_SERVER"); return; }
    fprintf(f, "%s:%s:%s\n", new_login, hashed, disp_name);
    fclose(f);

    log_event("Пользователь '%s' создан по запросу '%s'", new_login, login);
    send_sys(client_fd, login, "OK_NEWUSER");
}

// ──────────────────────────────────────────
//  cmd_newgroup
//
//  Формат: NEWGROUP:название:участник1:участник2:...
//  Создатель добавляется автоматически.
//  Ответы: OK_NEWGROUP / ERR_EXISTS / ERR_BADFORMAT
// ──────────────────────────────────────────
static void cmd_newgroup(int client_fd, const char *login,
                         const char *payload)
{
    // payload = "название:участник1:участник2:..."
    char buf[512];
    strncpy(buf, payload, sizeof(buf) - 1);

    char *colon = strchr(buf, ':');
    // Имя группы может быть без участников (только создатель)
    char group_name[64] = {0};
    if (colon) {
        int namelen = (int)(colon - buf);
        if (namelen <= 0 || namelen >= (int)sizeof(group_name)) {
            send_sys(client_fd, login, "ERR_BADFORMAT"); return;
        }
        strncpy(group_name, buf, namelen);
    } else {
        strncpy(group_name, buf, sizeof(group_name) - 1);
    }

    // Проверяем что группа не существует
    FILE *f = fopen(GROUPS_DAT, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char existing[64] = {0};
            sscanf(line, "%63[^:]", existing);
            if (strcmp(existing, group_name) == 0) {
                fclose(f);
                send_sys(client_fd, login, "ERR_EXISTS");
                return;
            }
        }
        fclose(f);
    }

    // Формируем строку: группа:создатель:участник1:участник2:...
    f = fopen(GROUPS_DAT, "a");
    if (!f) { send_sys(client_fd, login, "ERR_SERVER"); return; }

    fprintf(f, "%s:%s", group_name, login); // создатель всегда первый

    // Добавляем остальных участников (если есть)
    if (colon) {
        char *member = strtok(colon + 1, ":");
        while (member) {
            // Не дублируем создателя
            if (strcmp(member, login) != 0) {
                fprintf(f, ":%s", member);
            }
            member = strtok(nullptr, ":");
        }
    }
    fprintf(f, "\n");
    fclose(f);

    log_event("Группа '%s' создана пользователем '%s'", group_name, login);
    send_sys(client_fd, login, "OK_NEWGROUP");
}

// ──────────────────────────────────────────
//  cmd_addmember
//
//  Формат: ADDMEMBER:группа:логин
//  Добавляет участника в существующую группу.
//  Ответы: OK_ADDMEMBER / ERR_NOGROUP / ERR_EXISTS
// ──────────────────────────────────────────
static void cmd_addmember(int client_fd, const char *login,
                          const char *payload)
{
    char buf[256];
    strncpy(buf, payload, sizeof(buf) - 1);

    char *colon = strchr(buf, ':');
    if (!colon) { send_sys(client_fd, login, "ERR_BADFORMAT"); return; }
    *colon = '\0';

    const char *group_name  = buf;
    const char *new_member  = colon + 1;

    // Читаем groups.dat, ищем группу и добавляем участника
    FILE *fin = fopen(GROUPS_DAT, "r");
    if (!fin) { send_sys(client_fd, login, "ERR_NOGROUP"); return; }

    // Собираем новый файл в памяти
    char lines[64][512];
    int  line_count = 0;
    int  found = 0;

    char line[512];
    while (fgets(line, sizeof(line), fin) && line_count < 64) {
        line[strcspn(line, "\n")] = '\0';
        char gname[64] = {0};
        sscanf(line, "%63[^:]", gname);

        if (strcmp(gname, group_name) == 0) {
            found = 1;
            // Проверяем что участник ещё не в группе
            if (strstr(line, new_member)) {
                fclose(fin);
                send_sys(client_fd, login, "ERR_EXISTS");
                return;
            }
            // Добавляем нового участника в конец строки
            snprintf(lines[line_count], sizeof(lines[line_count]),
                     "%s:%s", line, new_member);
        } else {
            strncpy(lines[line_count], line, sizeof(lines[line_count]) - 1);
        }
        line_count++;
    }
    fclose(fin);

    if (!found) { send_sys(client_fd, login, "ERR_NOGROUP"); return; }

    // Перезаписываем groups.dat
    FILE *fout = fopen(GROUPS_DAT, "w");
    if (!fout) { send_sys(client_fd, login, "ERR_SERVER"); return; }
    for (int i = 0; i < line_count; i++)
        fprintf(fout, "%s\n", lines[i]);
    fclose(fout);

    log_event("Участник '%s' добавлен в группу '%s' по запросу '%s'",
              new_member, group_name, login);
    send_sys(client_fd, login, "OK_ADDMEMBER");
}

// ──────────────────────────────────────────
//  cmd_listgroups / cmd_listusers
//
//  Возвращают список через системные сообщения:
//    GROUPS:группа1:группа2:...
//    USERS:логин1:логин2:...
// ──────────────────────────────────────────
static void cmd_listgroups(int client_fd, const char *login)
{
    FILE *f = fopen(GROUPS_DAT, "r");
    char result[1000] = "GROUPS:";
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        char gname[64] = {0};
        sscanf(line, "%63[^:]", gname);
        if (gname[0] == '#' || gname[0] == '\0') continue;
        if (strlen(result) + strlen(gname) + 2 < sizeof(result)) {
            if (strlen(result) > 7) strcat(result, ":");
            strcat(result, gname);
        }
    }
    if (f) fclose(f);
    send_sys(client_fd, login, result);
}

static void cmd_listusers(int client_fd, const char *login)
{
    FILE *f = fopen(USERS_DAT, "r");
    char result[1000] = "USERS:";
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        char uname[32] = {0};
        sscanf(line, "%31[^:]", uname);
        if (uname[0] == '#' || uname[0] == '\0') continue;
        if (strlen(result) + strlen(uname) + 2 < sizeof(result)) {
            if (strlen(result) > 6) strcat(result, ":");
            strcat(result, uname);
        }
    }
    if (f) fclose(f);
    send_sys(client_fd, login, result);
}

// ──────────────────────────────────────────
//  handle_system_command
//
//  Разбирает msg.text системного сообщения
//  и вызывает нужный обработчик.
// ──────────────────────────────────────────
static void handle_system_command(int client_fd, const char *login,
                                   const Message *msg)
{
    const char *text = msg->text;

    if (strncmp(text, "NEWUSER:", 8) == 0)
        cmd_newuser(client_fd, login, text + 8);
    else if (strncmp(text, "NEWGROUP:", 9) == 0)
        cmd_newgroup(client_fd, login, text + 9);
    else if (strncmp(text, "ADDMEMBER:", 10) == 0)
        cmd_addmember(client_fd, login, text + 10);
    else if (strcmp(text, "LISTGROUPS") == 0)
        cmd_listgroups(client_fd, login);
    else if (strcmp(text, "USERS") == 0 || strcmp(text, "LISTUSERS") == 0)
        cmd_listusers(client_fd, login);
    else
        log_event("Неизвестная системная команда от '%s': %s", login, text);
}

// ──────────────────────────────────────────
//  route_message
// ──────────────────────────────────────────
int route_message(const Message *msg)
{
    if (msg->msg_type == MSG_PERSONAL) {
        char fifo_path[FIFO_MAXPATH];
        snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, msg->dest.receiver);
        int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            log_event("Получатель %s не в сети", msg->dest.receiver);
            return -1;
        }
        ssize_t w = write(fd, msg, sizeof(Message));
        close(fd);
        return (w == (ssize_t)sizeof(Message)) ? 0 : -1;

    } else if (msg->msg_type == MSG_GROUP) {
        FILE *f = fopen(GROUPS_DAT, "r");
        if (!f) return -1;

        char line[512];
        int delivered = 0;

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            char *group = strtok(line, ":");
            if (!group || strcmp(group, msg->dest.group_name) != 0) continue;

            char *member;
            while ((member = strtok(nullptr, ":")) != nullptr) {
                if (strcmp(member, msg->sender) == 0) continue;
                char fifo_path[FIFO_MAXPATH];
                snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, member);
                int fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
                if (fd < 0) continue;
                write(fd, msg, sizeof(Message));
                close(fd);
                delivered++;
            }
            break;
        }
        fclose(f);
        log_event("Группа '%s' от '%s': %d доставлено",
                  msg->dest.group_name, msg->sender, delivered);
        return (delivered > 0) ? 0 : -1;
    }
    return -1;
}

// ──────────────────────────────────────────
//  handle_client
// ──────────────────────────────────────────
void handle_client(int client_fd, struct sockaddr_in addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    log_event("Новое подключение с %s", client_ip);

    char login[32] = {0};
    if (authenticate(client_fd, login) < 0) {
        log_event("Отказ в доступе для %s", client_ip);
        close(client_fd);
        return;
    }
    log_event("Пользователь '%s' вошёл (%s)", login, client_ip);

    char fifo_path[FIFO_MAXPATH];
    snprintf(fifo_path, sizeof(fifo_path), FIFO_PATH, login);
    unlink(fifo_path);

    if (mkfifo(fifo_path, 0666) < 0) {
        log_event("ОШИБКА mkfifo для %s: %s", login, strerror(errno));
        close(client_fd); return;
    }

    int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
    if (fifo_fd < 0) {
        log_event("ОШИБКА open FIFO для %s: %s", login, strerror(errno));
        unlink(fifo_path); close(client_fd); return;
    }

    Message msg;
    fd_set read_fds;
    int maxfd = (client_fd > fifo_fd ? client_fd : fifo_fd) + 1;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(fifo_fd,   &read_fds);

        int ready = select(maxfd, &read_fds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(client_fd, &read_fds)) {
            ssize_t n = recv(client_fd, &msg, sizeof(Message), MSG_WAITALL);
            if (n <= 0) {
                log_event("Пользователь '%s' отключился", login);
                break;
            }

            log_event("Сообщение от '%s' тип=%d", login, msg.msg_type);

            if (msg.msg_type == MSG_SYSTEM) {
                // Системная команда — обрабатываем на сервере
                handle_system_command(client_fd, login, &msg);
            } else {
                save_to_history(&msg);
                if (route_message(&msg) < 0) {
                    char errtext[256];
                    if (msg.msg_type == MSG_PERSONAL)
                        snprintf(errtext, sizeof(errtext),
                                 "Пользователь '%s' не в сети",
                                 msg.dest.receiver);
                    else
                        snprintf(errtext, sizeof(errtext),
                                 "Группа '%s' недоступна или нет участников онлайн",
                                 msg.dest.group_name);
                    send_sys(client_fd, login, errtext);
                }
            }
        }

        if (FD_ISSET(fifo_fd, &read_fds)) {
            Message incoming;
            ssize_t n = read(fifo_fd, &incoming, sizeof(Message));
            if (n == (ssize_t)sizeof(Message))
                send(client_fd, &incoming, sizeof(Message), 0);
        }
    }

    close(fifo_fd);
    unlink(fifo_path);
    close(client_fd);
    log_event("Сессия '%s' завершена", login);
}