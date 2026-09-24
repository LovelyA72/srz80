#ifndef SRZ80_INPUT_H
#define SRZ80_INPUT_H
#include <stdint.h>

/* Provider JSON and wire contract: docs/host-input.md. No native structs are
   copied onto the wire. Multibyte mouse fields are little endian. */
#define SRH_INPUT_PROTOCOL "srz80.input.v1"
#define SRH_KEY_DOWN 1u
#define SRH_KEY_REPEAT 2u
#define SRH_KEY_PACKET_SIZE 2u
#define SRH_MOUSE_PACKET_SIZE 20u
enum SrhMouseEvent {
    SRH_MOUSE_RELATIVE = 1,
    SRH_MOUSE_ABSOLUTE = 2,
    SRH_MOUSE_BUTTON = 3,
    SRH_MOUSE_WHEEL = 4,
    SRH_MOUSE_LEAVE = 5
};
/* byte 0: event; byte 1: button down (otherwise zero); bytes 2..3:
   button (1 left, 2 middle, 3 right, 4 back, 5 forward); bytes 4..7: x;
   bytes 8..11: y; bytes 12..15: width; bytes 16..19: height.
   x/y: signed relative counts, absolute pixels, or signed wheel units
   (120 per notch). Width/height are nonzero only for absolute events.
   LEAVE ends absolute pointer presence; the next ABSOLUTE re-enters.
   Keyboard: {HID Keyboard/Keypad usage u8, flags u8}. */
static inline void srh_input_put_u32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16); out[3] = (uint8_t)(value >> 24);
}
static inline uint32_t srh_input_get_u32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}
static inline void srh_mouse_packet(uint8_t out[SRH_MOUSE_PACKET_SIZE], uint8_t event,
                                    uint8_t down, uint16_t button, int32_t x, int32_t y,
                                    uint32_t width, uint32_t height) {
    out[0] = event; out[1] = down;
    out[2] = (uint8_t)button; out[3] = (uint8_t)(button >> 8);
    srh_input_put_u32(out + 4, (uint32_t)x); srh_input_put_u32(out + 8, (uint32_t)y);
    srh_input_put_u32(out + 12, width); srh_input_put_u32(out + 16, height);
}
#endif
