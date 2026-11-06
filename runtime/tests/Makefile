# Runtime unit tests in one command: make -C runtime/tests run (seconds; builds into build/, which git ignores). make asan: the same tests under ASan and UBSan, built in
# build/asan so the plain build stays as it is. Each test links the whole runtime, built once into objects; t_boot runs the bundled OpenBIOS (bios/openbios)
B = build
SAN ?=
CFLAGS = -O1 -g -Wall -Wextra -I.. $(SAN)
TESTS = t_mem t_cpu t_cycles t_native t_overlays t_render t_lockstep t_dma t_timers t_gpu t_texture t_gte t_spu t_sio t_mdec t_cdrom t_disc t_cdswap t_cdread t_cdaudio t_homebrew t_boot t_ps1tests
RT = $(sort $(wildcard ../*.c))
OBJ = $(patsubst ../%.c,$(B)/rt/%.o,$(RT))
BIOS = $(abspath ../../bios/openbios/openbios.bin)
TESTS_DIR = $(abspath ../../tests)
TOOLS = ../../tools
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

# t_native links the native code the tools generate for its program, into the build folder only (plan.md, decision log)
$(B)/gen/native_cases.c: $(TOOLS)/tests/native_cases.py $(TOOLS)/tests/asm.py $(TOOLS)/codegen.py $(TOOLS)/analyze.py $(TOOLS)/mips.py
	@mkdir -p $(@D)
	python3 $(TOOLS)/tests/native_cases.py $@

$(B)/t_native: t_native.c $(B)/gen/native_cases.c $(OBJ) Makefile
	clang $(CFLAGS) -o $@ t_native.c $(B)/gen/native_cases.c $(OBJ)


$(B)/gen/overlay_cases.c: $(TOOLS)/tests/overlay_cases.py $(TOOLS)/tests/asm.py $(TOOLS)/codegen.py $(TOOLS)/overlays.py $(TOOLS)/analyze.py $(TOOLS)/mips.py
	@mkdir -p $(@D)
	python3 $(TOOLS)/tests/overlay_cases.py $@

$(B)/t_overlays: t_overlays.c $(B)/gen/overlay_cases.c $(OBJ) Makefile
	clang $(CFLAGS) -o $@ t_overlays.c $(B)/gen/overlay_cases.c $(OBJ)

# t_lockstep runs ps1-tests programs as native code generated from them here: $(1), the set's name; $(2), the program in tests/ps1-tests
CODEGEN = $(TOOLS)/codegen.py $(TOOLS)/analyze.py $(TOOLS)/mips.py $(TOOLS)/exe.py $(TOOLS)/overlays.py
define native_exe
$(B)/gen/$(1).c: $(TESTS_DIR)/ps1-tests/$(2) $(CODEGEN)
	@mkdir -p $$(@D)
	python3 $(TOOLS)/codegen.py $$< --name $(1) -o $$@
endef
$(eval $(call native_exe,cop,cpu/cop/cop.exe))
$(eval $(call native_exe,test_all,gte/test-all/test-all.exe))
$(eval $(call native_exe,otc_test,dma/otc-test/otc-test.exe))
# gpu/triangle's is split into files of about 400 instructions, as a game's is (scripts/native_game.py), so that the split's C links and runs here too
$(B)/gen/triangle/triangle.c: $(TESTS_DIR)/ps1-tests/gpu/triangle/triangle.exe $(CODEGEN)
	@mkdir -p $(@D)
	python3 $(TOOLS)/codegen.py $< --name triangle -o $@ --max-ops 400

# OpenBIOS's native code: the capture runs it in the interpreter to its shell, for the RAM it copies its kernel and shell into and the functions it reaches there
$(B)/capture: $(TOOLS)/capture.c $(OBJ) Makefile
	clang $(CFLAGS) -o $@ $< $(OBJ)
$(B)/gen/openbios.cap.txt: $(B)/capture $(BIOS)
	@mkdir -p $(@D)
	$(B)/capture $(BIOS) 60 $(B)/gen/openbios.cap
$(B)/gen/openbios.c: $(B)/gen/openbios.cap.txt $(CODEGEN)
	python3 $(TOOLS)/codegen.py $(BIOS) --bios --capture $(B)/gen/openbios.cap --name openbios -o $@
LOCKSTEP_GEN = $(addprefix $(B)/gen/,openbios.c cop.c test_all.c otc_test.c triangle/triangle.c)

$(B)/t_lockstep: t_lockstep.c $(LOCKSTEP_GEN) $(OBJ) Makefile
	clang $(CFLAGS) -DBIOS_PATH='"$(BIOS)"' -DTESTS_DIR='"$(TESTS_DIR)"' -o $@ t_lockstep.c $(filter-out %/triangle.c,$(LOCKSTEP_GEN)) $(B)/gen/triangle/*.c $(OBJ)

asan:
	ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) B=$(B)/asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' run

clean:
	rm -rf $(B)

.PHONY: run asan clean
