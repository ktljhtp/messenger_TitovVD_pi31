#pragma once

#include <time.h>
#include <stdint.h>

// ──────────────────────────────────────────
//  Константы протокола
// ──────────────────────────────────────────
#define DEFAULT_PORT     8080
#define MAX_CLIENTS      10
#define MSG_PERSONAL     0   // личное сообщение  → dest.receiver
#define MSG_SYSTEM       1   // системное уведомление
#define MSG_GROUP        2   // групповое         → dest.group_name

// ──────────────────────────────────────────
//  Основная структура пакета (v2)
//
//  Передаётся целиком как бинарный блок
//  фиксированного размера через send()/recv().
//
//  ВАЖНО: dest — union, receiver и group_name
//  занимают одни и те же 32 байта памяти.
//  Заполняй только одно поле в зависимости
//  от msg_type:
//    msg_type == MSG_PERSONAL → dest.receiver
//    msg_type == MSG_GROUP    → dest.group_name
//    msg_type == MSG_SYSTEM   → dest.receiver (логин получателя)
// ──────────────────────────────────────────
typedef struct {
    char sender[32];       // логин отправителя
    union {
        char receiver[32];    // логин получателя   (msg_type 0 или 1)
        char group_name[32];  // имя группы          (msg_type 2)
    } dest;                // ровно 32 байта — не 64!
    char    text[1024];    // тело сообщения (зашифровано)
    time_t  timestamp;     // время отправки (Unix timestamp)
    int     msg_type;      // дискриминатор: 0 / 1 / 2
} Message;
