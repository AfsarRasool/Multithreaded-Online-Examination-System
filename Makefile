CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread -Iinclude -Isrc/server

SERVER_SRC = $(wildcard src/server/*.c src/common/*.c src/exam/*.c \
             src/quiz/*.c src/results/*.c)
CLIENT_SRC = $(wildcard src/client/*.c src/common/*.c)

all: server client

server: $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f server client
