# The Mac host and app. make -C shell/mac: the host, build/ps1native; make -C shell/mac app: build/ps1native.app, packed and signed by tools/pack_app.py with the bundled
# OpenBIOS in its Resources (docs/development.md). Apple silicon only, macOS 14 or later. One clang command builds the host from every source; the runtime's are taken by
# wildcard, so a new file does not touch this line
B = build
MINOS = 14.0
RT = ../../runtime
GFX = ../../gfx/metal
BIOS = ../../bios/openbios
SRC = main.c front_mac.m $(GFX)/present.m $(sort $(wildcard $(RT)/*.c))
HDR = front.h $(GFX)/present.h $(wildcard $(RT)/*.h)
CFLAGS = -arch arm64 -mmacosx-version-min=$(MINOS) -O2 -g -Wall -Wextra -fobjc-arc -I$(RT) -I$(GFX)
FRAMEWORKS = -framework AppKit -framework QuartzCore -framework Metal -framework CoreGraphics -framework ImageIO
.DEFAULT_GOAL := host

host: $(B)/ps1native

$(B)/ps1native: $(SRC) $(HDR) Makefile
	@mkdir -p $(B)
	clang $(CFLAGS) -o $@ $(SRC) $(FRAMEWORKS)

app: $(B)/ps1native $(wildcard $(BIOS)/*)
	python3 ../../tools/pack_app.py $(B)/ps1native.app --exe $(B)/ps1native --resource $(BIOS)

clean:
	rm -rf $(B)

.PHONY: host app clean
