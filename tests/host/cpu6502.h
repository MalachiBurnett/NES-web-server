// A small NMOS 6502: the instructions main.asm and the test ROMs use, and
// how many clock cycles each one takes, so a simulation can put every bus
// access at its real time.
//
// The includer defines Cpu::read and Cpu::write - that is where the rest
// of the NES lives - so one core serves both the host tests (nes_sim.h)
// and the cycle-accurate Mega simulation (test_mega_avr.cpp).
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

struct Cpu {
  uint8_t a = 0, x = 0, y = 0, sp = 0xFD;
  uint16_t pc = 0;
  bool n = false, z = false, c = false, i = false, d = false, v = false;
  uint8_t ram[0x800] = {0};
  std::vector<uint8_t> prg;          // 32K at $8000
  uint64_t cycles = 0;               // instructions executed
  uint64_t clockCycles = 0;          // clock cycles they took
  uint8_t lastCycles = 0;            // clock cycles the last instruction took
  bool halted = false;
  const char *fault = nullptr;

  uint8_t read(uint16_t addr);
  void write(uint16_t addr, uint8_t val);

  uint8_t fetch() { return read(pc++); }
  uint16_t fetch16() { uint8_t lo = fetch(); uint8_t hi = fetch(); return lo | (hi << 8); }
  void push(uint8_t v) { ram[0x100 + sp] = v; sp--; }
  uint8_t pop() { sp++; return ram[0x100 + sp]; }
  void setNZ(uint8_t v) { n = v & 0x80; z = (v == 0); }

  // A taken branch costs a cycle, and one more if it lands on another page.
  void branch(bool take) {
    int8_t off = (int8_t)fetch();
    if (!take) return;
    uint16_t from = pc;
    pc += off;
    extra += ((from ^ pc) & 0xFF00) ? 2 : 1;
  }
  // So does indexing across a page boundary.
  uint16_t indexed(uint16_t base, uint8_t index) {
    uint16_t addr = base + index;
    if ((base ^ addr) & 0xFF00) extra++;
    return addr;
  }
  void adc(uint8_t m) {
    uint16_t r = a + m + (c ? 1 : 0);
    c = r > 0xFF; v = (~(a ^ m) & (a ^ r) & 0x80) != 0;
    a = r & 0xFF; setNZ(a);
  }
  void step();
  void reset() { pc = read(0xFFFC) | (read(0xFFFD) << 8); }

  uint8_t extra = 0;                 // cycles the current instruction adds to its base
};

inline void Cpu::step() {
  uint16_t opAddr = pc;
  uint8_t op = fetch();
  uint8_t base = 2;                  // most implied and immediate instructions
  extra = 0;
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
    case 0xA5: a = read(fetch()); setNZ(a); base = 3; break;      // LDA zp
    case 0xAD: a = read(fetch16()); setNZ(a); base = 4; break;    // LDA abs
    case 0xBD: { uint16_t b = fetch16(); a = read(indexed(b, x)); setNZ(a); base = 4; } break;  // LDA abs,x
    case 0xB9: { uint16_t b = fetch16(); a = read(indexed(b, y)); setNZ(a); base = 4; } break;  // LDA abs,y
    case 0xB1: { uint8_t zp = fetch(); uint16_t b = read(zp) | (read((zp + 1) & 0xFF) << 8);
                 a = read(indexed(b, y)); setNZ(a); base = 5; } break;   // LDA (zp),y
    case 0x85: write(fetch(), a); base = 3; break;                // STA zp
    case 0x8D: write(fetch16(), a); base = 4; break;              // STA abs
    case 0x86: write(fetch(), x); base = 3; break;                // STX zp
    case 0x8E: write(fetch16(), x); base = 4; break;              // STX abs
    case 0x84: write(fetch(), y); base = 3; break;                // STY zp
    case 0x9A: sp = x; break;                                     // TXS
    case 0xAA: x = a; setNZ(x); break;                            // TAX
    case 0xA8: y = a; setNZ(y); break;                            // TAY
    case 0x8A: a = x; setNZ(a); break;                            // TXA
    case 0x98: a = y; setNZ(a); break;                            // TYA
    case 0xE8: x++; setNZ(x); break;                              // INX
    case 0xC8: y++; setNZ(y); break;                              // INY
    case 0xCA: x--; setNZ(x); break;                              // DEX
    case 0x88: y--; setNZ(y); break;                              // DEY
    case 0xE6: { uint8_t zp = fetch(); uint8_t r = read(zp) + 1; write(zp, r); setNZ(r); base = 5; } break; // INC zp
    case 0xC6: { uint8_t zp = fetch(); uint8_t r = read(zp) - 1; write(zp, r); setNZ(r); base = 5; } break; // DEC zp
    case 0x4A: c = a & 1; a >>= 1; setNZ(a); break;               // LSR A
    case 0x46: { uint8_t zp = fetch(); uint8_t r = read(zp); c = r & 1; r >>= 1;
                 write(zp, r); setNZ(r); base = 5; } break;       // LSR zp
    case 0x0A: c = a & 0x80; a <<= 1; setNZ(a); break;            // ASL A
    case 0x66: { uint8_t zp = fetch(); uint8_t r = read(zp); bool oc = c; c = r & 1;
                 r = (r >> 1) | (oc ? 0x80 : 0); write(zp, r); setNZ(r); base = 5; } break; // ROR zp
    case 0x6A: { bool oc = c; c = a & 1; a = (a >> 1) | (oc ? 0x80 : 0); setNZ(a); } break; // ROR A
    case 0x05: a |= read(fetch()); setNZ(a); base = 3; break;     // ORA zp
    case 0x49: a ^= fetch(); setNZ(a); break;                     // EOR #
    case 0x45: a ^= read(fetch()); setNZ(a); base = 3; break;     // EOR zp
    case 0x29: a &= fetch(); setNZ(a); break;                     // AND #
    case 0x09: a |= fetch(); setNZ(a); break;                     // ORA #
    case 0xC9: { uint8_t m = fetch(); c = a >= m; setNZ(a - m); } break;       // CMP #
    case 0xC5: { uint8_t m = read(fetch()); c = a >= m; setNZ(a - m); base = 3; } break; // CMP zp
    case 0xE0: { uint8_t m = fetch(); c = x >= m; setNZ(x - m); } break;       // CPX #
    case 0xC0: { uint8_t m = fetch(); c = y >= m; setNZ(y - m); } break;       // CPY #
    case 0xE9: { uint8_t m = fetch(); uint16_t r = a - m - (c ? 0 : 1);
                 c = !(r & 0x100); v = ((a ^ m) & (a ^ r) & 0x80) != 0;
                 a = r & 0xFF; setNZ(a); } break;                 // SBC #
    case 0x69: adc(fetch()); break;                               // ADC #
    case 0x65: adc(read(fetch())); base = 3; break;               // ADC zp
    case 0x2C: { uint8_t m = read(fetch16()); n = m & 0x80; v = m & 0x40;
                 z = ((a & m) == 0); base = 4; } break;           // BIT abs
    case 0x24: { uint8_t m = read(fetch()); n = m & 0x80; v = m & 0x40;
                 z = ((a & m) == 0); base = 3; } break;           // BIT zp
    case 0x10: branch(!n); break;                                 // BPL
    case 0x30: branch(n); break;                                  // BMI
    case 0xD0: branch(!z); break;                                 // BNE
    case 0xF0: branch(z); break;                                  // BEQ
    case 0x90: branch(!c); break;                                 // BCC
    case 0xB0: branch(c); break;                                  // BCS
    case 0x4C: pc = fetch16(); base = 3; break;                   // JMP abs
    case 0x20: { uint16_t t = fetch16(); uint16_t r = pc - 1;
                 push(r >> 8); push(r & 0xFF); pc = t; base = 6; } break;   // JSR
    case 0x60: { uint8_t lo = pop(); uint8_t hi = pop();
                 pc = (lo | (hi << 8)) + 1; base = 6; } break;    // RTS
    case 0x40: halted = true; fault = "RTI executed (spurious interrupt)"; break;
    default: {
      static char msg[64];
      snprintf(msg, sizeof msg, "unimplemented opcode $%02X at $%04X", op, opAddr);
      fault = msg;
      halted = true;
    }
  }
  lastCycles = base + extra;
  clockCycles += lastCycles;
  cycles++;
}
