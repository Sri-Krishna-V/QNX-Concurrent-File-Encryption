# Concurrent Encryption Service
#
#   make                  build encserver + encclient for QNX 8.0 on aarch64le (Raspberry Pi 4B)
#   make CRYPTO=builtin   same, but use the bundled AES instead of OpenSSL
#   make test             build and run the portable-core tests on Linux/WSL with gcc
#   make tsan             the same tests under ThreadSanitizer
#   make qnx-syntax       compile-check the QNX sources on Linux against stub headers
#   make clean
#
# The QNX build needs the SDP environment first:
#   Windows: run qnxsdp-env.bat     Linux: source qnxsdp-env.sh

CRYPTO      ?= openssl
QNX_VARIANT ?= gcc_ntoaarch64le
QCC         ?= qcc
HOSTCC      ?= gcc

CORE_SRC   := src/core/cipher_common.c src/core/worker_pool.c src/core/segment.c \
              src/core/job.c src/core/util.c
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

$(QNX_OUT)/encclient: src/qnx/client.c $(CORE_SRC) $(CIPHER_SRC) src/core/*.h src/qnx/*.h
	$(QCC) $(QNX_CFLAGS) -o $@ src/qnx/client.c $(CORE_SRC) $(CIPHER_SRC) $(CRYPTO_LIB)

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
	$(HOSTCC) $(HOST_CFLAGS) $(1) $(E2E_INC) -o $(HOST_OUT)/test_e2e tests/test_e2e.c \
		tests/qnx_shim.c $(HOST_OUT)/e2e_server.o $(HOST_OUT)/e2e_client.o \
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
	@echo "QNX sources compile-check: passed"

clean:
	rm -f $(QNX_OUT)/encserver $(QNX_OUT)/encclient
	rm -rf $(HOST_OUT)

.PHONY: all test tsan qnx-syntax clean
