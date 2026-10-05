# Servidor central — compilación con gcc.
#
#   make              compila ./server (autenticación PAM)
#   make stub         compila ./server-stub (autenticación de prueba, solo tests)
#   make test         compila server-stub y corre tests/test_server.py
#   make install-pam  instala server/pam/monitoreo en /etc/pam.d (requiere sudo)
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -pthread
LDLIBS  := -pthread -lm

SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
OBJ     := $(patsubst src/%.c,build/%.o,$(SRC))
OBJ_STUB:= $(patsubst src/%.c,build/stub/%.o,$(SRC))

.PHONY: all stub test clean install-pam

all: server

server: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -lpam

stub: server-stub

server-stub: $(OBJ_STUB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDR) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/stub/%.o: src/%.c $(HDR) | build/stub
	$(CC) $(CFLAGS) -DMCS_AUTH_STUB -c -o $@ $<

build build/stub:
	mkdir -p $@

test: server-stub
	python3 ../tests/test_server.py

# Debian/Ubuntu usan common-auth; Arch/Fedora usan system-auth.
install-pam:
	@if [ -f /etc/pam.d/common-auth ]; then \
	    install -m 644 pam/monitoreo /etc/pam.d/monitoreo; \
	else \
	    install -m 644 pam/monitoreo.arch /etc/pam.d/monitoreo; \
	fi
	@echo "instalado /etc/pam.d/monitoreo"

clean:
	rm -rf build server server-stub
