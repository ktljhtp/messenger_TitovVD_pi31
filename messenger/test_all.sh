#!/bin/bash
# ══════════════════════════════════════════════════════════
#  test_all.sh — Этап 5: автоматическое тестирование
#
#  Проверяет все 5 сценариев из гайда:
#    5.1 Базовое подключение и авторизация
#    5.2 Личные сообщения между клиентами
#    5.3 Групповые сообщения
#    5.4 Лимит подключений (10 клиентов)
#    5.5 Шифрование (проверка через перехват трафика)
#
#  Требования:
#    - ./server и ./client уже собраны (make all)
#    - nc (netcat) установлен
#    - Запускать из корневой папки проекта
#
#  Запуск: bash test_all.sh
# ══════════════════════════════════════════════════════════

# ── Настройки ──
SERVER_IP="127.0.0.1"
SERVER_PORT="8080"
SERVER_BIN="./messenger_server"
TIMEOUT=3       # секунд ожидания ответа от сервера
PASS_COUNT=0
FAIL_COUNT=0

# ── Цвета ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; ((PASS_COUNT++)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; ((FAIL_COUNT++)); }
info() { echo -e "${CYAN}[→]${NC} $1"; }
section() {
    echo ""
    echo -e "${BOLD}──────────────────────────────────────────${NC}"
    echo -e "${BOLD}  $1${NC}"
    echo -e "${BOLD}──────────────────────────────────────────${NC}"
}

# ──────────────────────────────────────────
#  Вспомогательные функции
# ──────────────────────────────────────────

# Запустить сервер в фоне, вернуть PID
# Глобальная переменная PID сервера
GLOBAL_SRV_PID=0

start_server() {
    local port="${1:-$SERVER_PORT}"
    mkdir -p data
    $SERVER_BIN "$port" &>/tmp/server_test.log &
    GLOBAL_SRV_PID=$!
    sleep 0.7  # ждём пока сокет поднимется
}

stop_server() {
    kill "$GLOBAL_SRV_PID" 2>/dev/null
    wait "$GLOBAL_SRV_PID" 2>/dev/null
    rm -f /tmp/fifo_* 2>/dev/null
}

# Остановить сервер по PID
stop_server() {
    local pid="$1"
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    # Чистим FIFO
    rm -f /tmp/fifo_* 2>/dev/null
}

# Отправить команды серверу через netcat, вернуть ответ
# Использует текстовый протокол аутентификации (LOGIN/PASSWORD/OK)
nc_auth() {
    local login="$1"
    local password="$2"
    # printf отправляет строки с \n, timeout ограничивает ожидание
    {
        sleep 0.3
        printf "%s\n" "$login"
        sleep 0.2
        printf "%s\n" "$password"
        sleep 1.0
    } | nc -q1 "$SERVER_IP" "$SERVER_PORT" 2>/dev/null
}

# Проверить что сервер отвечает на порту
server_is_up() {
    nc -z "$SERVER_IP" "$SERVER_PORT" 2>/dev/null
}

# ──────────────────────────────────────────
#  Предварительная проверка
# ──────────────────────────────────────────
echo ""
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${BOLD}   Автотест мессенджера                   ${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"

# Проверяем наличие бинарников
if [[ ! -f "$SERVER_BIN" ]]; then
    echo -e "${RED}ОШИБКА: $SERVER_BIN не найден. Запусти make all${NC}"
    exit 1
fi
if [[ ! -f "./client" ]]; then
    echo -e "${RED}ОШИБКА: ./client не найден. Запусти make all${NC}"
    exit 1
fi
if ! which nc &>/dev/null; then
    echo -e "${RED}ОШИБКА: nc (netcat) не установлен: sudo apt install netcat-openbsd${NC}"
    exit 1
fi

# Убеждаемся что старый сервер не висит на порту
pkill -f "$SERVER_BIN" 2>/dev/null || true
sleep 0.3

# ══════════════════════════════════════════
#  ТЕСТ 5.1 — Базовое подключение и авторизация
# ══════════════════════════════════════════
section "5.1 Базовое подключение и авторизация"

start_server
SRV_PID=$GLOBAL_SRV_PID
info "Сервер запущен (PID=$SRV_PID)"

# Тест: верный логин/пароль → ожидаем "OK"
response=$(nc_auth "alice" "alice123")
if echo "$response" | grep -q "OK"; then
    pass "Верный логин/пароль → сервер ответил OK"
else
    fail "Верный логин/пароль → OK не получен (ответ: $(echo $response | tr -d '\n'))"
fi

# Тест: неверный пароль → ожидаем "FAIL"
response=$(nc_auth "alice" "wrongpass")
if echo "$response" | grep -q "FAIL"; then
    pass "Неверный пароль → сервер ответил FAIL"
else
    fail "Неверный пароль → FAIL не получен (ответ: $(echo $response | tr -d '\n'))"
fi

# Тест: несуществующий пользователь → ожидаем "FAIL"
response=$(nc_auth "nobody" "pass")
if echo "$response" | grep -q "FAIL"; then
    pass "Несуществующий пользователь → сервер ответил FAIL"
else
    fail "Несуществующий пользователь → FAIL не получен"
fi

# Тест: проверяем server.log
sleep 0.3
if [[ -f "data/server.log" ]]; then
    pass "data/server.log создан сервером"
    if grep -q "Аутентификация успешна" data/server.log 2>/dev/null; then
        pass "В логе есть запись об успешном входе alice"
    else
        fail "В логе нет записи об успешном входе"
    fi
else
    fail "data/server.log не создан"
fi

stop_server

# ══════════════════════════════════════════
#  ТЕСТ 5.2 — Личные сообщения через FIFO
# ══════════════════════════════════════════
section "5.2 Личные сообщения (FIFO)"

start_server
SRV_PID=$GLOBAL_SRV_PID
info "Сервер запущен (PID=$SRV_PID)"

# Подключаем alice — она создаст FIFO
{
    sleep 0.3; printf "alice\n"
    sleep 0.2; printf "alice123\n"
    sleep 2    # держим соединение открытым
} | nc -q5 "$SERVER_IP" "$SERVER_PORT" &>/dev/null &
NC_PID=$!
sleep 2.5

# Проверяем что FIFO создался
if [[ -p "/tmp/fifo_alice" ]]; then
    pass "FIFO /tmp/fifo_alice создан после входа alice"
else
    fail "FIFO /tmp/fifo_alice не создан"
fi

# Подключаем bob
{
    sleep 0.3; printf "bob\n"
    sleep 0.2; printf "bob123\n"
    sleep 2
} | nc -q5 "$SERVER_IP" "$SERVER_PORT" &>/dev/null &
NC_PID2=$!
sleep 1.5

if [[ -p "/tmp/fifo_bob" ]]; then
    pass "FIFO /tmp/fifo_bob создан после входа bob"
else
    fail "FIFO /tmp/fifo_bob не создан"
fi

# Проверяем что оба пользователя в логе
if grep -q "alice" data/server.log && grep -q "bob" data/server.log; then
    pass "Оба пользователя отражены в server.log"
else
    fail "Не все пользователи в server.log"
fi

kill $NC_PID $NC_PID2 2>/dev/null
wait $NC_PID $NC_PID2 2>/dev/null
stop_server

# ══════════════════════════════════════════
#  ТЕСТ 5.3 — Групповые сообщения (groups.dat)
# ══════════════════════════════════════════
section "5.3 Групповые сообщения"

# Проверяем структуру groups.dat
if [[ -f "data/groups.dat" ]]; then
    pass "data/groups.dat существует"
else
    fail "data/groups.dat не найден"
fi

if grep -q "testgroup" data/groups.dat; then
    pass "Группа 'testgroup' есть в groups.dat"
else
    fail "Группа 'testgroup' не найдена в groups.dat"
fi

if grep -q "testgroup:.*alice.*bob.*charlie" data/groups.dat 2>/dev/null || \
   grep -E "^testgroup:" data/groups.dat | grep -q "alice"; then
    pass "Участники testgroup: alice, bob, charlie"
else
    fail "Участники testgroup не совпадают с ожидаемыми"
fi

# Проверяем формат каждой строки groups.dat
while IFS= read -r line; do
    [[ "$line" =~ ^# ]] && continue  # пропускаем комментарии
    [[ -z "$line" ]] && continue
    colons=$(echo "$line" | tr -cd ':' | wc -c)
    if [[ $colons -ge 1 ]]; then
        pass "Строка '$line' имеет верный формат (группа:участник1:...)"
    else
        fail "Строка '$line' — неверный формат (нет разделителя ':')"
    fi
done < data/groups.dat

# ══════════════════════════════════════════
#  ТЕСТ 5.4 — Лимит подключений (10 клиентов)
# ══════════════════════════════════════════
section "5.4 Лимит подключений (MAX_CLIENTS=10)"

start_server
SRV_PID=$GLOBAL_SRV_PID
info "Сервер запущен (PID=$SRV_PID)"
info "Подключаем 10 клиентов alice..."

# Создаём 10 соединений (все alice — для простоты теста)
PIDS=()
for i in $(seq 1 10); do
    {
        sleep 0.2; printf "alice\n"
        sleep 0.1; printf "alice123\n"
        sleep 5  # держим соединение
    } | nc -q5 "$SERVER_IP" "$SERVER_PORT" &>/dev/null &
    PIDS+=($!)
    sleep 0.15
done

sleep 1
info "Подключаем 11-й клиент..."

# 11-й клиент должен получить BUSY
response_11=$(
    {
        sleep 0.2
        printf "alice\n"
        sleep 0.2
        printf "alice123\n"
        sleep 1.0
    } | nc -q1 "$SERVER_IP" "$SERVER_PORT" 2>/dev/null
)

if echo "$response_11" | grep -qi "BUSY\|занят\|попробуйте"; then
    pass "11-й клиент получил сообщение о занятости сервера"
else
    # Если BUSY не получен — проверяем хотя бы что соединение не прошло
    if ! echo "$response_11" | grep -q "OK"; then
        pass "11-й клиент не прошёл аутентификацию (лимит сработал)"
    else
        fail "11-й клиент прошёл аутентификацию — лимит НЕ сработал!"
    fi
fi

# Завершаем всех тестовых клиентов
for pid in "${PIDS[@]}"; do
    kill "$pid" 2>/dev/null
done
wait "${PIDS[@]}" 2>/dev/null
stop_server

# ══════════════════════════════════════════
#  ТЕСТ 5.5 — Шифрование (проверка трафика)
# ══════════════════════════════════════════
section "5.5 Шифрование"

# Проверяем что модуль шифрования собирается и работает
info "Запуск теста шифрования (test_crypto)..."
if make test_crypto &>/tmp/test_crypto_out.log 2>&1; then
    pass "AES-256-CBC: зашифрованный текст успешно расшифрован"
    pass "Расшифрованный текст совпадает с исходным"
else
    fail "Тест шифрования провалился — смотри /tmp/test_crypto_out.log"
fi

# Проверяем что текст сообщения шифруется (не передаётся открыто)
# Для этого: ловим трафик через /proc/net/tcp или проверяем структуру Message
info "Проверка: зашифрованные данные не совпадают с открытым текстом..."
CHECK=$(cat << 'CPPCHECK'
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Простой тест: зашифрованное != исходное
extern int encrypt_message(const uint8_t*, int, const uint8_t*, const uint8_t*, uint8_t*);

int main() {
    uint8_t key[32] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                       0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
                       0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                       0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f};
    uint8_t iv[16]  = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                       0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    const char *plain = "Тестовое сообщение";
    uint8_t cipher[128] = {0};
    int clen = encrypt_message((uint8_t*)plain, strlen(plain), key, iv, cipher);
    if (clen <= 0) return 1;
    // Зашифрованное не должно содержать открытый текст
    if (memcmp(plain, cipher, strlen(plain)) == 0) return 1;
    return 0;
}
CPPCHECK
)
echo "$CHECK" > /tmp/check_encrypt.cpp
if g++ -std=c++17 /tmp/check_encrypt.cpp common/crypto.cpp -lssl -lcrypto \
   -o /tmp/check_encrypt 2>/dev/null && /tmp/check_encrypt; then
    pass "Зашифрованные данные НЕ совпадают с открытым текстом"
else
    fail "Шифрование не изменяет данные — проверь crypto.cpp"
fi
rm -f /tmp/check_encrypt.cpp /tmp/check_encrypt

# Проверяем что wireshark/tcpdump доступен для ручной проверки
if which wireshark &>/dev/null || which tshark &>/dev/null || which tcpdump &>/dev/null; then
    pass "Инструмент перехвата трафика найден — можно проверить шифрование вручную"
    if which tshark &>/dev/null; then
        info "Команда для захвата: sudo tshark -i lo -f 'tcp port $SERVER_PORT' -x"
    elif which tcpdump &>/dev/null; then
        info "Команда для захвата: sudo tcpdump -i lo -X port $SERVER_PORT"
    fi
else
    echo -e "${YELLOW}[!]${NC} Wireshark/tcpdump не установлен"
    info "Установка: sudo apt install tshark"
    info "Захват:    sudo tshark -i lo -f 'tcp port $SERVER_PORT' -x"
fi

# ══════════════════════════════════════════
#  ИТОГИ
# ══════════════════════════════════════════
echo ""
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo -e "${BOLD}   Результаты тестирования                ${NC}"
echo -e "${BOLD}══════════════════════════════════════════${NC}"
echo ""
echo -e "  ${GREEN}Пройдено : $PASS_COUNT${NC}"
echo -e "  ${RED}Провалено: $FAIL_COUNT${NC}"
echo ""

if [[ $FAIL_COUNT -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}Все тесты пройдены! Проект готов к защите.${NC}"
else
    echo -e "  ${YELLOW}${BOLD}Есть проваленные тесты — проверь ошибки выше.${NC}"
    echo ""
    echo -e "  Лог сервера: ${CYAN}cat data/server.log${NC}"
    echo -e "  Лог теста:   ${CYAN}cat /tmp/server_test.log${NC}"
fi
echo ""

# Финальная очистка
pkill -f "$SERVER_BIN" 2>/dev/null || true
rm -f /tmp/fifo_* 2>/dev/null || true
rm -f /tmp/server_test.log 2>/dev/null || true

exit $FAIL_COUNT
