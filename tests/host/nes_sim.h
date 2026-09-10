// The NES side of the end to end tests: the 6502 core (cpu6502.h) running
// the real assembled ROM, with $4016 wired through a model of the cable to
// a gateway's real interrupt handlers.  Shared by both gateways, so the
// ESP32 firmware and the Mega port run against the same ROM, cable and
// hot plug schedule.
//
// The cable can be pulled out and pushed back in (with contact bounce)
// at any point in a transfer.
//
// Include after a shim (arduino_shim.h or avr_shim.h), its sketch, and
// harness.h.
#pragma once

#include <vector>

#include "cpu6502.h"

// Must match main.asm.
static const uint8_t COL_IDLE = 0x0C, COL_JUNK = 0x06, COL_SEND = 0x28;
static const uint16_t ZP_ID = 0x00, ZP_COLOUR = 0x08, ZP_RESULT = 0x0B;

static uint32_t rng = 0xC0FFEE;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// --- the cable ---
// CONNECTED passes everything through.  UNPLUGGED leaves the gateway's
// inputs wherever its board holds them (SIM_INPUT_UNPLUGGED: pulled down
// by the ESP32's dividers, up by the Mega's pull-ups) and the NES
// reading a floating D0.  BOUNCING is the plug on its way in: each
// signal makes contact or not at random, and both inputs chatter.
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
  bool prev = simInputLevel(PIN_NES_DATA_IN);
  simSetInput(PIN_NES_DATA_IN, level);
  if (prev && !level) onStrobe();
}

static void gwSetClk(bool level) {
  bool prev = gwClk;
  gwClk = level;
  simSetInput(PIN_NES_CLOCK, level);
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
    bit = simGatewayOutput(PIN_NES_DATA_OUT) ^ 1;
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
  gwSetOut0(SIM_INPUT_UNPLUGGED);
  gwSetClk(SIM_INPUT_UNPLUGGED);
}

static void plugIn() {
  cable = BOUNCING;
  bounceLeft = 1000 + rnd() % 4000;  // ~2-8ms of chatter
}

static void tickCable() {
  if (cable != BOUNCING) return;
  if ((rnd() & 7) == 0) {
    if (rnd() & 1) gwSetOut0(!simInputLevel(PIN_NES_DATA_IN));
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

// ---------------- simulation ----------------
static Cpu cpu;

// One instruction, and the time it takes.  Instructions average about
// 2.5 cycles (0.559us each), so ~1.4us: coarse, but the gateway's
// thresholds sit an order of magnitude away from anything the ROM
// does, which is the point of them.  test_mega_avr.cpp is the one
// that places every bus access at its real time.
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

// ---------------- the ROM ----------------
static std::vector<uint8_t> rom;
static std::vector<size_t> offsets;

static uint16_t declaredLen(uint8_t page) { return storedLength(rom, offsets[page]); }

// Load the ROM into the CPU.  False if it is not a web server ROM.
static bool loadRom(const char *path) {
  rom = readFile(path);
  if (rom.size() < 16 + 0x8000) { printf("ROM too small\n"); return false; }
  cpu.prg.assign(rom.begin() + 16, rom.begin() + 16 + 0x8000);
  offsets = findPackets(rom);
  printf("ROM: %zu bytes, reset vector $%04X, %zu packets\n\n",
         rom.size(), cpu.read(0xFFFC) | (cpu.read(0xFFFD) << 8), offsets.size());
  if (offsets.size() < 3) { printf("expected 3 packets in the ROM\n"); return false; }
  return true;
}
