// Unit tests for the gateway firmware: packet decoding, rejection of
// malformed packets, and the link layer's handling of everything a
// hot plug can throw at it - half frames, half responses, contact
// bounce, and a port with nothing on it.
//
// The real ROM, and hot plugging against it, is in test_rom_link.cpp.
//
//   test_gateway [path/to/nes_web_server.nes]
#include <vector>

#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"

// ---------- a minimal NES, enough to exercise the link ----------
// Timing is how the gateway tells a poll from a data bit, so this keeps
// the rough shape of main.asm's: tens of microseconds per bit, and a
// ~2ms quiet hold before every poll.
static const uint32_t BIT_US = 40, HOLD_US = 2500, STROBE_US = 100;

static void nesSetOut0(bool v) {
  bool prev = (g_gpio_in >> PIN_NES_DATA_IN) & 1;
  if (v) g_gpio_in |= (1u << PIN_NES_DATA_IN);
  else   g_gpio_in &= ~(1u << PIN_NES_DATA_IN);
  if (prev && !v) onStrobe();
}

static bool nesReadD0(uint32_t after = BIT_US) {
  g_micros += after;
  bool sampled = !((g_gpio_out >> PIN_NES_DATA_OUT) & 1);   // console inverts D0
  onClock();
  return sampled;
}

static bool wireHigh() { return (g_gpio_out >> PIN_NES_DATA_OUT) & 1; }

enum Poll { POLL_IDLE, POLL_REQUEST, POLL_JUNK };

// main.asm's PollRequest
static Poll nesPoll(uint8_t *idOut) {
  nesSetOut0(true);
  g_micros += HOLD_US;
  nesSetOut0(false);
  g_micros += STROBE_US;
  for (int i = 0; i < 8; i++)
    if (nesReadD0()) return POLL_JUNK;
  if (!nesReadD0()) return POLL_IDLE;
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) v = (v >> 1) | (nesReadD0() ? 0x8000 : 0);
  if (((v ^ (v >> 8)) & 0xFF) != 0xFF) return POLL_JUNK;
  *idOut = v & 0xFF;
  return POLL_REQUEST;
}

// Data onto OUT0, then the clock a couple of microseconds later.
static void nesSendRaw(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    g_micros += BIT_US - 2;
    nesSetOut0((b >> i) & 1);
    nesReadD0(2);
  }
}

// main.asm's SendResponse, with the knobs to get it wrong: flip a bit
// of one packet byte in transit, swap two neighbouring bytes (which a
// plain sum cannot see), or stop after some number of bytes.
static void nesRespond(uint8_t echo, const uint8_t *pkt, uint16_t len,
                       long corruptAt = -1, uint32_t stopAfter = UINT32_MAX,
                       long swapAt = -1) {
  uint8_t s1 = 0, s2 = 0;
  uint32_t sent = 0;
  auto send = [&](uint8_t b, uint8_t onWire) {
    s1 += b; s2 += s1;
    if (sent++ < stopAfter) nesSendRaw(onWire);
  };
  send(echo, echo);
  send(len & 0xFF, len & 0xFF);
  send(len >> 8, len >> 8);
  for (uint32_t i = 0; i < len && sent < stopAfter; i++) {
    uint8_t onWire = pkt[i];
    if ((long)i == corruptAt) onWire ^= 0x10;
    if (swapAt >= 0 && (long)i == swapAt) onWire = pkt[i + 1];
    if (swapAt >= 0 && (long)i == swapAt + 1) onWire = pkt[i - 1];
    send(pkt[i], onWire);
  }
  uint8_t sum2 = s2;
  if (sent++ < stopAfter) nesSendRaw(s1);
  if (sent++ < stopAfter) nesSendRaw(sum2);
}

static uint32_t rng = 0x1234567;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// Contacts chattering as a plug goes in: both inputs flicker at random.
static void bounce(int edges) {
  for (int i = 0; i < edges; i++) {
    g_micros += 1 + rnd() % 300;
    if (rnd() & 1) {
      nesSetOut0(!((g_gpio_in >> PIN_NES_DATA_IN) & 1));
    } else {
      onClock();                    // a falling CLK edge
    }
  }
  nesSetOut0(false);                // settles where the NES left it
}

static void request(uint8_t id) {
  resetLink();
  pendingId = id;
  requestPending = true;
}

// ---------- harness ----------
static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
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

// A flash cart menu: strobes briefly once a frame and reads 8 buttons.
// Between its frames nothing reads the port, but a 1 left on the line
// would still be wrong: the gateway should let go once reads stop.
static uint32_t menuNext = 0;
static int menuButtons = 0, menuLineHeld = 0;
static void menuHook(uint32_t us) {
  g_micros += us;
  if (g_micros < menuNext) return;
  if (!wireHigh()) menuLineHeld++;
  menuNext = g_micros + 16667;
  nesSetOut0(true);
  g_micros += 12;
  nesSetOut0(false);
  for (int i = 0; i < 8; i++)
    if (nesReadD0(12)) menuButtons++;
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

  // Arbitrary bytes: the link layer does not care what is inside.
  uint8_t pkt[300];
  for (int i = 0; i < 300; i++) pkt[i] = (uint8_t)(i * 7 + 3);
  uint8_t id = 0xAA;

  printf("\nlink layer: framing\n");
  resetLink();
  check(nesPoll(&id) == POLL_IDLE, "idle poll reads all zeros");
  check(linkState == LINK_IDLE && wireHigh(), "gateway stays idle with the wire held high");

  request(1);
  check(nesPoll(&id) == POLL_REQUEST && id == 1, "request frame carries the id and its complement");
  check(linkState == LINK_RECEIVING, "waits for the response once the frame is read");
  nesRespond(1, pkt, 300);
  check(rxComplete && rxExpected == 300 && memcmp(packetBuf, pkt, 300) == 0,
        "response arrives intact");
  check(!requestPending && linkState == LINK_IDLE && wireHigh(), "request cleared, back to idle");
  check(nesPoll(&id) == POLL_IDLE, "the poll after that is idle again");

  printf("\nlink layer: bad responses are refused, then retried\n");
  struct Bad {
    const char *what; uint8_t echo; uint16_t len; long corrupt; uint32_t stop; long swap; uint8_t expect;
  };
  const Bad bads[] = {
    {"response for a different page is refused", 2, 300, -1, UINT32_MAX, -1, LE_ECHO},
    {"a flipped bit is caught by the checksum", 1, 300, 123, UINT32_MAX, -1, LE_CHECKSUM},
    {"two swapped bytes are caught too (sum2)", 1, 300, -1, UINT32_MAX, 200, LE_CHECKSUM},
    {"65535 byte length prefix is refused", 1, 0xFFFF, -1, 3, -1, LE_LENGTH},
  };
  for (const Bad &b : bads) {
    request(1);
    nesPoll(&id);
    uint32_t polls = pollCount;
    nesRespond(b.echo, pkt, b.len, b.corrupt, b.stop, b.swap);
    check(linkError == b.expect && !rxComplete, b.what);
    check(pollCount == polls && linkState == LINK_IDLE,
          "    the rest of it is not mistaken for polls");
    check(requestPending && nesPoll(&id) == POLL_REQUEST && id == 1,
          "    the request goes out again on the next poll");
    nesRespond(1, pkt, 300);
    check(rxComplete && memcmp(packetBuf, pkt, 300) == 0, "    and the retry succeeds");
  }

  printf("\nlink layer: staying in step\n");
  resetLink();
  for (int i = 0; i < 64; i++) { nesSetOut0(i & 1); nesReadD0(); }
  check(linkState == LINK_IDLE && !rxComplete && wireHigh(), "line noise while idle is ignored");

  request(1);
  nesPoll(&id);                     // the NES reads the frame, then ignores it
  check(nesPoll(&id) == POLL_REQUEST && linkError == LE_REJECTED,
        "a frame the NES ignored is noticed and sent again");
  nesRespond(1, pkt, 300);
  check(rxComplete, "    and then served");

  request(1);
  nesPoll(&id);
  nesRespond(1, pkt, 300, -1, 50);  // the cable comes out 50 bytes in
  g_micros += 5000;
  check(nesPoll(&id) == POLL_REQUEST && linkError == LE_CUT,
        "a response that stops partway is dropped at the next poll");
  nesRespond(1, pkt, 300);
  check(rxComplete, "    and the retry succeeds");

  // Plugged in after the NES strobed: the gateway missed the edge and
  // sees only the clocks of a frame it never armed.
  request(1);
  g_micros += HOLD_US;
  g_gpio_in &= ~(1u << PIN_NES_DATA_IN);
  uint8_t zeros = 0;
  for (int i = 0; i < 25; i++) zeros += !nesReadD0();
  check(zeros == 25 && linkState == LINK_IDLE, "joining partway through a poll hands the NES only zeros");
  check(nesPoll(&id) == POLL_REQUEST && id == 1, "    and the next poll carries the request");
  nesRespond(1, pkt, 300);
  check(rxComplete, "    which is served");

  // Plugged in, or rebooted, while the NES streams a response blind.
  // With no clock seen for ages the first data edge looks like a poll,
  // so the gateway briefly takes data for a frame - it must not keep it.
  request(1);
  lastClockUs = lastActivityUs = g_micros - 10000000;
  g_gpio_in |= (1u << PIN_NES_DATA_IN);   // OUT0 high, so the first bit is an edge
  uint32_t pollsBefore = pollCount;
  nesRespond(0, pkt, 300);
  check(pollCount == pollsBefore + 1 && requestPending,
        "    only the first stray edge was taken for a poll");
  check(!rxComplete, "joining partway through a response does not accept it");
  check(nesPoll(&id) == POLL_REQUEST && id == 1, "    the next poll carries the request");
  nesRespond(1, pkt, 300);
  check(rxComplete && memcmp(packetBuf, pkt, 300) == 0, "    which is served intact");

  request(1);
  g_micros += 10000000;
  bounce(400);
  check(nesPoll(&id) == POLL_REQUEST && id == 1, "contact bounce on plug-in is shrugged off");
  nesRespond(1, pkt, 300);
  check(rxComplete && memcmp(packetBuf, pkt, 300) == 0, "    and the page comes through");

  resetLink();
  g_micros += 10000000;
  bounce(400);
  check(linkState == LINK_IDLE && wireHigh(), "bounce with nothing pending leaves the wire idle");

  printf("\nfetchPage without the ROM\n");
  g_serial_echo = false;
  resetLink();
  g_micros += 10000000;
  uint32_t t0 = g_micros;
  bool ok = fetchPage(0);
  check(!ok && strstr(g_serial_last, "silent"), "fails when nothing is polling");
  check(g_micros - t0 < 1500000, "    within ~1s, not the full deadline");
  check(!requestPending && linkState == LINK_IDLE && wireHigh(), "    and leaves the line idle");

  // Plugged in at the flash cart menu while the bridge is asking for a
  // page: the menu strobes and reads 8 bits a frame.
  resetLink();
  menuNext = g_micros;
  g_delay_hook = menuHook;
  t0 = g_micros;
  ok = fetchPage(1);
  g_delay_hook = nullptr;
  check(menuButtons == 0, "a flash cart menu never sees a button press");
  check(menuLineHeld == 0, "    and nothing is left on the line between its frames");
  check(!ok && strstr(g_serial_last, "timeout") && g_micros - t0 >= FETCH_DEADLINE_US,
        "    and the fetch times out instead of hanging");
  check(!requestPending && wireHigh(), "    leaving the line idle");
  g_serial_echo = true;

  printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return failures ? 1 : 0;
}
