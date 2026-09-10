// An ATmega2560, enough of one to run the Arduino Mega gateway's real
// compiled firmware cycle for cycle: every instruction avr-gcc emits for
// it, with its clock cycles, and the peripherals that firmware and the
// Arduino core use - ports E and G, external interrupts INT4 and INT5,
// timer 0 (millis and micros) and USART0.  Any other register just reads
// back what was written to it.
//
// Time is counted in clock cycles at 16MHz, 62.5ns each.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Data space addresses, as the firmware sees them (I/O registers are at
// their I/O address + 0x20).  From avr/iomxx0_1.h.
namespace m2560 {
enum : uint16_t {
  PINE = 0x2C, DDRE = 0x2D, PORTE = 0x2E,
  PING = 0x32, DDRG = 0x33, PORTG = 0x34,
  TIFR0 = 0x35, EIFR = 0x3C, EIMSK = 0x3D,
  TCCR0B = 0x45, TCNT0 = 0x46,
  RAMPZ = 0x5B, EIND = 0x5C, SPL = 0x5D, SPH = 0x5E, SREG = 0x5F,
  EICRB = 0x6A, TIMSK0 = 0x6E,
  UCSR0A = 0xC0, UCSR0B = 0xC1, UBRR0L = 0xC4, UBRR0H = 0xC5, UDR0 = 0xC6,
  RAMSTART = 0x200, RAMEND = 0x21FF,
};
enum { PORT_E = 0, PORT_G = 1 };
enum { VEC_INT4 = 5, VEC_INT5 = 6, VEC_TIMER0_OVF = 23,
       VEC_USART0_RX = 25, VEC_USART0_UDRE = 26, VEC_USART0_TX = 27 };
}  // namespace m2560

class Mega2560 {
 public:
  static constexpr uint64_t TICK_PS = 62500;   // one clock cycle at 16MHz, in picoseconds

  std::vector<uint8_t> flash = std::vector<uint8_t>(256 * 1024, 0xFF);
  uint8_t mem[m2560::RAMEND + 1];
  uint32_t pc = 0;                   // in words
  uint64_t ticks = 0;                // clock cycles since reset
  const char *fault = nullptr;       // set when the simulation cannot go on
  int handler = -1;                  // vector of the interrupt handler running, or -1
  int resets = 0;                    // jumps back to the reset vector after start-up

  // Instrumentation
  struct HandlerStats { uint64_t count = 0, totalTicks = 0, maxTicks = 0; };
  HandlerStats handlers[64];
  uint16_t lowestSp = m2560::RAMEND;
  uint64_t longestInterruptsOffTicks = 0;   // outside handlers, once first enabled
  uint64_t serialOverruns = 0;

  // The world outside the chip
  std::function<void(uint8_t)> onSerialByte;      // USART0 has sent a byte
  std::function<void(uint16_t)> onPortWrite;      // PORTx or DDRx was written
  std::function<void(uint16_t)> onPinRead;        // PINx was read
  std::function<void(int)> onInterrupt;           // a handler was entered

  Mega2560() { reset(); }

  void reset() {
    memset(mem, 0, sizeof mem);
    pc = 0;
    ticks = 0;
    fault = nullptr;
    handler = -1;
    resets = 0;
    holdInterrupts = false;
    setSp(m2560::RAMEND);
    for (auto &port : drive) for (auto &pin : port) pin = -1;
    intLevel[0] = intLevel[1] = false;
    t0Prescale = 0;
    txBusy = udrFull = txc = false;
    rxQueue.clear();
    rxFifo.clear();
    for (auto &h : handlers) h = HandlerStats();
    lowestSp = m2560::RAMEND;
    interruptsEverOn = false;
    offSince = NEVER;
    longestInterruptsOffTicks = 0;
    serialOverruns = 0;
  }

  // --- pins ---------------------------------------------------------
  // What a pin sits at: driven by the chip if it is an output, else by
  // whatever is outside (-1 for nothing), else the pull-up if PORTx is set.
  bool pinLevel(int port, int bit) const {
    uint16_t ddr = port ? m2560::DDRG : m2560::DDRE, out = port ? m2560::PORTG : m2560::PORTE;
    if ((mem[ddr] >> bit) & 1) return (mem[out] >> bit) & 1;
    if (drive[port][bit] >= 0) return drive[port][bit];
    return (mem[out] >> bit) & 1;
  }

  void driveInput(int port, int bit, int level) {
    drive[port][bit] = level;
    updateExternalInterrupts();
  }

  // --- the host on the other end of USART0 ---------------------------
  void serialSend(const std::string &bytes, double baud = 115200) {
    double frameTicks = 10 * 16000000.0 / baud;
    uint64_t t = rxQueue.empty() ? ticks : std::max(ticks, rxQueue.back().first);
    for (size_t i = 0; i < bytes.size(); i++)
      rxQueue.push_back({t + (uint64_t)((i + 1) * frameTicks), (uint8_t)bytes[i]});
  }

  // --- run ------------------------------------------------------------
  void step() {
    if (fault) return;
    if (!holdInterrupts) {
      int v = pendingVector();
      if (v >= 0) { enterInterrupt(v); return; }
    }
    holdInterrupts = false;
    uint64_t before = ticks;
    unsigned cycles = exec();
    advance(cycles);
    trackInterruptsOff(before);
  }

  uint16_t sp() const { return mem[m2560::SPL] | (mem[m2560::SPH] << 8); }

 private:
  static constexpr uint8_t C = 1, Z = 2, N = 4, V = 8, S = 16, H = 32, T = 64, I = 128;
  static constexpr uint64_t NEVER = ~0ULL;

  bool holdInterrupts = false;       // SEI and RETI let one instruction run first
  int8_t drive[2][8];
  bool intLevel[2];                  // INT4 and INT5 pin levels, for edge detection
  uint32_t t0Prescale = 0;
  bool txBusy = false, udrFull = false, txc = false;
  uint8_t udrTx = 0, txShift = 0, rxLast = 0;
  uint64_t txDoneAt = 0;
  std::deque<std::pair<uint64_t, uint8_t>> rxQueue;   // arrival tick, byte
  std::deque<uint8_t> rxFifo;                         // the USART's two byte receive buffer
  bool interruptsEverOn = false;
  uint64_t offSince = NEVER;

  bool flag(uint8_t f) const { return mem[m2560::SREG] & f; }
  void setFlag(uint8_t f, bool on) {
    if (on) mem[m2560::SREG] |= f; else mem[m2560::SREG] &= ~f;
  }
  void nzs(uint8_t r) {
    setFlag(N, r & 0x80);
    setFlag(Z, r == 0);
    setFlag(S, flag(N) != flag(V));
  }

  uint16_t word(uint32_t w) const {
    uint32_t a = w * 2;
    return a + 1 < flash.size() ? flash[a] | (flash[a + 1] << 8) : 0xFFFF;
  }
  static bool isTwoWord(uint16_t op) {
    return (op & 0xFE0C) == 0x940C ||     // JMP, CALL
           (op & 0xFC0F) == 0x9000;       // LDS, STS
  }
  uint16_t pair(int lo) const { return mem[lo] | (mem[lo + 1] << 8); }
  void setPair(int lo, uint16_t v) { mem[lo] = v & 0xFF; mem[lo + 1] = v >> 8; }
  void setSp(uint16_t v) { setPair(m2560::SPL, v); }

  void push(uint8_t v) {
    uint16_t s = sp();
    if (s < m2560::RAMSTART || s > m2560::RAMEND) { fault = "stack pointer left RAM"; return; }
    mem[s] = v;
    setSp(s - 1);
    lowestSp = std::min<uint16_t>(lowestSp, s - 1);
  }
  uint8_t pop() {
    uint16_t s = sp() + 1;
    setSp(s);
    return s <= m2560::RAMEND ? mem[s] : 0;
  }
  // The ATmega2560's program counter is 3 bytes.
  void push3(uint32_t ret) { push(ret & 0xFF); push((ret >> 8) & 0xFF); push((ret >> 16) & 0xFF); }
  uint32_t pop3() { uint32_t hi = pop(), mid = pop(), lo = pop(); return (hi << 16) | (mid << 8) | lo; }

  uint8_t add(uint8_t a, uint8_t b, bool carry) {
    uint8_t r = a + b + carry;
    int cy = (a & b) | (b & ~r) | (~r & a);
    setFlag(H, cy & 0x08);
    setFlag(C, cy & 0x80);
    setFlag(V, ((a & b & ~r) | (~a & ~b & r)) & 0x80);
    nzs(r);
    return r;
  }
  // keepZ: SBC, SBCI and CPC only ever clear Z, so multi-byte compares work.
  uint8_t sub(uint8_t a, uint8_t b, bool carry, bool keepZ) {
    uint8_t r = a - b - carry;
    int bw = (~a & b) | (b & r) | (r & ~a);
    setFlag(H, bw & 0x08);
    setFlag(C, bw & 0x80);
    setFlag(V, ((a & ~b & ~r) | (~a & b & r)) & 0x80);
    setFlag(N, r & 0x80);
    setFlag(Z, keepZ ? (r == 0 && flag(Z)) : r == 0);
    setFlag(S, flag(N) != flag(V));
    return r;
  }
  uint8_t logic(uint8_t r) { setFlag(V, false); nzs(r); return r; }
  void shiftFlags(uint8_t r) {           // after ASR, LSR, ROR set C
    setFlag(N, r & 0x80);
    setFlag(Z, r == 0);
    setFlag(V, flag(N) != flag(C));
    setFlag(S, flag(N) != flag(V));
  }
  static int32_t signExtend(uint32_t v, int bits) {
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
  }

  unsigned unknown(uint16_t op) {
    static char text[80];
    snprintf(text, sizeof text, "unimplemented instruction %04X at %06X", op, pc * 2);
    fault = text;
    return 1;
  }

  unsigned skipNext(uint32_t &next) {
    bool two = isTwoWord(word(pc + 1));
    next = pc + (two ? 3 : 2);
    return two ? 2 : 1;
  }

  // --- memory and I/O ---
  uint8_t pins(int port) const {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) v |= pinLevel(port, b) << b;
    return v;
  }
  // Flag and PINx registers act on the bits written as ones.
  static bool writesOnes(uint16_t a) {
    return a == m2560::TIFR0 || a == m2560::EIFR || a == m2560::PINE || a == m2560::PING;
  }

  uint8_t read(uint16_t a) {
    using namespace m2560;
    if (a > RAMEND) return 0;
    switch (a) {
      case PINE: if (onPinRead) onPinRead(a); return pins(PORT_E);
      case PING: if (onPinRead) onPinRead(a); return pins(PORT_G);
      case UCSR0A:
        return (rxFifo.empty() ? 0 : 0x80) | (txc ? 0x40 : 0) | (udrFull ? 0 : 0x20) | (mem[a] & 0x03);
      case UDR0:
        if (!rxFifo.empty()) { rxLast = rxFifo.front(); rxFifo.pop_front(); }
        return rxLast;
      default:
        return mem[a];
    }
  }

  void write(uint16_t a, uint8_t v) {
    using namespace m2560;
    if (a > RAMEND) return;
    switch (a) {
      case PINE: mem[PORTE] ^= v; portWritten(PORTE); return;
      case PING: mem[PORTG] ^= v; portWritten(PORTG); return;
      case TIFR0: case EIFR: mem[a] &= ~v; return;
      case UCSR0A:
        if (v & 0x40) txc = false;
        mem[a] = (mem[a] & ~0x03) | (v & 0x03);
        return;
      case UDR0: udrTx = v; udrFull = true; startTx(); return;
      default:
        mem[a] = v;
        if (a == PORTE || a == DDRE || a == PORTG || a == DDRG) portWritten(a);
        else if (a == EICRB) updateExternalInterrupts();
    }
  }

  void portWritten(uint16_t a) {
    updateExternalInterrupts();
    if (onPortWrite) onPortWrite(a);
  }

  void updateExternalInterrupts() {
    for (int n = 4; n <= 5; n++) {
      bool level = pinLevel(m2560::PORT_E, n);
      bool &prev = intLevel[n - 4];
      int mode = (mem[m2560::EICRB] >> (2 * (n - 4))) & 3;
      if ((mode == 1 && level != prev) || (mode == 2 && prev && !level) || (mode == 3 && !prev && level))
        mem[m2560::EIFR] |= 1 << n;
      prev = level;
    }
  }

  // --- peripherals ---
  uint64_t bitTicks() const {
    uint32_t ubrr = mem[m2560::UBRR0L] | (mem[m2560::UBRR0H] << 8);
    return ((mem[m2560::UCSR0A] & 0x02) ? 8 : 16) * (uint64_t)(ubrr + 1);
  }
  void startTx() {
    if (txBusy || !udrFull || !(mem[m2560::UCSR0B] & 0x08)) return;
    txShift = udrTx;
    udrFull = false;
    txBusy = true;
    txDoneAt = ticks + 10 * bitTicks();
  }

  void advance(unsigned n) {
    using namespace m2560;
    ticks += n;

    static const uint16_t prescale[8] = {0, 1, 8, 64, 256, 1024, 0, 0};
    if (uint16_t div = prescale[mem[TCCR0B] & 7]) {
      t0Prescale += n;
      while (t0Prescale >= div) {
        t0Prescale -= div;
        if (++mem[TCNT0] == 0) mem[TIFR0] |= 1;
      }
    }

    if (txBusy && ticks >= txDoneAt) {
      txBusy = false;
      if (onSerialByte) onSerialByte(txShift);
      if (udrFull) startTx(); else txc = true;
    }
    while (!rxQueue.empty() && rxQueue.front().first <= ticks) {
      if (mem[UCSR0B] & 0x10) {
        if (rxFifo.size() < 2) rxFifo.push_back(rxQueue.front().second);
        else serialOverruns++;
      }
      rxQueue.pop_front();
    }
  }

  // --- interrupts ---
  int pendingVector() const {
    using namespace m2560;
    if (!flag(I)) return -1;
    for (int n = 4; n <= 5; n++) {
      int mode = (mem[EICRB] >> (2 * (n - 4))) & 3;
      bool enabled = (mem[EIMSK] >> n) & 1;
      bool pending = mode ? (mem[EIFR] >> n) & 1 : !pinLevel(PORT_E, n);
      if (enabled && pending) return n + 1;          // INT4 is vector 5
    }
    if (mem[TIFR0] & mem[TIMSK0] & 1) return VEC_TIMER0_OVF;
    if (!rxFifo.empty() && (mem[UCSR0B] & 0x80)) return VEC_USART0_RX;
    if (!udrFull && (mem[UCSR0B] & 0x20)) return VEC_USART0_UDRE;
    if (txc && (mem[UCSR0B] & 0x40)) return VEC_USART0_TX;
    return -1;
  }

  void enterInterrupt(int v) {
    using namespace m2560;
    push3(pc);
    mem[SREG] &= ~I;
    if (v == VEC_INT4 || v == VEC_INT5) mem[EIFR] &= ~(1 << (v - 1));
    if (v == VEC_TIMER0_OVF) mem[TIFR0] &= ~1;
    if (v == VEC_USART0_TX) txc = false;
    if (offSince != NEVER) {                          // interrupts were on, so nothing to track
      offSince = NEVER;
    }
    handler = v;
    handlerStart = ticks;
    if (onInterrupt) onInterrupt(v);
    pc = v * 2;
    advance(5);
  }
  uint64_t handlerStart = 0;

  void leaveHandler() {
    if (handler < 0) return;
    HandlerStats &h = handlers[handler];
    uint64_t t = ticks + 5 - handlerStart;           // including the RETI
    h.count++;
    h.totalTicks += t;
    h.maxTicks = std::max(h.maxTicks, t);
    handler = -1;
  }

  void trackInterruptsOff(uint64_t before) {
    bool on = flag(I);
    if (on) interruptsEverOn = true;
    if (!interruptsEverOn || handler >= 0) { offSince = NEVER; return; }
    if (!on) {
      if (offSince == NEVER) offSince = before;
    } else if (offSince != NEVER) {
      longestInterruptsOffTicks = std::max(longestInterruptsOffTicks, ticks - offSince);
      offSince = NEVER;
    }
  }

  // --- instructions ---
  unsigned exec() {
    using namespace m2560;
    uint16_t op = word(pc);
    uint32_t next = pc + 1;
    unsigned cycles = 1;
    const int d = (op >> 4) & 0x1F;
    const int r = ((op >> 5) & 0x10) | (op & 0x0F);
    const int dh = 16 + ((op >> 4) & 0x0F);
    const uint8_t K = ((op >> 4) & 0xF0) | (op & 0x0F);

    switch (op >> 12) {
      case 0x0:
        if (op == 0x0000) break;                                       // NOP
        if ((op & 0xFF00) == 0x0100) {                                 // MOVW
          int dd = ((op >> 4) & 0x0F) * 2, rr = (op & 0x0F) * 2;
          mem[dd] = mem[rr];
          mem[dd + 1] = mem[rr + 1];
          break;
        }
        switch ((op >> 10) & 3) {
          case 1: sub(mem[d], mem[r], flag(C), true); break;           // CPC
          case 2: mem[d] = sub(mem[d], mem[r], flag(C), true); break;  // SBC
          case 3: mem[d] = add(mem[d], mem[r], false); break;          // ADD, LSL
          default: return unknown(op);                                  // MULS, MULSU, FMUL
        }
        break;
      case 0x1:
        switch ((op >> 10) & 3) {
          case 0: if (mem[d] == mem[r]) cycles += skipNext(next); break;   // CPSE
          case 1: sub(mem[d], mem[r], false, false); break;            // CP
          case 2: mem[d] = sub(mem[d], mem[r], false, false); break;   // SUB
          case 3: mem[d] = add(mem[d], mem[r], flag(C)); break;        // ADC, ROL
        }
        break;
      case 0x2:
        switch ((op >> 10) & 3) {
          case 0: mem[d] = logic(mem[d] & mem[r]); break;              // AND, TST
          case 1: mem[d] = logic(mem[d] ^ mem[r]); break;              // EOR, CLR
          case 2: mem[d] = logic(mem[d] | mem[r]); break;              // OR
          case 3: mem[d] = mem[r]; break;                              // MOV
        }
        break;
      case 0x3: sub(mem[dh], K, false, false); break;                  // CPI
      case 0x4: mem[dh] = sub(mem[dh], K, flag(C), true); break;       // SBCI
      case 0x5: mem[dh] = sub(mem[dh], K, false, false); break;        // SUBI
      case 0x6: mem[dh] = logic(mem[dh] | K); break;                   // ORI, SBR
      case 0x7: mem[dh] = logic(mem[dh] & K); break;                   // ANDI, CBR
      case 0x8: case 0xA: {                                            // LDD, STD (LD, ST for Y and Z)
        int q = ((op >> 8) & 0x20) | ((op >> 7) & 0x18) | (op & 0x07);
        uint16_t at = ((op & 0x08) ? pair(28) : pair(30)) + q;
        if (op & 0x0200) write(at, mem[d]); else mem[d] = read(at);
        cycles = 2;
        break;
      }
      case 0x9: cycles = exec9(op, next, d, r); break;
      case 0xB: {                                                      // IN, OUT
        uint16_t a = 0x20 + (((op >> 5) & 0x30) | (op & 0x0F));
        if (op & 0x0800) write(a, mem[d]); else mem[d] = read(a);
        break;
      }
      case 0xC:                                                        // RJMP
        next = pc + 1 + signExtend(op & 0x0FFF, 12);
        cycles = 2;
        break;
      case 0xD:                                                        // RCALL
        push3(pc + 1);
        next = pc + 1 + signExtend(op & 0x0FFF, 12);
        cycles = 4;
        break;
      case 0xE: mem[dh] = K; break;                                    // LDI, SER
      case 0xF: {
        int b = op & 7;
        if ((op & 0xF800) == 0xF000) {                                 // BRBS, BRBC
          bool set = (mem[SREG] >> b) & 1;
          if (set == !(op & 0x0400)) {
            next = pc + 1 + signExtend((op >> 3) & 0x7F, 7);
            cycles = 2;
          }
        } else if (op & 0x0008) {
          return unknown(op);
        } else {
          switch ((op >> 9) & 3) {
            case 0: mem[d] = (mem[d] & ~(1 << b)) | ((flag(T) ? 1 : 0) << b); break;   // BLD
            case 1: setFlag(T, (mem[d] >> b) & 1); break;                // BST
            case 2: if (!((mem[d] >> b) & 1)) cycles += skipNext(next); break;   // SBRC
            case 3: if ((mem[d] >> b) & 1) cycles += skipNext(next); break;      // SBRS
          }
        }
        break;
      }
    }
    if (fault) return cycles;
    if (next == 0 && ticks > 0) resets++;
    if (next * 2 >= flash.size()) { fault = "program counter left flash"; return cycles; }
    pc = next;
    return cycles;
  }

  unsigned exec9(uint16_t op, uint32_t &next, int d, int r) {
    using namespace m2560;
    if ((op & 0xFC00) == 0x9000) {                                     // 1001 00sd dddd xxxx
      bool store = op & 0x0200;
      int low = op & 0x0F;
      switch (low) {
        case 0x0: {                                                    // LDS, STS
          uint16_t k = word(pc + 1);
          next = pc + 2;
          if (store) write(k, mem[d]); else mem[d] = read(k);
          return 2;
        }
        case 0x1: case 0x2: case 0x9: case 0xA: case 0xC: case 0xD: case 0xE: {   // via X, Y, Z
          int reg = low >= 0xC ? 26 : low >= 0x9 ? 28 : 30;
          uint16_t p = pair(reg);
          int mode = low & 3;                                          // 0 plain, 1 post-increment, 2 pre-decrement
          if (mode == 2) p--;
          if (store) write(p, mem[d]); else mem[d] = read(p);
          if (mode == 1) p++;
          setPair(reg, p);
          return 2;
        }
        case 0x4: case 0x5: case 0x6: case 0x7: {                      // LPM, ELPM
          if (store) return unknown(op);
          bool extended = low & 0x02;
          uint32_t z = pair(30) | (extended ? (uint32_t)mem[RAMPZ] << 16 : 0);
          mem[d] = z < flash.size() ? flash[z] : 0xFF;
          if (low & 0x01) {
            z++;
            setPair(30, z & 0xFFFF);
            if (extended) mem[RAMPZ] = (z >> 16) & 0xFF;
          }
          return 3;
        }
        case 0xF:                                                      // PUSH, POP
          if (store) push(mem[d]); else mem[d] = pop();
          return 2;
        default:
          return unknown(op);
      }
    }

    if ((op & 0xFE00) == 0x9400) {                                     // 1001 010x
      int low = op & 0x0F;
      switch (low) {
        case 0x0:                                                      // COM
          mem[d] = ~mem[d];
          setFlag(C, true);
          setFlag(V, false);
          nzs(mem[d]);
          return 1;
        case 0x1: {                                                    // NEG
          uint8_t v = mem[d], res = 0 - v;
          setFlag(H, (res | v) & 0x08);
          setFlag(V, res == 0x80);
          setFlag(C, res != 0);
          nzs(res);
          mem[d] = res;
          return 1;
        }
        case 0x2: mem[d] = (mem[d] << 4) | (mem[d] >> 4); return 1;    // SWAP
        case 0x3:                                                      // INC
          mem[d]++;
          setFlag(V, mem[d] == 0x80);
          nzs(mem[d]);
          return 1;
        case 0x5: {                                                    // ASR
          uint8_t v = mem[d];
          setFlag(C, v & 1);
          v = (v >> 1) | (v & 0x80);
          shiftFlags(v);
          mem[d] = v;
          return 1;
        }
        case 0x6: {                                                    // LSR
          uint8_t v = mem[d];
          setFlag(C, v & 1);
          v >>= 1;
          shiftFlags(v);
          mem[d] = v;
          return 1;
        }
        case 0x7: {                                                    // ROR
          uint8_t v = mem[d];
          bool carry = flag(C);
          setFlag(C, v & 1);
          v = (v >> 1) | (carry ? 0x80 : 0);
          shiftFlags(v);
          mem[d] = v;
          return 1;
        }
        case 0xA:                                                      // DEC
          mem[d]--;
          setFlag(V, mem[d] == 0x7F);
          nzs(mem[d]);
          return 1;
        case 0x8:
          if (!(op & 0x0100)) {                                        // BSET, BCLR
            int s = (op >> 4) & 7;
            bool set = !(op & 0x0080);
            setFlag(1 << s, set);
            if (s == 7 && set) holdInterrupts = true;                  // SEI
            return 1;
          }
          switch (op) {
            case 0x9508: next = pop3(); return 5;                      // RET
            case 0x9518:                                               // RETI
              next = pop3();
              setFlag(I, true);
              holdInterrupts = true;
              leaveHandler();
              return 5;
            case 0x9588: case 0x95A8: return 1;                        // SLEEP, WDR
            default: return unknown(op);
          }
        case 0x9:
          switch (op) {
            case 0x9409: next = pair(30); return 2;                                         // IJMP
            case 0x9419: next = pair(30) | ((uint32_t)mem[EIND] << 16); return 2;           // EIJMP
            case 0x9509: push3(pc + 1); next = pair(30); return 4;                          // ICALL
            case 0x9519: push3(pc + 1); next = pair(30) | ((uint32_t)mem[EIND] << 16); return 4;  // EICALL
            default: return unknown(op);
          }
        case 0xC: case 0xD: case 0xE: case 0xF: {                      // JMP, CALL
          uint32_t k = ((uint32_t)((((op >> 4) & 0x1F) << 1) | (op & 1)) << 16) | word(pc + 1);
          if (low >= 0xE) { push3(pc + 2); next = k; return 5; }
          next = k;
          return 3;
        }
        default:
          return unknown(op);
      }
    }

    if ((op & 0xFE00) == 0x9600) {                                     // ADIW, SBIW
      int reg = 24 + 2 * ((op >> 4) & 3);
      uint8_t k = ((op >> 2) & 0x30) | (op & 0x0F);
      uint16_t w = pair(reg);
      bool subtract = op & 0x0100;
      uint16_t res = subtract ? w - k : w + k;
      bool r15 = res & 0x8000, w15 = w & 0x8000;
      setFlag(V, subtract ? (w15 && !r15) : (!w15 && r15));
      setFlag(C, subtract ? (r15 && !w15) : (!r15 && w15));
      setFlag(N, r15);
      setFlag(Z, res == 0);
      setFlag(S, flag(N) != flag(V));
      setPair(reg, res);
      return 2;
    }

    if ((op & 0xFC00) == 0x9800) {                                     // CBI, SBIC, SBI, SBIS
      uint16_t a = 0x20 + ((op >> 3) & 0x1F);
      uint8_t bit = 1 << (op & 7);
      switch ((op >> 8) & 3) {
        case 0: write(a, writesOnes(a) ? 0 : (read(a) & ~bit)); return 2;        // CBI
        case 1: return (read(a) & bit) ? 1 : 1 + skipNext(next);                  // SBIC
        case 2: write(a, writesOnes(a) ? bit : (read(a) | bit)); return 2;        // SBI
        case 3: return (read(a) & bit) ? 1 + skipNext(next) : 1;                  // SBIS
      }
    }

    if ((op & 0xFC00) == 0x9C00) {                                     // MUL
      uint16_t p = (uint16_t)mem[d] * mem[r];
      mem[0] = p & 0xFF;
      mem[1] = p >> 8;
      setFlag(C, p & 0x8000);
      setFlag(Z, p == 0);
      return 2;
    }
    return unknown(op);
  }
};

// Load an ELF the way a programmer would: every loadable segment with
// bytes in it at its physical address in flash (for .data, that is its
// load address after .text).  Segments above flash - SRAM, EEPROM, fuses -
// are not flash contents.  Symbols go into *symbols by name.
inline bool loadElf(Mega2560 &mcu, const std::vector<uint8_t> &elf,
                    std::map<std::string, uint32_t> *symbols) {
  auto u16 = [&](size_t o) { return (uint32_t)(elf[o] | (elf[o + 1] << 8)); };
  auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
  if (elf.size() < 52 || memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1 || elf[5] != 1)
    return false;

  uint32_t phoff = u32(0x1C), shoff = u32(0x20);
  uint32_t phentsize = u16(0x2A), phnum = u16(0x2C), shentsize = u16(0x2E), shnum = u16(0x30);

  bool loaded = false;
  for (uint32_t i = 0; i < phnum; i++) {
    size_t h = phoff + i * phentsize;
    if (h + 32 > elf.size() || u32(h) != 1) continue;          // PT_LOAD
    uint32_t offset = u32(h + 4), paddr = u32(h + 12), filesz = u32(h + 16);
    if (filesz == 0 || paddr >= 0x800000) continue;
    if (paddr + filesz > mcu.flash.size() || offset + filesz > elf.size()) return false;
    memcpy(&mcu.flash[paddr], &elf[offset], filesz);
    loaded = true;
  }
  if (!loaded) return false;

  for (uint32_t i = 0; symbols && i < shnum; i++) {
    size_t s = shoff + i * shentsize;
    if (s + 40 > elf.size() || u32(s + 4) != 2) continue;      // SHT_SYMTAB
    uint32_t symOff = u32(s + 16), symSize = u32(s + 20), link = u32(s + 24);
    size_t strHeader = shoff + link * shentsize;
    uint32_t strOff = u32(strHeader + 16);
    for (uint32_t e = 0; e + 16 <= symSize; e += 16) {
      uint32_t name = u32(symOff + e);
      if (!name) continue;
      (*symbols)[std::string((const char *)&elf[strOff + name])] = u32(symOff + e + 4);
    }
  }
  return true;
}
