# Concurrent Encryption Service
#
#   make                  build encserver + encclient for QNX 8.0 on aarch64le (Raspberry Pi 4B)
#   make CRYPTO=builtin   same, but use the bundled AES instead of OpenSSL
#   make test             build and run the portable-core tests on Linux/WSL with gcc
#   make tsan             the same tests under ThreadSanitizer
#   make qnx-syntax       compile-check the QNX sources on Linux against stub headers
#   make deploy           copy programs, scripts and examples to the Pi (PI=, PI_DIR= to change)
#   make clean
#
# The QNX build needs the SDP environment first:
#   Windows: run qnxsdp-env.bat     Linux: source qnxsdp-env.sh

CRYPTO      ?= openssl
QNX_VARIANT ?= gcc_ntoaarch64le
QCC         ?= qcc
HOSTCC      ?= gcc

CORE_SRC   := src/core/cipher_common.c src/core/worker_pool.c src/core/segment.c \
              src/core/job.c src/core/util.c src/core/display.c src/core/encfile.c \
              src/core/sync.c
CLIENT_SRC := src/qnx/client.c src/qnx/client_file.c src/qnx/client_msg.c
ifeq ($(CRYPTO),builtin)
CIPHER_SRC := src/core/cipher_builtin.c src/core/aes256.c
CRYPTO_LIB :=
else
CIPHER_SRC := src/core/cipher_openssl.c
CRYPTO_LIB := -lcrypto
endif

COMMON_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Isrc/core

# ---------------------------------------------------------------- QNX target
QNX_OUT    := bin/aarch64le
# _QNX_SOURCE keeps the QNX/POSIX declarations visible when -std= is given.
# No -pthread or -lpthread: on QNX the thread functions are in libc.
QNX_CFLAGS := -V$(QNX_VARIANT) $(COMMON_CFLAGS) -D_QNX_SOURCE -Isrc/qnx

all: $(QNX_OUT)/encserver $(QNX_OUT)/encclient

$(QNX_OUT)/encserver: src/qnx/server.c $(CORE_SRC) $(CIPHER_SRC) src/core/*.h src/qnx/*.h
	$(QCC) $(QNX_CFLAGS) -o $@ src/qnx/server.c $(CORE_SRC) $(CIPHER_SRC) $(CRYPTO_LIB)

$(QNX_OUT)/encclient: $(CLIENT_SRC) $(CORE_SRC) $(CIPHER_SRC) src/core/*.h src/qnx/*.h
	$(QCC) $(QNX_CFLAGS) -o $@ $(CLIENT_SRC) $(CORE_SRC) $(CIPHER_SRC) $(CRYPTO_LIB)

# ---------------------------------------------------------------- host tests
HOST_OUT    := build/host
HOST_CFLAGS := $(COMMON_CFLAGS) -g -pthread

define host_tests
	mkdir -p $(HOST_OUT)
	$(HOSTCC) $(HOST_CFLAGS) $(1) -o $(HOST_OUT)/test_cipher_openssl tests/test_cipher.c \
		$(CORE_SRC) src/core/cipher_openssl.c -lcrypto
	$(HOSTCC) $(HOST_CFLAGS) $(1) -o $(HOST_OUT)/test_cipher_builtin tests/test_cipher.c \
		$(CORE_SRC) src/core/cipher_builtin.c src/core/aes256.c
	$(HOSTCC) $(HOST_CFLAGS) $(1) -o $(HOST_OUT)/test_job_openssl tests/test_job.c \
		$(CORE_SRC) src/core/cipher_openssl.c -lcrypto
	$(HOSTCC) $(HOST_CFLAGS) $(1) -o $(HOST_OUT)/test_job_builtin tests/test_job.c \
		$(CORE_SRC) src/core/cipher_builtin.c src/core/aes256.c
	$(2) $(HOST_OUT)/test_cipher_openssl -d $(HOST_OUT)/ctr_openssl.bin
	$(2) $(HOST_OUT)/test_cipher_builtin -d $(HOST_OUT)/ctr_builtin.bin
	cmp $(HOST_OUT)/ctr_openssl.bin $(HOST_OUT)/ctr_builtin.bin && echo "backends agree: passed"
	$(2) $(HOST_OUT)/test_job_openssl
	$(2) $(HOST_OUT)/test_job_builtin
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -Dmain=encserver_main -c src/qnx/server.c \
		-o $(HOST_OUT)/e2e_server.o
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -Dmain=encclient_main -c src/qnx/client.c \
		-o $(HOST_OUT)/e2e_client.o
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -c src/qnx/client_file.c -o $(HOST_OUT)/e2e_client_file.o
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -c src/qnx/client_msg.c -o $(HOST_OUT)/e2e_client_msg.o
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -o $(HOST_OUT)/test_e2e tests/test_e2e.c \
		tests/qnx_shim.c $(HOST_OUT)/e2e_server.o $(HOST_OUT)/e2e_client.o \
		$(HOST_OUT)/e2e_client_file.o $(HOST_OUT)/e2e_client_msg.o \
		$(CORE_SRC) src/core/cipher_openssl.c -lcrypto
	$(2) $(HOST_OUT)/test_e2e > $(HOST_OUT)/test_e2e.log || (cat $(HOST_OUT)/test_e2e.log; false)
	tail -n 2 $(HOST_OUT)/test_e2e.log
endef

# End-to-end test: real server.c + client.c on an in-process message-passing shim.
E2E_INC := -Isrc/qnx -Itests/qnx_stub

ASAN_FLAGS := -fsanitize=address,undefined

test:
	$(call host_tests,$(ASAN_FLAGS))

tsan:
	$(call host_tests,-fsanitize=thread,setarch $$(uname -m) -R)

qnx-syntax:
	$(HOSTCC) $(COMMON_CFLAGS) -fsyntax-only -Isrc/qnx -Itests/qnx_stub -D__QNX__ src/qnx/server.c
	$(HOSTCC) $(COMMON_CFLAGS) -fsyntax-only -Isrc/qnx -Itests/qnx_stub -D__QNX__ src/qnx/client.c
	$(HOSTCC) $(COMMON_CFLAGS) -fsyntax-only -Isrc/qnx -Itests/qnx_stub -D__QNX__ src/qnx/client_file.c
	$(HOSTCC) $(COMMON_CFLAGS) -fsyntax-only -Isrc/qnx -Itests/qnx_stub -D__QNX__ src/qnx/client_msg.c
	@echo "QNX sources compile-check: passed"

# ---------------------------------------------------------------- deploy to the Pi
# Build first (make, or make CRYPTO=builtin), then: make deploy
# No shell wildcards on this side, so it also works from a Windows command prompt.
PI       ?= qnxuser@10.0.0.1
PI_DIR   ?= /var/encsvc
SSH_OPTS ?= -o MACs=hmac-sha2-256

deploy:
	ssh $(SSH_OPTS) $(PI) "mkdir -p $(PI_DIR)"
	scp $(SSH_OPTS) $(QNX_OUT)/encserver $(QNX_OUT)/encclient scripts/demo.sh scripts/bench.sh \
		scripts/lib.sh $(PI):$(PI_DIR)/
	scp -r $(SSH_OPTS) examples $(PI):$(PI_DIR)/
	ssh $(SSH_OPTS) $(PI) "cd $(PI_DIR) && chmod +x encserver encclient demo.sh bench.sh"
	@echo "deployed to $(PI):$(PI_DIR)"

clean:
	rm -f $(QNX_OUT)/encserver $(QNX_OUT)/encclient
	rm -rf $(HOST_OUT)

.PHONY: all test tsan qnx-syntax deploy clean
