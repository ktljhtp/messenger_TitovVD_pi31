#pragma once

#include <time.h>
#include <stdint.h>

#define DEFAULT_PORT  8080
#define MAX_CLIENTS   10
#define MSG_PERSONAL  0
#define MSG_SYSTEM    1
#define MSG_GROUP     2

// Длина шифротекста хранится в первых 4 байтах поля text.
// Остальные байты — сам шифротекст.
// Это позволяет не менять размер структуры.
#define MSG_CLEN_SIZE 4                      // sizeof(int) для длины
#define MSG_TEXT_MAX  (1024 - MSG_CLEN_SIZE) // макс. байт шифротекста

typedef struct {
    char   sender[32];
    union {
        char receiver[32];
        char group_name[32];
    } dest;
    char   text[1024]; // [0..3]=длина шифротекста, [4..]=шифротекст
    time_t timestamp;
    int    msg_type;
} Message;

// Вспомогательные inline-функции для работы с text
#include <string.h>

// Записать длину в начало text[]
static inline void msg_set_clen(Message *m, int clen) {
    memcpy(m->text, &clen, MSG_CLEN_SIZE);
}

// Прочитать длину из text[]
static inline int msg_get_clen(const Message *m) {
    int clen = 0;
    memcpy(&clen, m->text, MSG_CLEN_SIZE);
    return clen;
}

// Указатель на начало шифротекста внутри text[]
static inline uint8_t *msg_cipher_ptr(Message *m) {
    return (uint8_t *)(m->text + MSG_CLEN_SIZE);
}
static inline const uint8_t *msg_cipher_cptr(const Message *m) {
    return (const uint8_t *)(m->text + MSG_CLEN_SIZE);
}