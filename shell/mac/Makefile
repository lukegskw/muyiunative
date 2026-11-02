# The Mac host and app. make -C shell/mac: the host, build/ps1native; make -C shell/mac app: build/ps1native.app, packed and signed by tools/pack_app.py with the bundled
# OpenBIOS in its Resources (docs/development.md). Apple silicon only, macOS 14 or later. One clang command builds the host from every source; the runtime's are taken by
# wildcard, so a new file does not touch this line. The host links OpenBIOS's native code, generated here from the bundled binary (plan.md, decision log): the capture
# runs OpenBIOS in the interpreter for what it copies into RAM and runs there (tools/capture.c), and tools/codegen.py writes the C, into build/gen only.
# The host build compiles gfx/metal/*.metal into build/gen/ps1gpu.metallib with xcrun (Xcode's Metal Toolchain); make app bundles that offline library.
B = build
GEN = $(B)/gen
MINOS = 14.0
RT = ../../runtime
GFX = ../../gfx/metal
BIOS = ../../bios/openbios
TOOLS = ../../tools
LIB = $(GEN)/ps1gpu.metallib
RTSRC = $(sort $(wildcard $(RT)/*.c))
SRC = main.c front_mac.m keyboard.m product.m saves.c $(GFX)/present.m $(GFX)/renderer.m $(RTSRC) $(GEN)/openbios.c
HDR = front.h audio.h input.h keys.h keyboard.h product.h saves.h $(GFX)/present.h $(GFX)/renderer.h $(wildcard $(RT)/*.h)
CODEGEN = $(TOOLS)/codegen.py $(TOOLS)/analyze.py $(TOOLS)/mips.py $(TOOLS)/exe.py $(TOOLS)/overlays.py
CFLAGS = -arch arm64 -mmacosx-version-min=$(MINOS) -O2 -g -Wall -Wextra -fobjc-arc -I$(RT) -I$(GFX)
FRAMEWORKS = -framework AppKit -framework QuartzCore -framework Metal -framework CoreGraphics -framework ImageIO -framework AudioToolbox -framework UniformTypeIdentifiers
.DEFAULT_GOAL := host

host: $(B)/ps1native

$(B)/ps1native: $(SRC) $(HDR) Makefile | $(LIB)
	@mkdir -p $(B)
	clang $(CFLAGS) -o $@ $(SRC) $(FRAMEWORKS)

$(B)/capture: $(TOOLS)/capture.c $(RTSRC) $(wildcard $(RT)/*.h) Makefile
	@mkdir -p $(B)
	clang -O2 -g -Wall -Wextra -I$(RT) -o $@ $(TOOLS)/capture.c $(RTSRC)

$(GEN)/openbios.cap.txt: $(B)/capture $(BIOS)/openbios.bin
	@mkdir -p $(GEN)
	$(B)/capture $(BIOS)/openbios.bin 60 $(GEN)/openbios.cap

$(GEN)/openbios.c: $(GEN)/openbios.cap.txt $(CODEGEN)
	python3 $(TOOLS)/codegen.py $(BIOS)/openbios.bin --bios --capture $(GEN)/openbios.cap --name openbios -o $@

$(GEN)/%.air: $(GFX)/%.metal $(RT)/render.h Makefile
	@mkdir -p $(@D)
	xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=$(MINOS) -c $< -o $@

$(LIB): $(GEN)/renderer.air $(GEN)/display.air
	xcrun -sdk macosx metallib $^ -o $@

# A game's host (scripts/native_game.py): GAME_C is the native code tools/codegen.py wrote for it, set native_game, in build/gen: one file, or the tables and the files of
# functions it was split into, each compiled on its own (make -j). It goes to build/game, apart from the host above, and like the C it is built from it holds the game's
# code: it stays on the Mac that built it
GAME_C ?=
GAME_O = $(patsubst %.c,$(B)/game/%.o,$(notdir $(GAME_C)))
game: $(B)/game/ps1native

$(B)/game/%.o: $(GEN)/%.c $(HDR) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -c -o $@ $<

$(B)/game/ps1native: $(SRC) $(HDR) $(GAME_O) Makefile | $(LIB)
	@test -n "$(GAME_C)" || { echo "make game needs GAME_C=<the game's generated C, in build/gen>" >&2; exit 1; }
	clang $(CFLAGS) -DNATIVE_GAME -o $@ $(SRC) $(GAME_O) $(FRAMEWORKS)

app: $(B)/ps1native $(wildcard $(BIOS)/*)
	python3 ../../tools/pack_app.py $(B)/ps1native.app --exe $(B)/ps1native --resource $(BIOS) --resource $(LIB)

clean:
	rm -rf $(B)

.PHONY: host app game clean
