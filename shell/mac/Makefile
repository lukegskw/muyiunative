# The Mac host and app. make -C shell/mac: the host, build/muyiunative; make -C shell/mac app: build/muyiunative.app, packed and signed by tools/pack_app.py with the bundled
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
PYTHON ?= python3
PREBUILT_METALLIB ?=
LIB = $(GEN)/ps1gpu.metallib
RTSRC = $(sort $(wildcard $(RT)/*.c))
SRC = main.c front_mac.m audio_mac.m keyboard.m product.m catalog.m build.m reports.m updates.m log.c report.c saves.c state.c media.c $(GFX)/present.m $(GFX)/renderer.m $(RTSRC) $(GEN)/openbios.c
HDR = front.h audio.h build.h controllers.h input.h keys.h keyboard.h product.h log.h report.h saves.h state.h media.h updates.h $(GEN)/version.h $(GFX)/present.h $(GFX)/renderer.h $(wildcard $(RT)/*.h)
CODEGEN = $(TOOLS)/codegen.py $(TOOLS)/analyze.py $(TOOLS)/mips.py $(TOOLS)/exe.py $(TOOLS)/overlays.py
CFLAGS = -arch arm64 -mmacosx-version-min=$(MINOS) -O2 -g -Wall -Wextra -fobjc-arc -I$(RT) -I$(GFX) -I$(GEN)
FRAMEWORKS = -framework AppKit -framework QuartzCore -framework Metal -framework CoreGraphics -framework ImageIO -framework AudioToolbox -framework UniformTypeIdentifiers -framework GameController
.DEFAULT_GOAL := host

host: $(B)/muyiunative

$(GEN)/version.h: FORCE $(TOOLS)/version.py
	"$(PYTHON)" $(TOOLS)/version.py --header $@

FORCE:

$(B)/muyiunative: $(SRC) $(HDR) Makefile | $(LIB)
	@mkdir -p $(B)
	clang $(CFLAGS) -o $@ $(SRC) $(FRAMEWORKS)

$(B)/capture: $(TOOLS)/capture.c $(RTSRC) $(wildcard $(RT)/*.h) Makefile
	@mkdir -p $(B)
	clang -O2 -g -Wall -Wextra -I$(RT) -o $@ $(TOOLS)/capture.c $(RTSRC)

$(GEN)/openbios.cap.txt: $(B)/capture $(BIOS)/openbios.bin
	@mkdir -p $(GEN)
	$(B)/capture $(BIOS)/openbios.bin 60 $(GEN)/openbios.cap

$(GEN)/openbios.c: $(GEN)/openbios.cap.txt $(CODEGEN)
	"$(PYTHON)" $(TOOLS)/codegen.py $(BIOS)/openbios.bin --bios --capture $(GEN)/openbios.cap --name openbios -o $@

ifneq ($(PREBUILT_METALLIB),)
$(LIB): $(PREBUILT_METALLIB)
	@mkdir -p $(@D)
	cp "$<" "$@"
else

$(GEN)/%.air: $(GFX)/%.metal $(RT)/render.h Makefile
	@mkdir -p $(@D)
	xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=$(MINOS) -c $< -o $@

$(LIB): $(GEN)/renderer.air $(GEN)/display.air
	xcrun -sdk macosx metallib $^ -o $@
endif

# A game's host (scripts/native_game.py): GAME_C is the native code tools/codegen.py wrote for it, set native_game, in build/gen: one file, or the tables and the files of
# functions it was split into, each compiled on its own (make -j). It goes to build/game, apart from the host above, and like the C it is built from it holds the game's
# code: it stays on the Mac that built it
GAME_C ?=
GAME_O = $(patsubst %.c,$(B)/game/%.o,$(notdir $(GAME_C)))
game: $(B)/game/muyiunative

$(B)/game/%.o: $(GEN)/%.c $(HDR) Makefile
	@mkdir -p $(@D)
	clang $(CFLAGS) -c -o $@ $<

$(B)/game/muyiunative: $(SRC) $(HDR) $(GAME_O) Makefile | $(LIB)
	@test -n "$(GAME_C)" || { echo "make game needs GAME_C=<the game's generated C, in build/gen>" >&2; exit 1; }
	clang $(CFLAGS) -DNATIVE_GAME -o $@ $(SRC) $(GAME_O) $(FRAMEWORKS)

$(B)/sparkle/.archive-sha256: $(TOOLS)/sparkle.py $(TOOLS)/vendor/sparkle/config.json
	"$(PYTHON)" $(TOOLS)/sparkle.py $(B)/sparkle

app: $(B)/muyiunative $(B)/sparkle/.archive-sha256 $(wildcard $(BIOS)/*)
	"$(PYTHON)" $(TOOLS)/build_kit.py $(B)/BuildKit --metallib $(LIB)
	"$(PYTHON)" $(TOOLS)/pack_app.py $(B)/muyiunative.app --exe $(B)/muyiunative --resource $(BIOS) --resource $(LIB) --resource $(B)/BuildKit --sparkle $(B)/sparkle/Sparkle.framework

clean:
	rm -rf $(B)

.PHONY: host app game clean FORCE
