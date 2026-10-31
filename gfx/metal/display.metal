// Display scanout from the independently rendered VRAM (plan.md, phase 6). psx-spx GP1(05h..08h), GPU Video Memory: 15-bit RGB or packed 24-bit RGB with row wrapping.
// 24-bit video remains at its source resolution; geometry in the 15-bit target has its own subpixel samples. No shader is compiled by the app.
#include <metal_stdlib>
using namespace metal;
struct Display { uint x, y, mode, off, width, height, scale; };
static uint expand(uint x) { return x << 3 | x >> 2; }
kernel void render_display(device const ushort *native [[buffer(0)]], device const ushort *scaled [[buffer(1)]],
    constant Display &p [[buffer(2)]], device uint *out [[buffer(3)]], uint2 pos [[thread_position_in_grid]]) {
    if (pos.x >= p.width || pos.y >= p.height) return;
    uint rgb = 0, x = pos.x / p.scale, y = pos.y / p.scale;
    if (!p.off) {
        if (p.mode & 16) {
            uint row = ((p.y + y) & 511) * 1024, k = p.x * 2 + x * 3;
            for (uint i = 0; i < 3; i++) rgb |= uint(native[row + (((k + i) >> 1) & 1023)] >> (((k + i) & 1) * 8) & 255) << (16 - i * 8);
        } else {
            uint c = p.scale == 1 ? native[((p.y + y) & 511) * 1024 + ((p.x + x) & 1023)] :
                scaled[((p.y * p.scale + pos.y) % (512 * p.scale)) * 1024 * p.scale + ((p.x * p.scale + pos.x) % (1024 * p.scale))];
            rgb = expand(c & 31) << 16 | expand(c >> 5 & 31) << 8 | expand(c >> 10 & 31);
        }
    }
    out[pos.y * p.width + pos.x] = 0xff000000 | rgb;
}
