// End to end test: the real assembled ROM, executed on a small 6502
// core, talking to the real gateway firmware through its interrupt
// handlers. Nothing here models main.asm - it runs the ROM bytes, so
// a change to either side that breaks the protocol shows up here.
//
// The cable between them is modelled too, so it can be pulled out and
// pushed back in (with contact bounce) at any point in a transfer.
//
//   test_rom_link [path/to/nes_web_server.nes]
#include <vector>

#include "arduino_shim.h"
#include "../../src/firmware/NES_router/NES_router.ino"

// Must match main.asm.
static const uint8_t COL_IDLE = 0x0C, COL_JUNK = 0x06, COL_SEND = 0x28;
static const uint16_t ZP_ID = 0x00, ZP_COLOUR = 0x08, ZP_RESULT = 0x0B;

// ---------------- 6502 ----------------
struct Cpu {
  uint8_t a = 0, x = 0, y = 0, sp = 0xFD;
  uint16_t pc = 0;
  bool n = false, z = false, c = false, i = false, d = false, v = false;
  uint8_t ram[0x800] = {0};
  std::vector<uint8_t> prg;          // 32K at $8000
  uint64_t cycles = 0;               // instructions executed
  bool halted = false;
  const char *fault = nullptr;

  uint8_t read(uint16_t addr);
  void write(uint16_t addr, uint8_t val);

  uint8_t fetch() { return read(pc++); }
  uint16_t fetch16() { uint8_t lo = fetch(); uint8_t hi = fetch(); return lo | (hi << 8); }
  void push(uint8_t v) { ram[0x100 + sp] = v; sp--; }
  uint8_t pop() { sp++; return ram[0x100 + sp]; }
  void setNZ(uint8_t v) { n = v & 0x80; z = (v == 0); }
  void branch(bool take) { int8_t off = (int8_t)fetch(); if (take) pc += off; }
  void adc(uint8_t m) {
    uint16_t r = a + m + (c ? 1 : 0);
    c = r > 0xFF; v = (~(a ^ m) & (a ^ r) & 0x80) != 0;
    a = r & 0xFF; setNZ(a);
  }
  void step();
  void reset() { pc = read(0xFFFC) | (read(0xFFFD) << 8); }
};

static uint32_t rng = 0xC0FFEE;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// --- the cable ---
// CONNECTED passes everything through.  UNPLUGGED leaves the gateway's
// inputs pulled to ground by their dividers and the NES reading a
// floating D0.  BOUNCING is the plug on its way in: each signal makes
// contact or not at random, and both inputs chatter on their own.
enum Cable { CONNECTED, UNPLUGGED, BOUNCING };
static Cable cable = CONNECTED;
static bool floatingD0 = true;       // what the NES reads with nothing driving D0
static bool nesOut0 = false;         // what the NES is driving onto OUT0
static bool gwClk = true;            // CLK as the gateway sees it; idles high
static int bounceLeft = 0;

static uint64_t clockPulses = 0;
static uint64_t strobes = 0;         // OUT0 falling edges from the NES
static uint64_t sendsShown = 0;      // times the ROM showed COL_SEND

static void gwSetOut0(bool level) {
  bool prev = (g_gpio_in >> PIN_NES_DATA_IN) & 1;
  if (level) g_gpio_in |= (1u << PIN_NES_DATA_IN);
  else       g_gpio_in &= ~(1u << PIN_NES_DATA_IN);
  if (prev && !level) onStrobe();
}

static void gwSetClk(bool level) {
  bool prev = gwClk;
  gwClk = level;
  if (level) g_gpio_in |= (1u << PIN_NES_CLOCK);
  else       g_gpio_in &= ~(1u << PIN_NES_CLOCK);
  if (prev && !level) onClock();
}

static bool contact() {
  return cable == CONNECTED || (cable == BOUNCING && (rnd() & 1));
}

// A write to $4016 drives OUT0 (port pin 3).
static void portWriteOut0(bool level) {
  if (nesOut0 && !level) strobes++;
  nesOut0 = level;
  if (contact()) gwSetOut0(level);
}

// A read of $4016 samples D0 (pin 4) and pulses the clock line (pin 2).
static uint8_t portReadD0() {
  clockPulses++;
  uint8_t bit;
  if (contact()) {
    // The console inverts D0 on its way to the CPU, as it does for a
    // real pad: wire low reads as 1.  Modelling it is what gives this
    // test teeth - a gateway with the polarity backwards agrees
    // perfectly with a non-inverting harness and fails on the console.
    bit = ((g_gpio_out >> PIN_NES_DATA_OUT) & 1) ^ 1;
    gwSetClk(false);             // sampled during the pulse, edge seen after
    gwSetClk(true);
  } else {
    bit = cable == BOUNCING ? rnd() & 1 : floatingD0;
  }
  return 0x40 | bit;             // upper bits read as open bus on real hardware
}

static void unplug(bool floatsHigh) {
  cable = UNPLUGGED;
  floatingD0 = floatsHigh;
  gwSetOut0(false);
  gwSetClk(false);
}

static void plugIn() {
  cable = BOUNCING;
  bounceLeft = 1000 + rnd() % 4000;  // ~2-8ms of chatter
}

static void tickCable() {
  if (cable != BOUNCING) return;
  if ((rnd() & 7) == 0) {
    if (rnd() & 1) gwSetOut0(!((g_gpio_in >> PIN_NES_DATA_IN) & 1));
    else           gwSetClk(!gwClk);
  }
  if (--bounceLeft == 0) {
    cable = CONNECTED;
    gwSetClk(true);
    gwSetOut0(nesOut0);
  }
}

// --- scheduled cable events ---
static uint32_t unplugAt = 0;        // g_micros to pull the cable at, 0 = never
static uint32_t plugAt = 0;          // g_micros to push it back in at
static uint32_t unplugAtRxByte = 0;  // or pull it once this many response bytes are in
static uint32_t replugAfterUs = 0;   // ...and push it back this long after, 0 = leave out
static bool unplugFloatsHigh = true;

static void tickEvents() {
  if (unplugAtRxByte && linkState == LINK_RECEIVING && rxIndex >= unplugAtRxByte) {
    unplugAtRxByte = 0;
    unplug(unplugFloatsHigh);
    if (replugAfterUs) plugAt = g_micros + replugAfterUs;
  }
  if (unplugAt && g_micros >= unplugAt) { unplugAt = 0; unplug(unplugFloatsHigh); }
  if (plugAt && g_micros >= plugAt) { plugAt = 0; plugIn(); }
}

static uint16_t ppuAddr = 0;

uint8_t Cpu::read(uint16_t addr) {
  if (addr < 0x2000) return ram[addr & 0x7FF];
  if (addr >= 0x8000) return prg[addr - 0x8000];
  if ((addr & 0x2007) == 0x2002) return 0x80;   // vblank always set
  if (addr == 0x4016) return portReadD0();
  return 0;
}

void Cpu::write(uint16_t addr, uint8_t val) {
  if (addr < 0x2000) { ram[addr & 0x7FF] = val; return; }
  if (addr == 0x4016) { portWriteOut0(val & 1); return; }
  if (addr == 0x2006) { ppuAddr = ((ppuAddr << 8) | val) & 0x3FFF; return; }
  if (addr == 0x2007) {
    if (ppuAddr == 0x3F00 && val == COL_SEND) sendsShown++;
    ppuAddr++;
    return;
  }
  // the rest of the PPU and APU is not modelled
}

void Cpu::step() {
  uint16_t opAddr = pc;
  uint8_t op = fetch();
  switch (op) {
    case 0x78: i = true; break;                                   // SEI
    case 0xD8: d = false; break;                                  // CLD
    case 0x58: i = false; break;                                  // CLI
    case 0x38: c = true; break;                                   // SEC
    case 0x18: c = false; break;                                  // CLC
    case 0xEA: break;                                             // NOP
    case 0xA9: a = fetch(); setNZ(a); break;                      // LDA #
    case 0xA2: x = fetch(); setNZ(x); break;                      // LDX #
    case 0xA0: y = fetch(); setNZ(y); break;                      // LDY #
    case 0xA5: a = read(fetch()); setNZ(a); break;                // LDA zp
    case 0xAD: a = read(fetch16()); setNZ(a); break;              // LDA abs
    case 0xBD: { uint16_t b = fetch16(); a = read(b + x); setNZ(a); } break;  // LDA abs,x
    case 0xB9: { uint16_t b = fetch16(); a = read(b + y); setNZ(a); } break;  // LDA abs,y
    case 0xB1: { uint8_t zp = fetch(); uint16_t b = read(zp) | (read((zp + 1) & 0xFF) << 8);
                 a = read(b + y); setNZ(a); } break;              // LDA (zp),y
    case 0x85: write(fetch(), a); break;                          // STA zp
    case 0x8D: write(fetch16(), a); break;                        // STA abs
    case 0x86: write(fetch(), x); break;                          // STX zp
    case 0x8E: write(fetch16(), x); break;                        // STX abs
    case 0x84: write(fetch(), y); break;                          // STY zp
    case 0x9A: sp = x; break;                                     // TXS
    case 0xAA: x = a; setNZ(x); break;                            // TAX
    case 0xA8: y = a; setNZ(y); break;                            // TAY
    case 0x8A: a = x; setNZ(a); break;                            // TXA
    case 0x98: a = y; setNZ(a); break;                            // TYA
    case 0xE8: x++; setNZ(x); break;                              // INX
    case 0xC8: y++; setNZ(y); break;                              // INY
    case 0xCA: x--; setNZ(x); break;                              // DEX
    case 0x88: y--; setNZ(y); break;                              // DEY
    case 0xE6: { uint8_t zp = fetch(); uint8_t r = read(zp) + 1; write(zp, r); setNZ(r); } break; // INC zp
    case 0xC6: { uint8_t zp = fetch(); uint8_t r = read(zp) - 1; write(zp, r); setNZ(r); } break; // DEC zp
    case 0x4A: c = a & 1; a >>= 1; setNZ(a); break;               // LSR A
    case 0x46: { uint8_t zp = fetch(); uint8_t r = read(zp); c = r & 1; r >>= 1;
                 write(zp, r); setNZ(r); } break;                 // LSR zp
    case 0x0A: c = a & 0x80; a <<= 1; setNZ(a); break;            // ASL A
    case 0x66: { uint8_t zp = fetch(); uint8_t r = read(zp); bool oc = c; c = r & 1;
                 r = (r >> 1) | (oc ? 0x80 : 0); write(zp, r); setNZ(r); } break; // ROR zp
    case 0x6A: { bool oc = c; c = a & 1; a = (a >> 1) | (oc ? 0x80 : 0); setNZ(a); } break; // ROR A
    case 0x05: a |= read(fetch()); setNZ(a); break;               // ORA zp
    case 0x49: a ^= fetch(); setNZ(a); break;                     // EOR #
    case 0x45: a ^= read(fetch()); setNZ(a); break;               // EOR zp
    case 0x29: a &= fetch(); setNZ(a); break;                     // AND #
    case 0x09: a |= fetch(); setNZ(a); break;                     // ORA #
    case 0xC9: { uint8_t m = fetch(); c = a >= m; setNZ(a - m); } break;       // CMP #
    case 0xC5: { uint8_t m = read(fetch()); c = a >= m; setNZ(a - m); } break; // CMP zp
    case 0xE0: { uint8_t m = fetch(); c = x >= m; setNZ(x - m); } break;       // CPX #
    case 0xC0: { uint8_t m = fetch(); c = y >= m; setNZ(y - m); } break;       // CPY #
    case 0xE9: { uint8_t m = fetch(); uint16_t r = a - m - (c ? 0 : 1);
                 c = !(r & 0x100); v = ((a ^ m) & (a ^ r) & 0x80) != 0;
                 a = r & 0xFF; setNZ(a); } break;                 // SBC #
    case 0x69: adc(fetch()); break;                               // ADC #
    case 0x65: adc(read(fetch())); break;                         // ADC zp
    case 0x2C: { uint8_t m = read(fetch16()); n = m & 0x80; v = m & 0x40;
                 z = ((a & m) == 0); } break;                     // BIT abs
    case 0x24: { uint8_t m = read(fetch()); n = m & 0x80; v = m & 0x40;
                 z = ((a & m) == 0); } break;                     // BIT zp
    case 0x10: branch(!n); break;                                 // BPL
    case 0x30: branch(n); break;                                  // BMI
    case 0xD0: branch(!z); break;                                 // BNE
    case 0xF0: branch(z); break;                                  // BEQ
    case 0x90: branch(!c); break;                                 // BCC
    case 0xB0: branch(c); break;                                  // BCS
    case 0x4C: pc = fetch16(); break;                             // JMP abs
    case 0x20: { uint16_t t = fetch16(); uint16_t r = pc - 1;
                 push(r >> 8); push(r & 0xFF); pc = t; } break;   // JSR
    case 0x60: { uint8_t lo = pop(); uint8_t hi = pop();
                 pc = (lo | (hi << 8)) + 1; } break;              // RTS
    case 0x40: halted = true; fault = "RTI executed (spurious interrupt)"; break;
    default: {
      static char msg[64];
      snprintf(msg, sizeof msg, "unimplemented opcode $%02X at $%04X", op, opAddr);
      fault = msg;
      halted = true;
    }
  }
  cycles++;
}

// ---------------- simulation ----------------
static Cpu cpu;

// One instruction, and the time it takes.  Instructions average about
// 2.5 cycles (0.559us each), so ~1.4us: coarse, but the gateway's
// thresholds sit an order of magnitude away from anything the ROM
// does, which is the point of them.
static uint32_t subMicros = 0;
static void stepNes() {
  cpu.step();
  subMicros += 1400;
  g_micros += subMicros / 1000;
  subMicros %= 1000;
  tickCable();
  tickEvents();
}

// While the firmware waits in delayMicroseconds, the NES keeps running.
static void runNesFor(uint32_t us) {
  uint32_t target = g_micros + us;
  while (g_micros < target) {
    if (cpu.halted) { g_micros = target; return; }
    stepNes();
  }
}

// Run until the predicate holds, the CPU faults, or the budget runs out.
static bool runUntil(bool (*done)(), uint64_t budget) {
  uint64_t start = cpu.cycles;
  while (!done() && cpu.cycles - start < budget) {
    stepNes();
    if (cpu.halted) { printf("    CPU fault: %s\n", cpu.fault); return false; }
  }
  return done();
}

static bool rxDone() { return rxComplete; }
static bool never() { return false; }

// ---------------- test ----------------
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

static std::vector<uint8_t> rom;
static std::vector<size_t> offsets;

static uint16_t declaredLen(uint8_t page) {
  size_t off = offsets[page];
  return rom[off - 2] | (rom[off - 1] << 8);
}

// The gateway's cached copy of a page is exactly what the ROM holds.
static bool cacheMatchesRom(uint8_t page) {
  char *ref = nullptr;
  uint32_t refLen = 0;
  if (!decodePacket(&rom[offsets[page]], declaredLen(page), &ref, &refLen)) return false;
  bool same = pageCache[page] && pageLen[page] == refLen &&
              memcmp(pageCache[page], ref, refLen) == 0;
  free(ref);
  return same;
}

static void forget(uint8_t page) {
  free(pageCache[page]);
  pageCache[page] = nullptr;
  pageLen[page] = 0;
}

int main(int argc, char **argv) {
  const char *romPath = argc > 1 ? argv[1] : "build/nes_web_server.nes";
  rom = readFile(romPath);
  if (rom.size() < 16 + 0x8000) { printf("ROM too small\n"); return 2; }
  cpu.prg.assign(rom.begin() + 16, rom.begin() + 16 + 0x8000);

  for (size_t i = 0; i + 4 <= rom.size(); i++)
    if (memcmp(&rom[i], "NHF2", 4) == 0) offsets.push_back(i);

  printf("ROM: %zu bytes, reset vector $%04X, %zu packets\n\n",
         rom.size(), cpu.read(0xFFFC) | (cpu.read(0xFFFD) << 8), offsets.size());
  if (offsets.size() < 3) { printf("expected 3 packets in the ROM\n"); return 1; }

  g_gpio_in |= (1u << PIN_NES_CLOCK);
  resetLink();
  cpu.reset();

  printf("boot and idle polling\n");
  runUntil(never, 60000);
  check(!cpu.halted, "ROM boots and runs without hitting a bad opcode");
  check(pollCount > 5 && pollCount == strobes, "every NES strobe is recognised as a poll");
  check(linkState == LINK_IDLE && !rxComplete, "gateway stays idle while nothing is pending");
  check(sendsShown == 0 && cpu.ram[ZP_COLOUR] == COL_IDLE, "the ROM sits idle, sending nothing");
  printf("    %llu polls, %llu clock pulses in %llu instructions\n",
         (unsigned long long)pollCount, (unsigned long long)clockPulses,
         (unsigned long long)cpu.cycles);

  const char *names[] = {"index.html", "style.css", "404"};
  struct Case { uint8_t ask; uint8_t expect; const char *what; };
  Case cases[] = {{0, 0, "index.html"}, {1, 1, "style.css"},
                  {2, 2, "404"}, {9, 2, "unknown id falls back to 404"}};

  for (const Case &cs : cases) {
    printf("\nrequest id %u (%s)\n", cs.ask, cs.what);
    resetLink();
    uint32_t errors = linkErrors;
    uint64_t polls = pollCount;
    pendingId = cs.ask;
    requestPending = true;

    uint64_t t0 = cpu.cycles;
    uint64_t strobesBefore = strobes;
    bool ok = runUntil(rxDone, 40000000ULL);
    check(ok && linkErrors == errors, "response received first time, framing intact");
    if (!ok) continue;

    check(cpu.ram[ZP_ID] == cs.ask, "ROM latched the id we sent");
    uint16_t declared = declaredLen(cs.expect);
    check(rxExpected == declared, "length prefix matches the page in the ROM");
    check(memcmp(packetBuf, &rom[offsets[cs.expect]], declared) == 0, "packet bytes are identical");
    check(pollCount - polls <= 2 && strobes - strobesBefore > 300,
          "hundreds of data edges, none taken for a poll");

    char *out = nullptr;
    uint32_t outLen = 0;
    check(decodePacket(packetBuf, rxExpected, &out, &outLen), "gateway decodes the packet");
    if (out) {
      printf("    %s: %u byte packet -> %u bytes, %llu instructions\n",
             names[cs.expect], declared, outLen, (unsigned long long)(cpu.cycles - t0));
      printf("    starts: %.60s\n", out);
      free(out);
    }
    check(linkState == LINK_IDLE && !requestPending, "link returns to idle, request cleared");
  }

  printf("\nback to back requests without a reset\n");
  pendingId = 1;
  requestPending = true;
  rxComplete = false;
  bool ok = runUntil(rxDone, 40000000ULL);
  check(ok && rxExpected == declaredLen(1) &&
        memcmp(packetBuf, &rom[offsets[1]], declaredLen(1)) == 0,
        "second request works with no intervention");

  // From here on the firmware's own fetchPage() drives everything, and
  // the NES runs whenever it waits.
  g_delay_hook = runNesFor;
  g_serial_echo = false;

  printf("\nfetchPage, end to end\n");
  for (uint8_t page = 0; page < 3; page++) {
    forget(page);
    uint32_t errors = linkErrors;
    char label[96];
    snprintf(label, sizeof label, "%s fetched, cached copy identical to the ROM", names[page]);
    check(fetchPage(page) && cacheMatchesRom(page) && linkErrors == errors, label);
  }
  uint32_t t0 = g_micros;
  check(fetchPage(0) && g_micros == t0, "a cached page is served without touching the link");

  printf("\nhot plug\n");
  unplug(true);
  uint64_t sends = sendsShown;
  runNesFor(1000000);
  check(sendsShown == sends, "port floating high: the ROM refuses to answer junk");
  check(cpu.ram[ZP_COLOUR] == COL_JUNK, "    and shows the junk colour");

  plugIn();
  runNesFor(1000000);
  check(cpu.ram[ZP_COLOUR] == cpu.ram[ZP_RESULT] && linkState == LINK_IDLE,
        "plugged back in: the junk colour clears by itself");

  unplug(false);
  sends = sendsShown;
  runNesFor(200000);
  check(sendsShown == sends && cpu.ram[ZP_COLOUR] == cpu.ram[ZP_RESULT],
        "port floating low: reads as an idle gateway, nothing sent");

  forget(0);
  plugAt = g_micros + 300000;
  check(fetchPage(0) && cacheMatchesRom(0), "request waiting when the cable goes in");

  forget(0);
  uint32_t errors = linkErrors;
  unplugFloatsHigh = true;
  unplugAtRxByte = 1000;
  replugAfterUs = 250000;
  check(fetchPage(0) && cacheMatchesRom(0) && linkErrors > errors,
        "cable pulled mid-response and pushed back: retried, intact");

  forget(1);
  unplugAtRxByte = 400;
  replugAfterUs = 0;
  t0 = g_micros;
  ok = fetchPage(1);
  check(!ok && strstr(g_serial_last, "silent"), "cable pulled and left out: the fetch fails");
  check(g_micros - t0 < 2000000, "    promptly, not at the full deadline");
  check(!requestPending && linkState == LINK_IDLE &&
        ((g_gpio_out >> PIN_NES_DATA_OUT) & 1), "    leaving the line idle");
  plugIn();
  runNesFor(200000);

  // The gateway reboots while the NES is streaming, so the NES carries
  // on sending into a gateway that has no idea a transfer is under way.
  resetLink();
  pendingId = 0;
  requestPending = true;
  runUntil([] { return linkState == LINK_RECEIVING && rxIndex >= 300; }, 40000000ULL);
  linkState = LINK_IDLE;
  requestPending = false;
  lastClockUs = lastActivityUs = 0;
  forget(0);
  check(fetchPage(0) && cacheMatchesRom(0), "gateway restarts mid-response, next fetch is intact");

  printf("\nsoak: random unplug and replug during every fetch\n");
  const int rounds = 40;
  int good = 0;
  errors = linkErrors;
  for (int r = 0; r < rounds; r++) {
    uint8_t page = r % 3;
    forget(page);
    unplugFloatsHigh = rnd() & 1;
    unplugAt = g_micros + rnd() % 900000;
    plugAt = unplugAt + 1000 + rnd() % 700000;
    if (fetchPage(page) && cacheMatchesRom(page)) good++;
    else printf("    round %d failed: %s", r, g_serial_last);
    runNesFor(20000);             // let any scheduled plug-in happen
    if (plugAt || unplugAt || cable != CONNECTED) {
      unplugAt = 0;
      if (cable == UNPLUGGED) plugIn();
      plugAt = 0;
      runNesFor(20000);
    }
  }
  char label[96];
  snprintf(label, sizeof label, "%d/%d fetches came through intact (%lu retries along the way)",
           good, rounds, (unsigned long)(linkErrors - errors));
  check(good == rounds, label);
  check(!cpu.halted, "the ROM never crashed");

  printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return failures ? 1 : 0;
}
