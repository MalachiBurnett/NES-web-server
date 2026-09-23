// Unit tests for the gateway firmware, in whichever build run_tests.py
// compiles it for (see arduino_shim.h): its start-up, its RAM budget,
// packet decoding where the board decodes, and the link layer
// (link_tests.h).
//
// The real ROM, and hot plugging against it, is in test_link.cpp.
//
//   test_gateway [path/to/nes_web_server.nes]
#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"
#include "harness.h"
#include "link_tests.h"

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  std::vector<uint8_t> rom = readFile(romPath);
  std::vector<size_t> offsets = findPackets(rom);
  printf("%s gateway, CLK on %s %d, OUT0 on %s %d, D0 on %s %d\n\n", BOARD_NAME,
         PIN_WORD, PIN_NES_CLOCK, PIN_WORD, PIN_NES_DATA_IN, PIN_WORD, PIN_NES_DATA_OUT);

  printf("start-up\n");
  g_serial_echo = false;
  setup();
  g_serial_echo = true;
  check(g_serial_out.find("online") != std::string::npos,
        "announces itself online (serial_bridge.py waits for it)");
  check(wireHigh() && linkState == LINK_IDLE, "holds D0 idle, the wire high, from the start");

  printf("\nRAM\n");
  check(offsets.size() == 3, "the ROM holds three pages");
  const char *names[] = {"index.html", "style.css", "404"};
  for (size_t i = 0; i < offsets.size() && i < 3; i++) {
    char label[96];
    snprintf(label, sizeof label, "%s: %u byte packet fits the %u byte buffer",
             names[i], (unsigned)storedLength(rom, offsets[i]), (unsigned)MAX_PACKET);
    check(storedLength(rom, offsets[i]) <= MAX_PACKET, label);
  }

#if DECODES
  printf("\ndecoding the real packets\n");
  for (size_t i = 0; i < 3 && i < offsets.size(); i++) {
    size_t off = offsets[i];
    uint16_t declared = storedLength(rom, off);
    char *out = nullptr;
    uint32_t outLen = 0;
    bool ok = decodePacket(&rom[off], declared, &out, &outLen);
    char label[96];
    snprintf(label, sizeof label, "%s decodes (%u byte packet)", names[i], declared);
    check(ok, label);
    if (ok) {
      snprintf(label, sizeof label, "%s is NUL terminated at %u bytes", names[i], outLen);
      check(out[outLen] == '\0' && strlen(out) == outLen, label);
      free(out);
    }
  }

  printf("\nmalformed packets are rejected, not decoded\n");
  if (!offsets.empty()) {
    size_t off = offsets[0];
    uint16_t declared = storedLength(rom, off);
    std::vector<uint8_t> pkt(&rom[off], &rom[off] + declared);
    char *out = nullptr;
    uint32_t outLen = 0;
    g_serial_echo = false;

    std::vector<uint8_t> bad = pkt;
    bad[3] = '9';
    check(!decodePacket(bad.data(), bad.size(), &out, &outLen), "wrong magic");

    check(!decodePacket(pkt.data(), 4, &out, &outLen), "truncated to the magic");
    check(!decodePacket(pkt.data(), pkt.size() / 2, &out, &outLen), "truncated mid packet");

    bad = pkt;
    for (size_t i = 40; i < 80 && i < bad.size(); i++) bad[i] ^= 0xFF;
    bool ok = decodePacket(bad.data(), bad.size(), &out, &outLen);
    check(!ok, "corrupted dictionary/tree region");
    if (ok) free(out);
    g_serial_echo = true;
  }
#endif

  runLinkTests();
  return finish();
}
