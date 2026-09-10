// The Arduino Mega gateway on simulated hardware, cycle for cycle: the
// real compiled firmware - the .elf arduino-cli builds - on an ATmega2560
// model (avr_core.h), against the real assembled ROM on a 6502 whose every
// $4016 access lands at its true time (cpu6502.h).
//
// test_mega_link.cpp proves the gateway's logic with a clock that only
// counts instructions.  This proves its timing: that the compiled
// interrupt handlers, behind the Arduino core's dispatch and alongside its
// timer and serial interrupts, keep up with a 6502 reading the port every
// ~33 microseconds - and by how much.  It cannot see anything electrical:
// edges are ideal, and the NES holds CLK low for half a CPU cycle.
//
//   test_mega_avr <nes_web_server.nes> <NES_router_mega.ino.elf>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "avr_core.h"
#include "cpu6502.h"
#include "harness.h"

using namespace m2560;

// Time is in picoseconds.
static const uint64_t US = 1000000ULL;
static const uint64_t MS = 1000 * US;
static const uint64_t SEC = 1000 * MS;
static const uint64_t PAL_CYCLE = 601465;       // 2A07 clock: 26.601712MHz / 16
static const uint64_t NTSC_CYCLE = 558730;      // 2A03 clock: 21.477272MHz / 12
static const uint64_t NEVER = UINT64_MAX;

// Must match main.asm.
static const uint8_t COL_IDLE = 0x0C, COL_JUNK = 0x06, COL_SEND = 0x28;
static const uint16_t ZP_COLOUR = 0x08;

// Mega pins 2, 3 and 4 are PE4, PE5 and PG5.
static const int BIT_CLK = 4, BIT_OUT0 = 5, BIT_D0 = 5;

static Mega2560 mcu;
static std::map<std::string, uint32_t> symbols;
static Cpu nes;
static std::vector<uint8_t> romImage, prg;
static std::vector<size_t> offsets;

static uint64_t cycle = PAL_CYCLE;       // one 6502 clock cycle
static uint64_t nesPs = 0;               // when the next 6502 instruction starts
static bool plugged = true;              // the cable
static bool placingPortAccess = false;   // the timeline placed this $4016 access
static bool d0ForRead = false;           // what that read returns
static bool out0Latched = false;         // bit 0 of the last $4016 write
static bool out0 = false;                // what the NES drives onto OUT0
static uint64_t sends = 0;               // times the ROM showed COL_SEND
static uint16_t ppuAddr = 0;
static std::string serialOut;            // everything the gateway sent the host

static uint64_t now() { return mcu.ticks * Mega2560::TICK_PS; }
static double us(uint64_t ps) { return ps / (double)US; }

// How the gateway's handlers line up with the NES's port accesses, worst
// cases over a run.
//
// Clock handlers serve clock edges in order.  The interrupt flag is a
// single bit, so a handler that overruns can make two edges share one
// run; pairing runs with edges first in, first out keeps the lateness of
// every run after that visible, rather than crediting it to a newer edge.
// A handler's work is late if the NES has made its next port access by
// the time it happens.
struct Timing {
  uint64_t clkFalls = 0, lastClkFall = 0;
  uint64_t minClkGap = NEVER;
  std::deque<uint64_t> unserved;          // clock edges no handler has started on yet
  uint64_t handlerClkFall = 0;            // the edge the running clock handler serves
  uint64_t maxClkToHandler = 0;           // CLK falling edge to its handler starting

  bool d0Wire = false;
  uint64_t d0Changes = 0, lateD0 = 0;     // D0 changes by the clock handler, and ones after the next read
  uint64_t lastD0Change = 0, lastRead = 0;
  uint64_t maxClkToD0 = 0;                // CLK falling edge to the handler changing D0
  uint64_t minD0Setup = NEVER;            // D0 changing to the NES reading it

  uint64_t out0Samples = 0, lateOut0 = 0; // OUT0 reads by the clock handler, and ones after the next write
  uint64_t lastWrite = 0, lastOut0Sample = 0;
  uint64_t minOut0Setup = NEVER;          // the NES writing OUT0 to the handler reading it
  uint64_t minOut0Hold = NEVER;           // the handler reading OUT0 to the NES writing it again
};
static Timing timing;

// ---------------- the NES ----------------
uint8_t Cpu::read(uint16_t addr) {
  if (addr < 0x2000) return ram[addr & 0x7FF];
  if (addr >= 0x8000) return prg[addr - 0x8000];
  if ((addr & 0x2007) == 0x2002) return 0x80;   // vblank always set
  if (addr == 0x4016) {
    if (!placingPortAccess) { halted = true; fault = "$4016 read by an instruction the timeline does not place"; }
    return 0x40 | (d0ForRead ? 1 : 0);
  }
  return 0;
}

void Cpu::write(uint16_t addr, uint8_t val) {
  if (addr < 0x2000) { ram[addr & 0x7FF] = val; return; }
  if (addr == 0x4016) {
    if (!placingPortAccess) { halted = true; fault = "$4016 written by an instruction the timeline does not place"; }
    out0Latched = val & 1;
    return;
  }
  if (addr == 0x2006) { ppuAddr = ((ppuAddr << 8) | val) & 0x3FFF; return; }
  if (addr == 0x2007) {
    if (ppuAddr == 0x3F00 && val == COL_SEND) sends++;
    ppuAddr++;
  }
}

enum PortUse { PORT_NONE, PORT_READ, PORT_WRITE };

// Whether the next instruction touches $4016, decided before it runs, so
// its clock pulse and data can be placed inside it.
static PortUse nextPortUse() {
  if (nes.pc < 0x8000 || nes.pc > 0xFFFD) return PORT_NONE;
  const uint8_t *p = &nes.prg[nes.pc - 0x8000];
  if ((p[1] | (p[2] << 8)) != 0x4016) return PORT_NONE;
  switch (p[0]) {
    case 0xAD: case 0xAE: case 0xAC: case 0x2C: case 0x0D: case 0x2D:
    case 0x4D: case 0x6D: case 0xCD: case 0xED: case 0xEC: case 0xCC:
      return PORT_READ;              // absolute reads
    case 0x8D: case 0x8E: case 0x8C:
      return PORT_WRITE;             // absolute writes
    default:
      return PORT_NONE;
  }
}

static void runMcuUntil(uint64_t t) {
  while (now() < t && !mcu.fault) mcu.step();
}

// With the cable out, the Mega's pull-ups hold its inputs high and the
// NES reads a floating D0, which comes back as ones.
static void unplug() {
  plugged = false;
  mcu.driveInput(PORT_E, BIT_CLK, -1);
  mcu.driveInput(PORT_E, BIT_OUT0, -1);
}

static void plugIn() {
  plugged = true;
  timing.unserved.clear();
  mcu.driveInput(PORT_E, BIT_CLK, 1);
  mcu.driveInput(PORT_E, BIT_OUT0, out0);
}

// One 6502 instruction.  One that touches the port is a 4 cycle absolute
// read or write, with the bus access in its last cycle: a read pulls CLK
// low for half that cycle and samples D0 at its end, and a write changes
// OUT0 halfway through it.  The AVR runs up to each of those moments.
// Everything else cannot affect the gateway, so it just takes its time.
static void stepNes() {
  if (nes.halted) {
    nesPs += cycle;
    runMcuUntil(nesPs);
    return;
  }
  PortUse use = nextPortUse();
  if (use == PORT_NONE) {
    nes.step();
    nesPs += nes.lastCycles * cycle;
    return;
  }

  placingPortAccess = true;
  if (use == PORT_READ) {
    uint64_t fall = nesPs + 3 * cycle;
    runMcuUntil(fall);
    if (plugged) {
      if (timing.lastClkFall) timing.minClkGap = std::min(timing.minClkGap, fall - timing.lastClkFall);
      timing.lastClkFall = fall;
      timing.clkFalls++;
      timing.unserved.push_back(fall);
      if (timing.unserved.size() > 1024) timing.unserved.pop_front();
      mcu.driveInput(PORT_E, BIT_CLK, 0);
      runMcuUntil(fall + cycle / 2);
      mcu.driveInput(PORT_E, BIT_CLK, 1);
    }

    uint64_t sample = nesPs + 4 * cycle;
    runMcuUntil(sample);
    if (plugged) {
      d0ForRead = !mcu.pinLevel(PORT_G, BIT_D0);   // the console inverts D0
      if (timing.lastD0Change > timing.lastRead)
        timing.minD0Setup = std::min(timing.minD0Setup, sample - timing.lastD0Change);
      timing.lastRead = sample;
    } else {
      d0ForRead = true;
    }
    nes.step();
  } else {
    uint64_t latch = nesPs + 3 * cycle + cycle / 2;
    runMcuUntil(latch);
    nes.step();
    if (plugged) {
      if (timing.lastClkFall && timing.lastOut0Sample > timing.lastClkFall)
        timing.minOut0Hold = std::min(timing.minOut0Hold, latch - timing.lastOut0Sample);
      timing.lastWrite = latch;
    }
    if (out0Latched != out0) {
      out0 = out0Latched;
      if (plugged) mcu.driveInput(PORT_E, BIT_OUT0, out0);
    }
  }
  placingPortAccess = false;
  nesPs += nes.lastCycles * cycle;
}

// Both CPUs, for a stretch of simulated time.
static void run(uint64_t duration) {
  uint64_t end = now() + duration;
  while (nesPs < end && !mcu.fault) stepNes();
  runMcuUntil(end);
}

static bool runUntil(const std::function<bool()> &done, uint64_t budget) {
  uint64_t end = now() + budget;
  while (!done() && now() < end && !mcu.fault) run(MS);
  return done();
}

static void installHooks() {
  mcu.onSerialByte = [](uint8_t b) { serialOut += (char)b; };

  mcu.onInterrupt = [](int vector) {
    if (vector != VEC_INT4 || !plugged || timing.unserved.empty()) return;
    timing.handlerClkFall = timing.unserved.front();
    timing.unserved.pop_front();
    timing.maxClkToHandler = std::max(timing.maxClkToHandler, now() - timing.handlerClkFall);
  };

  // D0 changes made by the clock handler must land before the NES's next
  // read, which comes after the next clock edge.
  mcu.onPortWrite = [](uint16_t) {
    bool wire = mcu.pinLevel(PORT_G, BIT_D0);
    if (wire == timing.d0Wire) return;
    timing.d0Wire = wire;
    timing.lastD0Change = now();
    if (mcu.handler != VEC_INT4 || !plugged || !timing.handlerClkFall) return;
    timing.d0Changes++;
    if (timing.lastClkFall != timing.handlerClkFall) timing.lateD0++;
    timing.maxClkToD0 = std::max(timing.maxClkToD0, now() - timing.handlerClkFall);
  };

  // OUT0 reads made by the clock handler must land before the NES writes
  // the next bit.
  mcu.onPinRead = [](uint16_t addr) {
    if (addr != PINE || mcu.handler != VEC_INT4 || !plugged || !timing.handlerClkFall) return;
    timing.out0Samples++;
    timing.lastOut0Sample = now();
    if (timing.lastWrite > timing.handlerClkFall) timing.lateOut0++;
    else timing.minOut0Setup = std::min(timing.minOut0Setup, now() - timing.lastWrite);
  };
}

// The ROM's BitDelay routine is LDA #BIT_DELAY / JMP DelayA, with
// StrobeDelay's LDA #24 straight after it.  Returns the offset of the
// operand, or -1.
static long findBitDelay(const std::vector<uint8_t> &code) {
  for (size_t i = 0; i + 7 <= code.size(); i++)
    if (code[i] == 0xA9 && code[i + 2] == 0x4C && code[i + 5] == 0xA9 && code[i + 6] == 24)
      return (long)i + 1;
  return -1;
}

// Power both on, cable in.  bitDelay < 0 leaves the ROM as built.
static void boot(uint64_t cpuCycle, int bitDelay) {
  mcu.reset();
  nes = Cpu();
  nes.prg = prg;
  if (bitDelay >= 0) nes.prg[findBitDelay(prg)] = (uint8_t)bitDelay;
  nes.reset();
  cycle = cpuCycle;
  nesPs = 0;
  out0 = out0Latched = d0ForRead = false;
  sends = 0;
  ppuAddr = 0;
  serialOut.clear();
  timing = Timing();
  plugIn();
}

// ---------------- the firmware, from outside ----------------
static uint16_t symbolAddress(const char *name) {
  auto it = symbols.find(name);
  return it == symbols.end() ? 0 : it->second & 0xFFFF;
}

static uint32_t firmware32(const char *name) {
  uint16_t a = symbolAddress(name);
  return a ? mcu.mem[a] | (mcu.mem[a + 1] << 8) | (mcu.mem[a + 2] << 16) | ((uint32_t)mcu.mem[a + 3] << 24) : 0;
}

struct Response {
  bool complete = false;
  int status = 0;
  long contentLength = -1;
  std::string contentType, packet, body;
};

static std::string headerValue(const std::string &head, const char *name) {
  std::string key = std::string("\r\n") + name + ": ";
  size_t p = head.find(key);
  if (p == std::string::npos) return "";
  p += key.size();
  return head.substr(p, head.find("\r\n", p) - p);
}

static Response parseResponse(const std::string &out) {
  Response r;
  size_t start = out.find("HTTP/1.1 ");
  if (start == std::string::npos) return r;
  size_t headEnd = out.find("\r\n\r\n", start);
  if (headEnd == std::string::npos) return r;
  std::string head = out.substr(start, headEnd + 2 - start);
  r.status = atoi(head.c_str() + 9);
  r.contentType = headerValue(head, "Content-Type");
  r.packet = headerValue(head, "X-NES-Packet");
  std::string len = headerValue(head, "Content-Length");
  r.contentLength = len.empty() ? -1 : atol(len.c_str());
  r.body = out.substr(headEnd + 4);
  r.complete = r.contentLength >= 0 && (long)r.body.size() >= r.contentLength;
  return r;
}

static bool responseComplete() { return parseResponse(serialOut).complete; }

// A request line down the serial port, as serial_bridge.py sends it.
static Response ask(const std::string &line, uint64_t budget = 10 * SEC) {
  serialOut.clear();
  mcu.serialSend(line + "\n");
  runUntil(responseComplete, budget);
  return parseResponse(serialOut);
}

static bool bootOnline() {
  return runUntil([] { return serialOut.find("online") != std::string::npos; }, 2 * SEC);
}

static bool isPage(const Response &r, uint8_t page) {
  uint16_t len = storedLength(romImage, offsets[page]);
  return r.complete && r.body.size() == len && memcmp(r.body.data(), &romImage[offsets[page]], len) == 0;
}

// One page over serial: whole, and first time.
static void fetchPage(const char *line, int status, const char *mime, uint8_t page, const char *what) {
  uint32_t errors = firmware32("linkErrors");
  uint64_t t0 = now();
  Response r = ask(line);
  check(r.status == status && r.contentType == mime && r.packet == "nhf2" && isPage(r, page), what);
  uint32_t retries = firmware32("linkErrors") - errors;
  char label[96];
  snprintf(label, sizeof label, "    first time, with no retries (%u)", (unsigned)retries);
  check(retries == 0, label);
  printf("      %u byte packet, answered in %.0f ms\n",
         (unsigned)storedLength(romImage, offsets[page]), (now() - t0) / (double)MS);
}

static const char *usText(uint64_t ps) {
  static char buf[4][24];
  static int next = 0;
  char *b = buf[next++ % 4];
  if (ps == NEVER) snprintf(b, sizeof buf[0], "n/a");
  else snprintf(b, sizeof buf[0], "%.2f us", us(ps));
  return b;
}

// Clock edges the firmware never ran a handler for.
static long long lostClocks() {
  return (long long)timing.clkFalls - (long long)mcu.handlers[VEC_INT4].count;
}

static void reportTiming() {
  printf("      clock edges %llu, the closest %s apart, %lld without a handler run\n",
         (unsigned long long)timing.clkFalls, usText(timing.minClkGap), lostClocks());
  printf("      CLK edge to its handler starting: worst %s\n", usText(timing.maxClkToHandler));
  printf("      D0: %llu changes, worst %s after the edge, settled %s before the NES reads it, %llu late\n",
         (unsigned long long)timing.d0Changes, usText(timing.maxClkToD0), usText(timing.minD0Setup),
         (unsigned long long)timing.lateD0);
  printf("      OUT0: %llu reads, %s after the NES wrote it, %s before the next write, %llu late\n",
         (unsigned long long)timing.out0Samples, usText(timing.minOut0Setup), usText(timing.minOut0Hold),
         (unsigned long long)timing.lateOut0);

  struct { int vector; const char *name; } names[] = {
    {VEC_INT4, "INT4, CLK"}, {VEC_INT5, "INT5, OUT0"}, {VEC_TIMER0_OVF, "timer 0"},
    {VEC_USART0_RX, "serial receive"}, {VEC_USART0_UDRE, "serial send"},
  };
  for (auto &n : names) {
    const Mega2560::HandlerStats &h = mcu.handlers[n.vector];
    if (!h.count) continue;
    printf("      %-15s %8llu runs, average %5.2f us, longest %5.2f us\n", n.name,
           (unsigned long long)h.count, h.totalTicks / 16.0 / h.count, h.maxTicks / 16.0);
  }
  printf("      longest stretch with interrupts off outside a handler: %.2f us\n",
         mcu.longestInterruptsOffTicks / 16.0);
  printf("      deepest stack: %u bytes of RAM to spare\n",
         (unsigned)(mcu.lowestSp - symbolAddress("__bss_end")));
}

static void checkTiming() {
  check(timing.clkFalls > 0 && lostClocks() == 0, "every clock edge gets a handler run of its own");
  check(timing.d0Changes > 0 && timing.lateD0 == 0 && timing.minD0Setup >= 5 * US,
        "D0 is settled at least 5us before every NES read");
  check(timing.out0Samples > 0 && timing.lateOut0 == 0 &&
        timing.minOut0Setup >= 1 * US && timing.minOut0Hold >= 5 * US,
        "OUT0 is read at least 1us after it is written, 5us before the next");
  check(mcu.lowestSp - symbolAddress("__bss_end") >= 512, "the stack never comes within 512 bytes of the globals");
  check(!mcu.fault && !nes.halted && mcu.resets == 0 && mcu.serialOverruns == 0,
        "no faults, resets or serial overruns");
  if (mcu.fault) printf("      AVR: %s\n", mcu.fault);
  if (nes.halted) printf("      6502: %s\n", nes.fault);
  if (mcu.resets || mcu.serialOverruns)
    printf("      %d resets, %llu serial overruns\n", mcu.resets, (unsigned long long)mcu.serialOverruns);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("usage: test_mega_avr <nes_web_server.nes> <NES_router_mega.ino.elf>\n");
    return 2;
  }
  romImage = readFile(argv[1]);
  if (romImage.size() < 16 + 0x8000) { printf("ROM too small\n"); return 2; }
  prg.assign(romImage.begin() + 16, romImage.begin() + 16 + 0x8000);
  offsets = findPackets(romImage);
  if (offsets.size() < 3 || findBitDelay(prg) < 0) { printf("not the web server ROM\n"); return 2; }
  if (!loadElf(mcu, readFile(argv[2]), &symbols)) { printf("cannot load %s\n", argv[2]); return 2; }
  for (const char *name : {"pollCount", "linkErrors", "__bss_end"}) {
    if (!symbolAddress(name)) { printf("the firmware has no symbol %s\n", name); return 2; }
  }
  installHooks();

  printf("PAL console, the ROM as built (BIT_DELAY %u)\n", prg[findBitDelay(prg)]);
  boot(PAL_CYCLE, -1);
  check(bootOnline(), "the firmware boots and announces itself online");
  run(SEC);
  check(!nes.halted && !mcu.fault, "both CPUs run a simulated second without faulting");
  if (nes.halted) printf("      6502: %s\n", nes.fault);
  if (mcu.fault) printf("      AVR: %s\n", mcu.fault);
  check(firmware32("pollCount") > 300 && firmware32("linkErrors") == 0,
        "hundreds of polls recognised, no link errors");
  check(nes.ram[ZP_COLOUR] == COL_IDLE, "the ROM reads a clean idle gateway (cyan)");

  fetchPage("GET / HTTP/1.1", 200, "text/html", 0, "GET / serves index.html, byte for byte");
  fetchPage("GET /style.css HTTP/1.1", 200, "text/css", 1, "GET /style.css");
  fetchPage("GET /missing HTTP/1.1", 404, "text/html", 2, "GET /missing gets the 404 page");
  check(sends == 3, "the ROM streamed exactly three responses");

  Response link = ask("GET /_link HTTP/1.1");
  unsigned long out0Edges = 0, out0High = 0, clkEdges = 0, clkHigh = 0, polls = 0;
  const char *b = link.body.c_str();
  const char *o = strstr(b, "OUT0:"), *c = strstr(b, "CLK:"), *p = strstr(b, "polls recognised");
  bool parsed = link.complete && o && c && p &&
                sscanf(o, "OUT0: %lu falling edges, high %lu", &out0Edges, &out0High) == 2 &&
                sscanf(c, "CLK: %lu falling edges, high %lu", &clkEdges, &clkHigh) == 2 &&
                sscanf(p, "polls recognised %lu", &polls) == 1;
  printf("      /_link: OUT0 %lu edges, high %lu%%   CLK %lu edges, high %lu%%   %lu polls\n",
         out0Edges, out0High, clkEdges, clkHigh, polls);
  check(parsed && out0Edges > 250 && out0Edges < 700 && polls == out0Edges && clkEdges >= 8 * polls &&
        out0High >= 70 && out0High <= 95 && clkHigh >= 99,
        "GET /_link reports what the ROM really does");

  printf("\n  timing, PAL\n");
  reportTiming();
  checkTiming();

  printf("\nNTSC console: a faster CPU, so less time between clocks\n");
  boot(NTSC_CYCLE, -1);
  check(bootOnline(), "the firmware boots and announces itself online");
  run(SEC / 2);
  fetchPage("GET / HTTP/1.1", 200, "text/html", 0, "GET / serves index.html, byte for byte");
  fetchPage("GET /style.css HTTP/1.1", 200, "text/css", 1, "GET /style.css");
  printf("\n  timing, NTSC\n");
  reportTiming();
  checkTiming();

  printf("\nhot plug, on the compiled firmware\n");
  boot(PAL_CYCLE, -1);
  check(bootOnline(), "the firmware boots and announces itself online");
  run(SEC / 2);
  uint32_t errors = firmware32("linkErrors");
  serialOut.clear();
  mcu.serialSend("GET / HTTP/1.1\n");
  run(300 * MS);                    // partway through the response
  unplug();
  run(700 * MS);                    // out for less than the gateway's 1s of silence
  check(nes.ram[ZP_COLOUR] == COL_JUNK, "cable out: the ROM reads junk and shows red");
  plugIn();
  runUntil(responseComplete, 10 * SEC);
  Response r = parseResponse(serialOut);
  check(r.status == 200 && isPage(r, 0) && firmware32("linkErrors") > errors,
        "pulled mid-response and pushed back: retried, and intact");

  unplug();
  run(100 * MS);
  uint64_t t0 = now();
  r = ask("GET /style.css HTTP/1.1");
  check(r.complete && r.status == 502, "left out: the request gets 502 Bad Gateway");
  check(now() - t0 < 1500 * MS, "    after about a second, not the full deadline");
  plugIn();
  run(SEC / 2);
  r = ask("GET /style.css HTTP/1.1");
  check(r.status == 200 && isPage(r, 1), "plugged back in: the next request is served");
  check(!mcu.fault && !nes.halted && mcu.resets == 0, "no faults or resets");

  // Not pass or fail: how far the ROM could be sped up before this
  // firmware stops keeping up, which is the margin the real one has.
  printf("\nmargin: GET / on PAL with the ROM's BIT_DELAY lowered\n");
  for (int bitDelay = 3; bitDelay >= 1; bitDelay--) {
    boot(PAL_CYCLE, bitDelay);
    bool online = bootOnline();
    run(SEC / 2);
    errors = firmware32("linkErrors");
    r = ask("GET / HTTP/1.1", 6 * SEC);
    bool served = r.status == 200 && isPage(r, 0);
    uint32_t retries = firmware32("linkErrors") - errors;
    printf("    BIT_DELAY %d, clocks %s apart: %s", bitDelay, usText(timing.minClkGap),
           !online ? "did not boot\n" : !served ? "no good page\n" : retries ? "served after retries" : "served first time");
    if (online && served)
      printf(" (%u); OUT0 read %s before the next write, %llu reads late, %lld clocks lost\n",
             (unsigned)retries, usText(timing.minOut0Hold), (unsigned long long)timing.lateOut0, lostClocks());
  }

  return finish();
}
