// Unit tests for the Arduino Mega gateway (NES_router_mega.ino): its
// start-up, its RAM budget, and the link layer tests it shares with the
// ESP32 original (link_tests.h).  There is no decoder on the Mega -
// scripts/serial_bridge.py decompresses pages, and run_tests.py tests
// that against this firmware's real output.
//
// The real ROM, and hot plugging against it, is in test_mega_link.cpp.
//
//   test_mega_gateway [path/to/nes_web_server.nes]
#include "avr_shim.h"
#include "../../src/firmware/NES_router_mega/NES_router_mega.ino"
#include "harness.h"
#include "link_tests.h"

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  std::vector<uint8_t> rom = readFile(romPath);
  std::vector<size_t> offsets = findPackets(rom);

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

  runLinkTests();
  return finish();
}
