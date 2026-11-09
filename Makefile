# CampusCompute - Makefile (Linux / Ubuntu / WSL Ubuntu, GNU make, gcc)
#
#   make            build ./campuscompute
#   make run        run the integrated demo
#   make test       build + unit tests + CLI/integration tests
#   make clean      remove build products
#   make clean-ipc  remove POSIX shm/semaphore objects created by this program
#   make clean-logs remove logs/*.log produced by this program
#   make dist       build CampusCompute_submission.zip
#   make verify-dist  list and integrity-check that archive
#   make help       list targets

CC       ?= gcc
BIN      := campuscompute
BUILD    := build

# -std=c11 plus the POSIX/GNU feature macros needed for sem_*, shm_*, nanosleep,
# strsignal, WCOREDUMP, ... Warnings are deliberately strict.
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
            -Wformat=2 -Wcast-qual -Wwrite-strings -Wundef
CFLAGS   ?= -g -O2
CFLAGS   += -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE $(WARN) \
            -Iinclude -pthread -MMD -MP $(SAN)
# -pthread : POSIX threads + mutex/condvar     -lrt : shm_open/sem_open on older glibc
LDFLAGS  ?=
SAN      ?=
LDLIBS   := $(SAN) -pthread -lrt

SRCS     := $(wildcard src/*.c)
OBJS     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
LIB_OBJS := $(filter-out $(BUILD)/main.o,$(OBJS))
DEPS     := $(OBJS:.o=.d) $(BUILD)/unit_tests.d

.PHONY: all run test unit integration asan tsan clean clean-ipc clean-logs \
        distclean dist verify-dist help

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# ---- tests ---------------------------------------------------------------
$(BUILD)/unit_tests: tests/unit_tests.c $(LIB_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit_tests.c $(LIB_OBJS) $(LDLIBS)

unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

integration: $(BIN)
	bash tests/run_tests.sh ./$(BIN)

test: unit integration

# Sanitizer builds (separate output tree so the normal build is untouched).
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread

asan:
	$(MAKE) BUILD=build-asan BIN=campuscompute_asan SAN="$(ASAN_FLAGS)" \
	    campuscompute_asan build-asan/unit_tests
	./build-asan/unit_tests
	bash tests/run_tests.sh ./campuscompute_asan

tsan:
	$(MAKE) BUILD=build-tsan BIN=campuscompute_tsan SAN="$(TSAN_FLAGS)" \
	    campuscompute_tsan
	./campuscompute_tsan logger --capacity 2 --producers 4 --messages 20 \
	    --consumer-delay 2 --log logs/tsan_check.log --quiet
	./campuscompute_tsan logger --capacity 1 --producers 8 --messages 50 \
	    --producer-delay 0 --consumer-delay 0 --log logs/tsan_check.log --quiet

# ---- running -------------------------------------------------------------
run: $(BIN)
	./$(BIN) demo

# ---- cleaning ------------------------------------------------------------
clean:
	rm -rf $(BUILD) build-asan build-tsan $(BIN) campuscompute_asan \
	    campuscompute_tsan

# Only removes the POSIX IPC objects this program names (per-user suffix).
clean-ipc: $(BIN)
	./$(BIN) cleanup

# Only removes *.log files inside this project's logs/ directory.
clean-logs:
	rm -f logs/*.log

distclean: clean clean-logs
	rm -rf .dist_stage

# Submission archive: sources, headers, Makefile, README, tests. A clean copy
# is staged first, so this works whatever the project folder is called and can
# never pick up build products, logs, the private docs/ notes or old archives.
dist: distclean
	rm -rf .dist_stage CampusCompute_submission.zip
	mkdir -p .dist_stage/CampusCompute/logs
	cp -r Makefile README.md src include tests .dist_stage/CampusCompute/
	touch .dist_stage/CampusCompute/logs/.keep
	cd .dist_stage && zip -qr ../CampusCompute_submission.zip CampusCompute \
	    -x '*.o' -x '*.d' -x '*__pycache__*'
	rm -rf .dist_stage
	unzip -l CampusCompute_submission.zip

# Integrity check of the archive made by 'make dist'.
verify-dist:
	@test -f CampusCompute_submission.zip || { echo "run 'make dist' first"; exit 1; }
	unzip -l CampusCompute_submission.zip
	unzip -t CampusCompute_submission.zip

help:
	@sed -n '2,13p' Makefile

-include $(DEPS)
