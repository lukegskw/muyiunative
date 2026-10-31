# Explicit local GPU validation (plan.md, phase 6); default CI remains CPU-only. Shader source is compiled offline; tests never compile source at runtime.
B = build
RT = ../../../runtime
SRC = $(sort $(wildcard $(RT)/*.c))
AIR = $(B)/renderer.air $(B)/display.air
CFLAGS = -arch arm64 -mmacosx-version-min=14.0 -O2 -g -Wall -Wextra -fobjc-arc -I$(RT)
.DEFAULT_GOAL := run

$(B)/%.air: ../%.metal $(RT)/render.h Makefile
	@mkdir -p $(@D)
	xcrun -sdk macosx metal -std=metal3.0 -mmacosx-version-min=14.0 -I$(RT) -c $< -o $@

$(B)/renderer.metallib: $(AIR)
	xcrun -sdk macosx metallib $^ -o $@

$(B)/t_renderer: t_renderer.m ../renderer.m ../renderer.h $(SRC) $(wildcard $(RT)/*.h) $(B)/renderer.metallib Makefile
	clang $(CFLAGS) -DMETALLIB_PATH='"$(abspath $(B)/renderer.metallib)"' -o $@ t_renderer.m ../renderer.m $(SRC) -framework Foundation -framework Metal

run: $(B)/t_renderer
	$(B)/t_renderer

clean:
	rm -rf $(B)

.PHONY: run clean
