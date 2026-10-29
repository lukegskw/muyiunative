# Runtime unit tests in one command: make -C runtime/tests run (seconds; builds into build/, which git ignores). make asan: the same tests under ASan and UBSan, built in
# build/asan so the plain build stays as it is. A test includes the files it tests (t_mem.c includes ../mem.c), so every runtime file is a dependency of every test
B = build
SAN ?=
CFLAGS = -O1 -g -Wall -Wextra -I.. $(SAN)
TESTS = t_mem
.DEFAULT_GOAL := run

run: $(addprefix $(B)/,$(TESTS))
	@set -e; for t in $^; do $$t; done

$(B)/t_%: t_%.c $(wildcard ../*.c ../*.h) Makefile
	@mkdir -p $(B)
	clang $(CFLAGS) -o $@ $<

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) B=$(B)/asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' run

clean:
	rm -rf $(B)

.PHONY: run asan clean
