#include "ui.h"
#include "../common/message.h"
#include "../common/crypto.h"

#include <locale.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>

// ── ANSI цвета ──────────────────────────
#define R      "\033[0m"
#define BOLD   "\033[1m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BLUE   "\033[34m"
#define GRAY   "\033[90m"
#define RED    "\033[31m"
#define CLEAR  "\r\033[2K"

// ── Константы ───────────────────────────
#define MAX_CHATS    64
#define MAX_MSGS     512
#define MSG_LINE_LEN 512

// ── Структуры ───────────────────────────
typedef struct {
    char time[10];
    char sender[32];
    char text[MSG_LINE_LEN];
} StoredMsg;

typedef struct {
    char      name[64];
    int       is_group;
    int       unread;
    StoredMsg msgs[MAX_MSGS];
    int       msg_count;
} Chat;

// ── Глобальное состояние ─────────────────
static Chat            chats[MAX_CHATS];
static int             chat_count  = 0;
static int             active_chat = -1;
static pthread_mutex_t ui_mutex    = PTHREAD_MUTEX_INITIALIZER;
static struct termios  orig_termios;
static int             raw_mode    = 0;
static char            my_login[32]      = {0};
static char            history_dir[128]  = {0};
static int             g_server_fd       = -1;

// ── Ключ/IV (совпадают с receiver.cpp) ──
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

// ════════════════════════════════════════
//  RAW-РЕЖИМ ТЕРМИНАЛА
// ════════════════════════════════════════
static void enable_raw()
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode = 1;
}
static void disable_raw()
{
    if (raw_mode) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); raw_mode = 0; }
}

// forward declaration
static int find_or_create_chat(const char *name, int is_group);

// ════════════════════════════════════════
//  ИСТОРИЯ НА ДИСКЕ
//
//  Папка:   history/<мой_логин>/
//  Файл:    history/<мой_логин>/<имя_чата>.log
//  Строка:  UNIX_TIMESTAMP|ОТПРАВИТЕЛЬ|ТЕКСТ
// ════════════════════════════════════════
static void history_init()
{
    mkdir("history", 0755);
    mkdir(history_dir, 0755);

    // Сканируем папку history/<логин>/ и загружаем все чаты
    DIR *dir = opendir(history_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Пропускаем . и ..
        if (entry->d_name[0] == '.') continue;

        // Берём только файлы .log
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".log") != 0) continue;

        // Имя чата = имя файла без .log
        char chat_name[64] = {0};
        int namelen = (int)(dot - entry->d_name);
        if (namelen <= 0 || namelen >= (int)sizeof(chat_name)) continue;
        strncpy(chat_name, entry->d_name, namelen);

        // Определяем тип чата: если имя начинается с "group_" — группа
        // Иначе — личный чат
        // (группы сохраняются с префиксом "group_" — см. history_append)
        int is_group = (strncmp(chat_name, "group_", 6) == 0);
        const char *display_name = is_group ? chat_name + 6 : chat_name;

        // Создаём чат и загружаем историю
        int idx = find_or_create_chat(display_name, is_group);
        (void)idx;
    }
    closedir(dir);
}

static void history_append(const char *chat_name, int is_group,
                            const char *sender, const char *text, time_t ts)
{
    char path[300];
    if (is_group)
        snprintf(path, sizeof(path), "%s/group_%s.log", history_dir, chat_name);
    else
        snprintf(path, sizeof(path), "%s/%s.log", history_dir, chat_name);
    FILE *f = fopen(path, "a");
    if (!f) return;

    // Убираем | и \n из текста чтобы не ломать формат
    char safe[MSG_LINE_LEN];
    int j = 0;
    for (int i = 0; text[i] && j < (int)sizeof(safe) - 2; i++)
        safe[j++] = (text[i] == '|' || text[i] == '\n') ? ' ' : text[i];
    safe[j] = '\0';

    fprintf(f, "%ld|%s|%s\n", (long)ts, sender, safe);
    fclose(f);
}

// forward declaration — нужна для history_load
static void chat_add_msg_nohistory(int idx, const char *sender,
                                   const char *text, time_t ts);

static void history_load(int idx)
{
    char path[300];
    if (chats[idx].is_group)
        snprintf(path, sizeof(path), "%s/group_%s.log", history_dir, chats[idx].name);
    else
        snprintf(path, sizeof(path), "%s/%s.log", history_dir, chats[idx].name);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MSG_LINE_LEN + 80];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *p1 = strchr(line, '|');  if (!p1) continue;
        *p1 = '\0';
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        *p2 = '\0';
        time_t ts      = (time_t)atol(line);
        const char *sn = p1 + 1;
        const char *tx = p2 + 1;
        chat_add_msg_nohistory(idx, sn, tx, ts);
    }
    fclose(f);
}

// ════════════════════════════════════════
//  УПРАВЛЕНИЕ ЧАТАМИ В ПАМЯТИ
// ════════════════════════════════════════

// Добавить сообщение БЕЗ записи на диск (используется при загрузке истории)
static void chat_add_msg_nohistory(int idx, const char *sender,
                                   const char *text, time_t ts)
{
    Chat *c = &chats[idx];
    if (c->msg_count >= MAX_MSGS) {
        memmove(&c->msgs[0], &c->msgs[1], sizeof(StoredMsg) * (MAX_MSGS - 1));
        c->msg_count = MAX_MSGS - 1;
    }
    StoredMsg *m = &c->msgs[c->msg_count++];
    struct tm *t = localtime(&ts);
    strftime(m->time, sizeof(m->time), "%H:%M", t);
    strncpy(m->sender, sender, sizeof(m->sender) - 1);
    strncpy(m->text,   text,   sizeof(m->text)   - 1);
}

// Добавить сообщение С записью на диск (используется при отправке/получении)
static void chat_add_msg(int idx, const char *sender,
                         const char *text, time_t ts)
{
    chat_add_msg_nohistory(idx, sender, text, ts);
    history_append(chats[idx].name, chats[idx].is_group, sender, text, ts);
}

// Найти чат по имени или создать новый (с загрузкой истории)
static int find_or_create_chat(const char *name, int is_group)
{
    for (int i = 0; i < chat_count; i++)
        if (strcmp(chats[i].name, name) == 0) return i;

    if (chat_count >= MAX_CHATS) return -1;

    int idx = chat_count++;
    memset(&chats[idx], 0, sizeof(Chat));
    strncpy(chats[idx].name, name, sizeof(chats[idx].name) - 1);
    chats[idx].is_group = is_group;
    chats[idx].unread   = 0;

    history_load(idx); // ← загружаем историю при первом обращении к чату
    return idx;
}

// ════════════════════════════════════════
//  ВВОД СТРОКИ (raw-режим, UTF-8)
// ════════════════════════════════════════
static int read_line(char *buf, int maxlen, const char *prompt)
{
    enable_raw();
    memset(buf, 0, maxlen);
    int len = 0;

    printf(CLEAR "%s%s" R, CYAN, prompt);
    fflush(stdout);

    while (1) {
        unsigned char ch;
        if (read(STDIN_FILENO, &ch, 1) <= 0) { disable_raw(); return -1; }

        if (ch == 3 || ch == 4)               { disable_raw(); printf("\n"); return -1; }
        if (ch == '\r' || ch == '\n')          { buf[len] = '\0'; disable_raw(); printf("\n"); return len; }

        // ESC-последовательности (стрелки) — съедаем
        if (ch == 27) {
            struct termios tmp = orig_termios;
            tmp.c_lflag &= ~(ICANON | ECHO);
            tmp.c_cc[VMIN] = 0; tmp.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &tmp);
            unsigned char esc[8]; read(STDIN_FILENO, esc, sizeof(esc));
            enable_raw();
            continue;
        }

        // Backspace
        if (ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                while (len > 0 && (buf[len] & 0xC0) == 0x80) len--;
                buf[len] = '\0';
                printf(CLEAR "%s%s%s" R, CYAN, prompt, buf);
                fflush(stdout);
            }
            continue;
        }

        // ASCII
        if (ch >= 0x20 && ch <= 0x7E && len < maxlen - 1) {
            buf[len++] = (char)ch; buf[len] = '\0';
            printf(CLEAR "%s%s%s" R, CYAN, prompt, buf);
            fflush(stdout);
            continue;
        }

        // UTF-8 (кириллица и др.)
        if (ch >= 0xC0 && ch <= 0xF7 && len < maxlen - 4) {
            int extra = (ch >= 0xF0) ? 3 : (ch >= 0xE0) ? 2 : 1;
            buf[len++] = (char)ch;
            unsigned char cont[3] = {0};
            int got = (int)read(STDIN_FILENO, cont, extra);
            for (int i = 0; i < got; i++) buf[len++] = (char)cont[i];
            buf[len] = '\0';
            printf(CLEAR "%s%s%s" R, CYAN, prompt, buf);
            fflush(stdout);
        }
    }
}

// ════════════════════════════════════════
//  ОТРИСОВКА
// ════════════════════════════════════════
static void draw_chat_list()
{
    printf("\033[2J\033[H");
    printf(BOLD CYAN
        "╔══════════════════════════════════════╗\n"
        "║  МЕССЕНДЖЕР  —  %s%-20s" CYAN "║\n"
        "╚══════════════════════════════════════╝\n" R,
        GREEN, my_login);

    if (chat_count == 0) {
        printf(GRAY "\n  Нет чатов. Отправьте первое сообщение:\n"
                    "  /msg <логин> <текст>\n\n" R);
    } else {
        printf(GRAY "  Выберите чат (номер) или введите команду\n\n" R);
        for (int i = 0; i < chat_count; i++) {
            const char *icon = chats[i].is_group ? "👥" : "💬";
            // Последнее сообщение в превью
            const char *preview = "";
            if (chats[i].msg_count > 0)
                preview = chats[i].msgs[chats[i].msg_count - 1].text;

            if (chats[i].unread > 0) {
                printf(BOLD "  %d. %s %-20s" YELLOW " [%d новых]" GRAY " %.20s\n" R,
                       i+1, icon, chats[i].name, chats[i].unread, preview);
            } else {
                printf("  %d. %s %-20s " GRAY "%.25s\n" R,
                       i+1, icon, chats[i].name, preview);
            }
        }
    }

    printf(GRAY
        "\n──────────────────────────────────────────\n"
        "  <номер>                — открыть чат\n"
        "  /msg <логин> <текст>   — новое сообщение\n"
        "  /group <группа> <текст>— в группу\n"
        "  /quit                  — выход\n"
        "──────────────────────────────────────────\n" R);
}

static void draw_chat(int idx)
{
    Chat *c = &chats[idx];
    printf("\033[2J\033[H");

    const char *icon = c->is_group ? "[G]" : "[P]";
    printf(BOLD CYAN "\n=== %s %s ===\n\n" R, icon, c->name);

    if (c->msg_count == 0) {
        printf(GRAY "\n  Нет сообщений\n\n" R);
    } else {
        printf("\n");
        int start = c->msg_count > 20 ? c->msg_count - 20 : 0;
        for (int i = start; i < c->msg_count; i++) {
            StoredMsg *m = &c->msgs[i];
            if (strcmp(m->sender, my_login) == 0)
                printf(GRAY "  [%s] " R BOLD GREEN "Вы: " R "%s\n", m->time, m->text);
            else
                printf(GRAY "  [%s] " R BOLD BLUE "%s: " R "%s\n", m->time, m->sender, m->text);
        }
        printf("\n");
    }

    int total = c->msg_count;
    printf(GRAY
        "  Сообщений в истории: %d\n"
        "──────────────────────────────────────────\n"
        "  Введите текст → Enter для отправки\n"
        "  /back — список чатов  |  /quit — выход\n"
        "──────────────────────────────────────────\n" R, total);
    c->unread = 0;
}

// ════════════════════════════════════════
//  ОТПРАВКА СООБЩЕНИЯ
// ════════════════════════════════════════
static int send_message(int server_fd, int msg_type,
                        const char *dest, const char *text)
{
    Message msg = {};
    msg.msg_type  = msg_type;
    msg.timestamp = time(nullptr);
    strncpy(msg.sender,        my_login, sizeof(msg.sender) - 1);
    strncpy(msg.dest.receiver, dest,     sizeof(msg.dest.receiver) - 1);

    uint8_t cipher[MSG_TEXT_MAX + 16] = {0};
    int clen = encrypt_message((const uint8_t *)text, (int)strlen(text),
                               SESSION_KEY, SESSION_IV, cipher);
    if (clen <= 0 || clen > MSG_TEXT_MAX) {
        printf(RED "  Ошибка шифрования\n" R);
        return -1;
    }
    msg_set_clen(&msg, clen);
    memcpy(msg_cipher_ptr(&msg), cipher, clen);

    if (send(server_fd, &msg, sizeof(Message), 0) != (ssize_t)sizeof(Message)) {
        printf(RED "  Ошибка отправки\n" R);
        return -1;
    }
    return 0;
}

// ════════════════════════════════════════
//  ПУБЛИЧНЫЕ ФУНКЦИИ
// ════════════════════════════════════════
void ui_init(const char *login)
{
    setlocale(LC_ALL, "");
    strncpy(my_login, login, sizeof(my_login) - 1);
    snprintf(history_dir, sizeof(history_dir), "history/%s", login);
    history_init();
}

void ui_receive_message(const Message *msg)
{
    pthread_mutex_lock(&ui_mutex);

    const char *chat_name;
    int is_group;
    if (msg->msg_type == MSG_GROUP) {
        chat_name = msg->dest.group_name;
        is_group  = 1;
    } else {
        chat_name = (strcmp(msg->dest.receiver, my_login) == 0)
                    ? msg->sender : msg->dest.receiver;
        is_group  = 0;
    }

    int idx = find_or_create_chat(chat_name, is_group);
    if (idx < 0) { pthread_mutex_unlock(&ui_mutex); return; }

    chat_add_msg(idx, msg->sender, msg->text, msg->timestamp);

    if (active_chat == idx) {
        struct tm *t = localtime(&msg->timestamp);
        char tb[10]; strftime(tb, sizeof(tb), "%H:%M", t);
        if (strcmp(msg->sender, my_login) == 0)
            printf(GRAY "\n  [%s] " R BOLD GREEN "Вы: " R "%s\n", tb, msg->text);
        else
            printf(GRAY "\n  [%s] " R BOLD BLUE "%s: " R "%s\n", tb, msg->sender, msg->text);
        printf(CLEAR CYAN "> " R);
        fflush(stdout);
    } else {
        chats[idx].unread++;
        printf(CLEAR YELLOW " [+] Новое от %s в чате \"%s\"" R "\n",
               msg->sender, chat_name);
        fflush(stdout);
    }

    pthread_mutex_unlock(&ui_mutex);
}

// ════════════════════════════════════════
//  АНИМАЦИЯ ЗАПУСКА
// ════════════════════════════════════════
static void show_startup_animation()
{
    printf("\033[2J\033[H");
    // ASCII-логотип
    printf(BOLD CYAN "\n\n"
    "  ██████╗██╗  ██╗ █████╗ ████████╗██╗██╗  ██╗\n"
    "  ██╔════╝██║  ██║██╔══██╗╚══██╔══╝██║██║ ██╔╝\n"
    "  ██║     ███████║███████║   ██║   ██║█████╔╝ \n"
    "  ██║     ██╔══██║██╔══██║   ██║   ██║██╔═██╗ \n"
    "  ╚██████╗██║  ██║██║  ██║   ██║   ██║██║  ██╗\n"
    "   ╚═════╝╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝   ╚═╝╚═╝  ╚═╝\n" R
    );

    fflush(stdout);
    usleep(500000);

    const char spin[] = "|/-\\";
    for (int i = 0; i < 12; i++) {
        printf("\r  Загрузка... %c", spin[i % 4]);
        fflush(stdout);
        usleep(80000);
    }
    printf("\r\033[K");  // очистить строку
    usleep(200000);
}

// ════════════════════════════════════════
//  ЦИКЛЫ НАВИГАЦИИ
// ════════════════════════════════════════
static int run_open_chat(int server_fd, int idx);

static void send_sys_cmd(int server_fd, const char *cmd_text)
{
    Message msg = {};
    msg.msg_type  = MSG_SYSTEM;
    msg.timestamp = time(nullptr);
    strncpy(msg.sender,        my_login, sizeof(msg.sender) - 1);
    strncpy(msg.dest.receiver, "server", sizeof(msg.dest.receiver) - 1);
    snprintf(msg.text, sizeof(msg.text), "%s", cmd_text);
    send(server_fd, &msg, sizeof(Message), 0);
}

static int run_chat_list(int server_fd)
{
    pthread_mutex_lock(&ui_mutex);
    active_chat = -1;
    draw_chat_list();
    pthread_mutex_unlock(&ui_mutex);

    char buf[1100];
    while (1) {
        int len = read_line(buf, sizeof(buf), "> ");
        if (len < 0) return -1;

        if (strlen(buf) == 0) {
            pthread_mutex_lock(&ui_mutex);
            draw_chat_list();
            pthread_mutex_unlock(&ui_mutex);
            continue;
        }

        if (strcmp(buf, "/quit") == 0) return -1;

        // Число → открыть чат
        char *ep;
        long num = strtol(buf, &ep, 10);
        if (*ep == '\0' && num >= 1 && num <= (long)chat_count) {
            int res = run_open_chat(server_fd, (int)num - 1);
            if (res == -1) return -1;
            pthread_mutex_lock(&ui_mutex);
            active_chat = -1;
            draw_chat_list();
            pthread_mutex_unlock(&ui_mutex);
            continue;
        }

        // /msg
        if (strncmp(buf, "/msg ", 5) == 0) {
            char *rest = buf + 5, *sp = strchr(rest, ' ');
            if (!sp) { printf(YELLOW "  /msg <логин> <текст>\n" R); continue; }
            *sp = '\0';
            pthread_mutex_lock(&ui_mutex);
            int idx = find_or_create_chat(rest, 0);
            pthread_mutex_unlock(&ui_mutex);
            if (idx >= 0 && send_message(server_fd, MSG_PERSONAL, rest, sp+1) == 0) {
                pthread_mutex_lock(&ui_mutex);
                chat_add_msg(idx, my_login, sp+1, time(nullptr));
                pthread_mutex_unlock(&ui_mutex);
                int res = run_open_chat(server_fd, idx);
                if (res == -1) return -1;
                pthread_mutex_lock(&ui_mutex);
                active_chat = -1;
                draw_chat_list();
                pthread_mutex_unlock(&ui_mutex);
            }
            continue;
        }

        // /group
        if (strncmp(buf, "/group ", 7) == 0) {
            char *rest = buf + 7, *sp = strchr(rest, ' ');
            if (!sp) { printf(YELLOW "  /group <группа> <текст>\n" R); continue; }
            *sp = '\0';
            pthread_mutex_lock(&ui_mutex);
            int idx = find_or_create_chat(rest, 1);
            pthread_mutex_unlock(&ui_mutex);
            if (idx >= 0 && send_message(server_fd, MSG_GROUP, rest, sp+1) == 0) {
                pthread_mutex_lock(&ui_mutex);
                chat_add_msg(idx, my_login, sp+1, time(nullptr));
                pthread_mutex_unlock(&ui_mutex);
                int res = run_open_chat(server_fd, idx);
                if (res == -1) return -1;
                pthread_mutex_lock(&ui_mutex);
                active_chat = -1;
                draw_chat_list();
                pthread_mutex_unlock(&ui_mutex);
            }
            continue;
        }

        // /newuser <логин> <пароль>
        if (strncmp(buf, "/newuser ", 9) == 0) {
            char *rest = buf + 9;
            char *sp   = strchr(rest, ' ');
            if (!sp) { printf(YELLOW "  /newuser <логин> <пароль>\n" R); continue; }
            *sp = '\0';
            // Имя по умолчанию = логин
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "NEWUSER:%s:%s:%s", rest, sp+1, rest);
            send_sys_cmd(server_fd, cmd);
            continue;
        }

        // /newgroup <группа> [участник1] [участник2] ...
        if (strncmp(buf, "/newgroup ", 10) == 0) {
            char *rest = buf + 10;
            // Разбираем: первое слово = имя группы, остальные = участники
            char *sp = strchr(rest, ' ');
            char cmd[512];
            if (sp) {
                *sp = '\0';
                // Заменяем пробелы между участниками на :
                char members[256] = {0};
                char *tok = strtok(sp + 1, " ");
                while (tok) {
                    if (strlen(members) > 0) strcat(members, ":");
                    strcat(members, tok);
                    tok = strtok(nullptr, " ");
                }
                snprintf(cmd, sizeof(cmd), "NEWGROUP:%s:%s", rest, members);
            } else {
                snprintf(cmd, sizeof(cmd), "NEWGROUP:%s", rest);
            }
            send_sys_cmd(server_fd, cmd);
            continue;
        }

        // /addmember <группа> <логин>
        if (strncmp(buf, "/addmember ", 11) == 0) {
            char *rest = buf + 11;
            char *sp   = strchr(rest, ' ');
            if (!sp) { printf(YELLOW "  /addmember <группа> <логин>\n" R); continue; }
            *sp = '\0';
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "ADDMEMBER:%s:%s", rest, sp+1);
            send_sys_cmd(server_fd, cmd);
            continue;
        }

        // /users
        if (strcmp(buf, "/users") == 0) {
            send_sys_cmd(server_fd, "LISTUSERS");
            continue;
        }

        // /groups
        if (strcmp(buf, "/groups") == 0) {
            send_sys_cmd(server_fd, "LISTGROUPS");
            continue;
        }

        printf(YELLOW "  Введите номер чата или /msg /group /newuser /newgroup /quit\n" R);
    }
}

static int run_open_chat(int server_fd, int idx)
{
    pthread_mutex_lock(&ui_mutex);
    active_chat = idx;
    chats[idx].unread = 0;

    // Для группы запрашиваем историю с сервера при первом открытии
    // (история личных чатов хранится локально, группы — на сервере)
    if (chats[idx].is_group && chats[idx].msg_count == 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "GETHISTORY:%s", chats[idx].name);
        pthread_mutex_unlock(&ui_mutex);
        send_sys_cmd(server_fd, cmd);
        // Ждём чуть-чуть пока придут сообщения истории
        struct timespec ts = {0, 300000000}; // 300мс
        nanosleep(&ts, nullptr);
        pthread_mutex_lock(&ui_mutex);
        active_chat = idx;
    }

    draw_chat(idx);
    pthread_mutex_unlock(&ui_mutex);

    char buf[1100];
    while (1) {
        int len = read_line(buf, sizeof(buf), "> ");
        if (len < 0) return -1;
        if (strcmp(buf, "/quit") == 0) return -1;
        if (strcmp(buf, "/back") == 0 || len == 0) return 0;

        int msg_type = chats[idx].is_group ? MSG_GROUP : MSG_PERSONAL;
        if (send_message(server_fd, msg_type, chats[idx].name, buf) == 0) {
            pthread_mutex_lock(&ui_mutex);
            time_t now = time(nullptr);
            chat_add_msg(idx, my_login, buf, now);
            struct tm *t = localtime(&now);
            char tb[10]; strftime(tb, sizeof(tb), "%H:%M", t);
            printf(GRAY "  [%s] " R BOLD GREEN "Вы: " R "%s\n", tb, buf);
            pthread_mutex_unlock(&ui_mutex);
        }
    }
}

void ui_run(int server_fd)
{
    g_server_fd = server_fd;
    show_startup_animation();  // <-- добавленная анимация при запуске
    while (run_chat_list(server_fd) != -1);
}

void ui_cleanup()
{
    disable_raw();
    printf(R "\n");
    fflush(stdout);
}