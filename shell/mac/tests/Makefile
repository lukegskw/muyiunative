# Mac host tests beside their code (plan.md, phases 4-7): replay, keyboard, controllers, playback policy and saves need no Apple frameworks, devices, game, BIOS or GPU.
# make run checks them; make asan builds the same checks with ASan/UBSan in build/asan. Only persistence needs the runtime, built once into objects.
B = build
SAN ?=
RT = ../../../runtime
CFLAGS = -O1 -g -Wall -Wextra -I$(RT) $(SAN)
TESTS = t_keyboard t_controllers t_replay t_audio t_saves
OBJ = $(patsubst $(RT)/%.c,$(B)/rt/%.o,$(sort $(wildcard $(RT)/*.c)))
.DEFAULT_GOAL := run
.SECONDARY: $(OBJ)

run: $(addprefix $(B)/,$(TESTS))
	@set -e; for t in $^; do $$t; done

$(B)/t_%: t_%.c $(wildcard ../*.h) $(wildcard $(RT)/*.h) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -o $@ $<

$(B)/rt/%.o: $(RT)/%.c $(wildcard $(RT)/*.h) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -c -o $@ $<

$(B)/t_saves: t_saves.c ../saves.c ../saves.h $(OBJ) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -o $@ t_saves.c ../saves.c $(OBJ)

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) B=$(B)/asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' run

clean:
	rm -rf $(B)

.PHONY: run asan clean
