// Unit tests for the gateway firmware: packet decoding, rejection of
// malformed packets, and the link layer's behaviour around the edges
// (idle polls, stray line noise, an absurd length prefix).
//
// The happy path over the real ROM lives in test_rom_link.cpp.
//
//   test_gateway [path/to/nes_web_server.nes]
#include <vector>

#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"

// ---------- a minimal NES, enough to exercise the link ----------
static void nesSetOut0(bool v) {
  bool prev = (g_gpio_in >> PIN_NES_DATA_IN) & 1;
  if (v) g_gpio_in |= (1u << PIN_NES_DATA_IN);
  else   g_gpio_in &= ~(1u << PIN_NES_DATA_IN);
  if (prev && !v) onStrobe();
}

static bool nesReadD0() {
  bool sampled = (g_gpio_out >> PIN_NES_DATA_OUT) & 1;
  onClock();
  return sampled;
}

static bool nesPoll(uint8_t *idOut) {
  nesSetOut0(true);
  nesSetOut0(false);
  if (!nesReadD0()) return false;                 // ready flag
  uint8_t id = 0;
  for (int i = 0; i < 8; i++) id = (id >> 1) | (nesReadD0() ? 0x80 : 0);
  *idOut = id;
  return true;
}

static void nesSendByte(uint8_t b) {
  for (int i = 0; i < 8; i++) { nesSetOut0((b >> i) & 1); nesReadD0(); }
}

// ---------- harness ----------
static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

static std::vector<uint8_t> readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> v(n);
  if (fread(v.data(), 1, n, f) != (size_t)n) exit(2);
  fclose(f);
  return v;
}

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  std::vector<uint8_t> rom = readFile(romPath);

  std::vector<size_t> offsets;
  for (size_t i = 0; i + 4 <= rom.size(); i++)
    if (memcmp(&rom[i], "NHF2", 4) == 0) offsets.push_back(i);
  printf("packets in ROM: %zu\n\n", offsets.size());
  if (offsets.size() < 3) { printf("expected 3 packets\n"); return 1; }

  const char *names[] = {"index.html", "style.css", "404"};

  printf("decoding the real packets\n");
  for (size_t i = 0; i < 3; i++) {
    size_t off = offsets[i];
    uint16_t declared = rom[off - 2] | (rom[off - 1] << 8);
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
    uint16_t declared = rom[off - 2] | (rom[off - 1] << 8);
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

  printf("\nlink layer edges\n");
  resetLink();
  uint8_t id = 0xAA;
  check(!nesPoll(&id), "idle poll reads a zero ready flag");
  check(linkState == LINK_IDLE, "no state change from an unanswered poll");

  resetLink();
  for (int i = 0; i < 64; i++) { nesSetOut0(i & 1); nesReadD0(); }
  check(linkState == LINK_IDLE && !rxComplete, "line noise while idle is ignored");

  resetLink();
  pendingId = 1;
  requestPending = true;
  check(nesPoll(&id) && id == 1, "syncs on the next poll after noise");
  check(linkState == LINK_RECEIVING, "moves to receive once the id is taken");

  // an implausible length prefix must be refused rather than overrun packetBuf
  nesSendByte(0xFF);
  nesSendByte(0xFF);
  check(rxOverflow && !rxComplete && linkState == LINK_IDLE,
        "65535 byte length prefix is refused");

  printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return failures ? 1 : 0;
}
