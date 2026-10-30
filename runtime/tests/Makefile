# Runtime unit tests in one command: make -C runtime/tests run (seconds; builds into build/, which git ignores). make asan: the same tests under ASan and UBSan, built in
# build/asan so the plain build stays as it is. Each test links the whole runtime, built once into objects; t_boot runs the bundled OpenBIOS (bios/openbios)
B = build
SAN ?=
CFLAGS = -O1 -g -Wall -Wextra -I.. $(SAN)
TESTS = t_mem t_cpu t_cycles t_dma t_timers t_gpu t_texture t_gte t_spu t_sio t_mdec t_cdrom t_disc t_cdread t_cdaudio t_homebrew t_boot t_ps1tests
RT = $(sort $(wildcard ../*.c))
OBJ = $(patsubst ../%.c,$(B)/rt/%.o,$(RT))
BIOS = $(abspath ../../bios/openbios/openbios.bin)
TESTS_DIR = $(abspath ../../tests)
.DEFAULT_GOAL := run
.SECONDARY: $(OBJ)

run: $(addprefix $(B)/,$(TESTS))
	@set -e; for t in $^; do $$t; done

$(B)/rt/%.o: ../%.c $(wildcard ../*.h) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -c -o $@ $<

$(B)/t_%: t_%.c $(OBJ) Makefile
	@mkdir -p $(B)
	clang $(CFLAGS) -DBIOS_PATH='"$(BIOS)"' -DTESTS_DIR='"$(TESTS_DIR)"' -o $@ $< $(OBJ)

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) B=$(B)/asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' run

clean:
	rm -rf $(B)

.PHONY: run asan clean
