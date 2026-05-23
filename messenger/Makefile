CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
LIBS_SRV = -lssl -lcrypto
LIBS_CLT = -lssl -lcrypto 

# ──────────────────────────────────────────
#  Исходники
# ──────────────────────────────────────────
SRV_SRC = server/main.cpp \
          server/session.cpp \
          server/auth.cpp \
          server/logger.cpp \
          common/crypto.cpp

CLT_SRC = client/main.cpp \
          client/sender.cpp \
          client/receiver.cpp \
          client/ui.cpp \
          common/crypto.cpp

# ──────────────────────────────────────────
#  Таргеты
# ──────────────────────────────────────────
all: messenger_server messenger_client

# Примечание: бинарники называются messenger_server и messenger_client
# потому что в Linux нельзя создать файл с именем существующей папки
# (у нас есть папки server/ и client/)

messenger_server: $(SRV_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS_SRV)
	@echo "[OK] Сервер собран → ./messenger_server"

messenger_client: $(CLT_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS_CLT)
	@echo "[OK] Клиент собран → ./messenger_client"

# Изолированный тест шифрования (этап 1)
test_crypto:
	$(CXX) $(CXXFLAGS) common/test_crypto.cpp common/crypto.cpp \
	    -lssl -lcrypto -o test_crypto
	./test_crypto

clean:
	rm -f messenger_server messenger_client test_crypto
	rm -f data/server.log
	rm -f /tmp/fifo_*

.PHONY: all clean test_crypto