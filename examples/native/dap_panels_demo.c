/*
 * Demo for Memory / Disassembly / Variables panels (lldb-dap).
 *
 * Build:
 *   ./examples/native/build.sh
 *
 * Run:
 *   ./cpp/build/tui-debug-ui examples/native/dap_panels_demo
 *
 * Tips:
 *   - Debugger stops in main() on launch.
 *   - Add "Memory" and "Disassembly (ASM)" panels from the + menu.
 *   - Inspect Locals / Registers; step into mix_buffer() for more instructions.
 */
#include <stdint.h>
#include <stdio.h>

static uint8_t g_buffer[32] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB,
    0xCC, 0xDD, 0xEE, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0,
};

static uint32_t mix_buffer(uint32_t seed) {
    uint32_t acc = seed;
    for (int i = 0; i < 32; ++i) {
        acc ^= (uint32_t)g_buffer[i] << ((i % 4) * 8);
        acc = (acc << 3) | (acc >> 29);
    }
    return acc;
}

static int checksum(const char* label, uint32_t value) {
    printf("%s checksum = 0x%08x\n", label, value);
    return (int)(value & 0xFFu);
}

int main(void) {
    const uint32_t mixed = mix_buffer(0x12345678u);
    const int tail = checksum("demo", mixed);

    g_buffer[0] = (uint8_t)(tail & 0xFFu);
    printf("buffer[0] now 0x%02x — set a breakpoint here and inspect memory\n", g_buffer[0]);
    return tail;
}
