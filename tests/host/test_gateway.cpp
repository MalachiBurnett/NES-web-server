// Unit tests for the ESP32 gateway firmware: packet decoding, rejection
// of malformed packets, and the link layer (link_tests.h, which the
// Arduino Mega port shares).
//
// The real ROM, and hot plugging against it, is in test_rom_link.cpp.
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
  printf("packets in ROM: %zu\n\n", offsets.size());
  if (offsets.size() < 3) { printf("expected 3 packets\n"); return 1; }

  const char *names[] = {"index.html", "style.css", "404"};

  printf("decoding the real packets\n");
  for (size_t i = 0; i < 3; i++) {
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
  {
    size_t off = offsets[0];
    uint16_t declared = storedLength(rom, off);
    std::vector<uint8_t> pkt(&rom[off], &rom[off] + declared);
    char *out = nullptr;
    uint32_t outLen = 0;

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
  }

  runLinkTests();
  return finish();
}
