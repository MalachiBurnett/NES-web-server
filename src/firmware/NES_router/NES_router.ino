// ===================================================================
//  NES WEB SERVER - gateway
//
//  Talks to the cartridge over controller port 1 and serves its pages
//  to the host over serial.  One sketch for every board: the board is
//  whichever you compile for, and the wiring is chosen below.
//
//    ESP32-C3 SuperMini   --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc
//    Arduino Mega 2560    --fqbn arduino:avr:mega:cpu=atmega2560
//
//  docs/protocol.md has the wire format, docs/mega.md the Mega.  The
//  controller port, whichever board is on the other end:
//
//    CLK   (port pin 2) input   one falling edge per $4016 read
//    OUT0  (port pin 3) input   NES data out + poll strobe
//    D0    (port pin 4) output  our data into the NES
//    GND   (port pin 1)
//
//  The clock pulse is only ~0.5us wide - about one 6502 cycle - and
//  cannot be widened in software, so CLK is always on an interrupt.
//  Polling its level misses edges.  OUT0 is on one too where the
//  wiring allows; where it does not, polls are recognised at their
//  first clock instead (see onClock).
//
//  D0 is INVERTED by the console, as for a real pad: wire low reads as
//  1.  So an idle gateway holds the wire high.  See d0Write().
//
//  The cable can go in or come out at any time.  Every poll from the
//  NES is a resync point, a request stays pending until a response
//  checks out, and a line that goes quiet mid-transfer is dropped.
// ===================================================================

// --- Build options --------------------------------------------------
// The Arduino Mega has two wirings.  The default puts CLK and OUT0 on
// the external interrupt pins 2 and 3.  NES_WIRING_RJ45 matches an RJ45
// jack plugged straight into the header at pins 8-13.  Choose it here,
// or leave this file alone and pass it to arduino-cli:
//
//   --build-property "compiler.cpp.extra_flags=-DNES_WIRING_RJ45"
//
// #define NES_WIRING_RJ45

#include <string.h>

// Forward declarations: the Arduino builder auto-generates function
// prototypes at the top of the translation unit, before the
// definitions below - without these, the generated prototypes for
// parseTree(BitReader&, ...) and measureLink(..., LinkProbe*) fail to
// compile.
struct BitReader;
struct LinkProbe;

// --- Boards and wiring ----------------------------------------------
//   OUT0_INTERRUPT  OUT0 has an interrupt of its own
//   CLOCK_PCINT     CLK is on a pin change interrupt, not attachInterrupt
//   DECODES         pages are decompressed and cached here, rather than
//                   sent raw for scripts/serial_bridge.py to decompress
#if defined(ARDUINO_ARCH_ESP32)
  // Safe GPIOs on a SuperMini: 2/8/9 are strapping pins, 18/19 are the
  // native USB pair, 20/21 are UART0.
  //
  // OUT0 is on 3, not 5.  An unconnected input next to a switching
  // output couples to it hard enough to look like a shorted wire, so
  // if this ever reads as a copy of D0, suspect the pin number before
  // you suspect the solder.
  //
  // The C3 is NOT 5V tolerant: CLK and OUT0 come in through 10k/20k
  // dividers, and D0 drives the NES at 3.3V through 1k.  The dividers
  // also hold the inputs low with the cable out.
  #include "soc/gpio_reg.h"
  #define BOARD_NAME        "ESP32-C3"
  #define PIN_WORD          "GPIO"
  #define PIN_NES_CLOCK     4
  #define PIN_NES_DATA_IN   3
  #define PIN_NES_DATA_OUT  6
  #define NES_INPUT_MODE    INPUT
  #define OUT0_INTERRUPT    1
  #define CLOCK_PCINT       0
  #define DECODES           1
  #define MAX_PACKET        8192
#elif defined(__AVR_ATmega2560__)
  // A 5V part, so nothing needs level shifting.  The inputs use the
  // internal pull-ups, so a pulled cable leaves them high and quiet.
  #define BOARD_NAME        "Arduino Mega"
  #define PIN_WORD          "pin"
  #define NES_INPUT_MODE    INPUT_PULLUP
  #define DECODES           0            // 8KB of RAM cannot hold the tables
  #define MAX_PACKET        4096         // half the RAM
  #define IRAM_ATTR
  #if defined(NES_WIRING_RJ45)
    // Pin 12 is PB6, which has pin change interrupt PCINT6 though not
    // an external interrupt, so attachInterrupt() cannot use it.  Pin
    // 9 has neither.  A jack on the header has no 1k resistors in
    // series, so mind the power order: see docs/mega.md.
    #define PIN_NES_CLOCK     12         // PB6, PCINT6
    #define PIN_NES_DATA_IN   9          // PH6, no interrupt
    #define PIN_NES_DATA_OUT  13         // PB7, also the L LED
    #define OUT0_INTERRUPT    0
    #define CLOCK_PCINT       1
    #define CLOCK_PCMSK_BIT   PCINT6
  #else
    // Only pins 2, 3 and 18-21 have external interrupts, and 18-21 are
    // Serial1 and I2C.  2 is INT4 and 3 is INT5; INT4 has the higher
    // priority, which is the one the clock wants.
    #define PIN_NES_CLOCK     2          // INT4
    #define PIN_NES_DATA_IN   3          // INT5
    #define PIN_NES_DATA_OUT  4
    #define OUT0_INTERRUPT    1
    #define CLOCK_PCINT       0
  #endif
#else
  #error "NES_router is for an ESP32-C3 or an Arduino Mega 2560. An Uno or Nano has 2KB of RAM, too little to hold a page."
#endif

#define NUM_PAGES          3   // index.html, style.css, 404
#define PAGE_404_ID        2

// The ROM holds OUT0 high on a quiet line for ~2ms before every poll,
// and never leaves more than ~100us between clock pulses mid-transfer.
// So a line this quiet is never mid-transfer, and an OUT0 edge after
// one is a poll rather than a data bit.
#define LINK_QUIET_US       1000UL
#define FETCH_DEADLINE_US   5000000UL   // give up on a page after this
#define NES_SILENT_US       1000000UL   // no clocks or polls at all: fail fast

// Request frame, LSB first: 8 zeros, ready flag, id, ~id.
#define FRAME_BITS          25

// --- Port access ----------------------------------------------------
// digitalRead() and digitalWrite() are too slow for the interrupt
// handlers - the ROM leaves ~33us between clock pulses.
#if defined(ARDUINO_ARCH_ESP32)
void setupPins() {}
static inline bool clockLevel()  { return (REG_READ(GPIO_IN_REG) >> PIN_NES_CLOCK) & 1; }
static inline bool dataInLevel() { return (REG_READ(GPIO_IN_REG) >> PIN_NES_DATA_IN) & 1; }
static inline void d0Wire(bool high) {
  REG_WRITE(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << PIN_NES_DATA_OUT);
}
#else
// Each pin's port register and bit are looked up once, so the pin
// numbers above stay the only place the wiring is written down.
volatile uint8_t *clockInReg, *dataInReg, *dataOutReg;
uint8_t clockMask, dataInMask, dataOutMask;

void setupPins() {
  clockInReg  = portInputRegister(digitalPinToPort(PIN_NES_CLOCK));
  clockMask   = digitalPinToBitMask(PIN_NES_CLOCK);
  dataInReg   = portInputRegister(digitalPinToPort(PIN_NES_DATA_IN));
  dataInMask  = digitalPinToBitMask(PIN_NES_DATA_IN);
  dataOutReg  = portOutputRegister(digitalPinToPort(PIN_NES_DATA_OUT));
  dataOutMask = digitalPinToBitMask(PIN_NES_DATA_OUT);
}

static inline bool clockLevel()  { return *clockInReg & clockMask; }
static inline bool dataInLevel() { return *dataInReg & dataInMask; }
// The port register is shared with other pins, so outside an interrupt
// handler call this with interrupts off.
static inline void d0Wire(bool high) {
  if (high) *dataOutReg |= dataOutMask;
  else      *dataOutReg &= ~dataOutMask;
}
#endif

// D0 is inverted by the console, exactly as for a real pad: a pressed
// button pulls the wire low and the game reads a 1.  Everything else
// in this file speaks in the value the NES will read; this is the only
// place that knows the wire is upside down.  Idle is "NES reads 0",
// which means the wire is held HIGH.  Get it backwards and an idle
// gateway looks like every button held down.
static inline void d0Write(bool nesReads) { d0Wire(!nesReads); }

// --- Link layer state (shared with the ISRs) ------------------------
// One byte, so the main loop can read it without tearing.
enum LinkState : uint8_t { LINK_IDLE, LINK_SENDING_ID, LINK_RECEIVING };

enum LinkError : uint8_t {
  LE_NONE,
  LE_REJECTED,   // the NES read our request frame and did not act on it
  LE_CUT,        // clocks stopped partway through a response
  LE_ECHO,       // the response is for a different page
  LE_LENGTH,     // length prefix out of range
  LE_CHECKSUM,   // bits lost or flipped on the way
};

volatile LinkState linkState = LINK_IDLE;
volatile bool     requestPending = false;   // held until a response checks out
volatile uint8_t  pendingId = 0;

volatile uint32_t idFrame = 0;        // shifted right one bit per clock
volatile uint8_t  idBit = 0;

volatile uint8_t  rxByte = 0;
volatile uint8_t  rxBitCount = 0;
volatile uint32_t rxIndex = 0;        // response bytes so far, echo included
volatile uint32_t rxExpected = 0;     // packet length, from the prefix
volatile uint8_t  rxSum1 = 0;
volatile uint8_t  rxSum2 = 0;
volatile bool     rxComplete = false;

volatile uint8_t  linkError = LE_NONE;   // the most recent failure
volatile uint32_t linkErrors = 0;        // failures since boot
volatile uint32_t pollCount = 0;         // polls seen since boot

volatile uint32_t lastClockUs = 0;       // last clock edge
volatile uint32_t lastActivityUs = 0;    // last clock edge or poll

// Raw edge counts, before any judgement about what they mean.  Only
// the wiring probe uses these: they say whether each wire is alive.
volatile uint32_t clockEdges = 0;
#if OUT0_INTERRUPT
volatile uint32_t out0Edges = 0;
#endif

uint8_t packetBuf[MAX_PACKET];

const __FlashStringHelper *linkErrorName(uint8_t e) {
  switch (e) {
    case LE_REJECTED: return F("NES did not accept the request frame");
    case LE_CUT:      return F("response cut short");
    case LE_ECHO:     return F("response was for a different page");
    case LE_LENGTH:   return F("bad length prefix");
    case LE_CHECKSUM: return F("checksum mismatch");
    default:          return F("no error");
  }
}

// Drop whatever transfer is under way.  requestPending is left alone,
// so a request that is still wanted simply goes out on the next poll.
// Interrupts must be off, or this must be called from an ISR.
void IRAM_ATTR linkAbort(uint8_t why) {
  if (why != LE_NONE) {
    linkError = why;
    linkErrors++;
  }
  linkState = LINK_IDLE;
  d0Write(false);
}

// The line went quiet, or a new poll began, before we finished.  A
// half-sent request frame is nobody's fault - the NES may not be
// running our ROM, or the cable just went in - but a response that
// never started means the NES refused the frame, and one that stopped
// partway means clocks were lost.
void IRAM_ATTR linkGiveUp() {
  if (linkState == LINK_RECEIVING)
    linkAbort(rxIndex == 0 && rxBitCount == 0 ? LE_REJECTED : LE_CUT);
  else
    linkAbort(LE_NONE);
}

// The NES has started a poll.  It is the resync point for everything:
// whatever we thought was going on, start a fresh frame.
void IRAM_ATTR beginPoll(uint32_t now) {
  lastActivityUs = now;
  pollCount++;
  if (linkState != LINK_IDLE) linkGiveUp();
  if (!requestPending) return;

  // (1 << 8) | (id << 9) | (~id << 17), put together a byte at a time:
  // shifts by 9 and 17 are loops on AVR, and this is the longest path
  // through the handlers.
  uint8_t id = pendingId, inv = ~id;
  idFrame = ((uint32_t)(uint8_t)(inv >> 7) << 24) |
            ((uint32_t)(uint8_t)((id >> 7) | (inv << 1)) << 16) |
            ((uint32_t)(uint8_t)(1 | (id << 1)) << 8);
  d0Write(idFrame & 1);
  idBit = 1;
  linkState = LINK_SENDING_ID;
}

#if OUT0_INTERRUPT
// Falling edge on OUT0.  Most of these are data bits: the NES puts
// every response bit on OUT0 right beside a clock pulse.  A poll is
// the one OUT0 edge that follows a quiet line.
void IRAM_ATTR onStrobe() {
  uint32_t now = micros();
  out0Edges++;
  if (now - lastClockUs < LINK_QUIET_US) return;   // a data bit, not a poll
  beginPoll(now);
}
#endif

// One falling edge per $4016 read.  The NES samples the line during
// the pulse itself, so by the time we get here it is safe to present
// the next bit; in the other direction the NES holds its data stable
// for the whole bit period, so sampling here is comfortably inside
// the valid window.
void IRAM_ATTR onClock() {
  uint32_t now = micros();
#if !OUT0_INTERRUPT
  // With no interrupt on OUT0, a poll is recognised at its first clock
  // rather than its strobe: the first clock after a quiet line, with
  // OUT0 low.  The NES reads the frame's first bit during this very
  // pulse, before we can present it - but that bit is a zero, which an
  // idle line is already giving it (see dropIfQuiet).
  if (now - lastClockUs >= LINK_QUIET_US && !dataInLevel()) beginPoll(now);
#endif
  clockEdges++;
  uint32_t gap = now - lastActivityUs;
  lastClockUs = now;
  lastActivityUs = now;

  if (linkState == LINK_IDLE) return;
  if (gap > LINK_QUIET_US) {        // the NES went quiet and started again
    linkGiveUp();                   // without a poll we saw: not ours
    return;
  }

  if (linkState == LINK_SENDING_ID) {
    if (idBit < FRAME_BITS) {
      idFrame >>= 1;                // a variable 32 bit shift is a loop on AVR
      d0Write(idFrame & 1);
      idBit++;
    } else {
      d0Write(false);               // back to idle, the NES has the frame
      rxByte = 0; rxBitCount = 0; rxIndex = 0; rxExpected = 0;
      rxSum1 = 0; rxSum2 = 0;
      linkState = LINK_RECEIVING;
    }
    return;
  }

  // LINK_RECEIVING: [id] [len lo] [len hi] [packet...] [sum1] [sum2]
  rxByte = (rxByte >> 1) | (dataInLevel() ? 0x80 : 0x00);  // LSB first
  if (++rxBitCount < 8) return;
  rxBitCount = 0;

  uint8_t b = rxByte;
  uint32_t i = rxIndex++;

  if (i == 0) {
    if (b != pendingId) { linkAbort(LE_ECHO); return; }
  } else if (i == 1) {
    rxExpected = b;
  } else if (i == 2) {
    rxExpected |= (uint32_t)b << 8;
    if (rxExpected == 0 || rxExpected > MAX_PACKET) { linkAbort(LE_LENGTH); return; }
  } else if (i < rxExpected + 3) {
    packetBuf[i - 3] = b;
  } else if (i == rxExpected + 3) {
    if (b != rxSum1) linkAbort(LE_CHECKSUM);
    return;                         // the trailer is not part of the sum
  } else {
    if (b != rxSum2) { linkAbort(LE_CHECKSUM); return; }
    rxComplete = true;
    requestPending = false;
    linkState = LINK_IDLE;
    return;
  }
  rxSum1 += b;
  rxSum2 += rxSum1;
}

#if CLOCK_PCINT
// A pin change interrupt fires on both edges, and by the time this runs
// the pulse is usually over.  Wait out the rising edge in case it is
// not, then clear the flag it set, so that one pulse is one run.  The
// flag is set a few cycles after the pin reads high, hence the pause.
// The wait is bounded so a CLK wire shorted to ground cannot hang us.
ISR(PCINT0_vect) {
  for (uint8_t n = 0; n < 255 && !clockLevel(); n++) {}
  __builtin_avr_delay_cycles(6);
  PCIFR = _BV(PCIF0);
  onClock();
}
#endif

// A transfer on a line that has gone quiet - the cable pulled, or the
// NES stopped - is dropped, which also puts D0 back to idle before the
// next poll reads it.  Without an interrupt on OUT0 nothing else does
// that in time: a poll is only seen at its first clock, after the NES
// has read the first bit.  Interrupts must be off.
void dropIfQuiet(uint32_t now) {
  if (linkState != LINK_IDLE && now - lastActivityUs > LINK_QUIET_US) linkGiveUp();
}

void resetLink() {
  noInterrupts();
  linkState = LINK_IDLE;
  requestPending = false;
  rxComplete = false;
  d0Write(false);
  interrupts();
}

#if DECODES
// --- Packet decoding ------------------------------------------------
#define MAX_TOKENS       128
#define MAX_TOKEN_LEN     31
#define MAX_NODES        511   // 2*256-1, the worst case Huffman tree
#define MAX_TREE_DEPTH    40   // a 5KB page cannot exceed ~19

// Bits are packed MSB first by scripts/build_rom.py.
struct BitReader {
  const uint8_t *buf;
  uint32_t bits;
  uint32_t pos = 0;

  BitReader(const uint8_t *b, uint32_t byteLen) : buf(b), bits(byteLen * 8) {}

  bool read() {
    if (pos >= bits) return false;
    bool v = (buf[pos >> 3] >> (7 - (pos & 7))) & 1;
    pos++;
    return v;
  }
  uint32_t readBits(uint8_t n) {
    uint32_t v = 0;
    while (n--) v = (v << 1) | (read() ? 1 : 0);
    return v;
  }
  bool exhausted() const { return pos >= bits; }
};

// Read little endian fields explicitly: chaining receive-style calls
// inside one expression leaves the byte order up to the compiler.
static inline uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct HuffNode { int16_t left, right, sym; };

HuffNode nodes[MAX_NODES];
int16_t  nodeCount = 0;

char    tokens[MAX_TOKENS][MAX_TOKEN_LEN + 1];
uint8_t tokenLens[MAX_TOKENS];
uint8_t tokenCount = 0;

int16_t parseTree(BitReader &br, uint8_t depth) {
  if (depth > MAX_TREE_DEPTH || nodeCount >= MAX_NODES || br.exhausted()) return -1;
  int16_t self = nodeCount++;
  if (br.read()) {
    nodes[self].left = nodes[self].right = -1;
    nodes[self].sym = (int16_t)br.readBits(8);
    return self;
  }
  nodes[self].sym = -1;
  int16_t l = parseTree(br, depth + 1);
  if (l < 0) return -1;
  int16_t r = parseTree(br, depth + 1);
  if (r < 0) return -1;
  nodes[self].left = l;
  nodes[self].right = r;
  return self;
}

// Decodes one NHF2 packet. On success returns a malloc'd, NUL
// terminated string in *out and its length in *outLen.
bool decodePacket(const uint8_t *pkt, uint32_t len, char **out, uint32_t *outLen) {
  if (len < 5 || memcmp(pkt, "NHF2", 4) != 0) {
    Serial.println(F("# bad magic"));
    return false;
  }

  uint32_t idx = 4;
  tokenCount = pkt[idx++];

  for (uint8_t i = 0; i < tokenCount; i++) {
    if (idx >= len) return false;
    uint8_t tlen = pkt[idx++];
    if (tlen > MAX_TOKEN_LEN) return false;
    uint32_t packed = ((uint32_t)tlen * 7 + 7) / 8;
    if (idx + packed > len) return false;
    BitReader tr(pkt + idx, packed);
    for (uint8_t j = 0; j < tlen; j++) tokens[i][j] = (char)tr.readBits(7);
    tokens[i][tlen] = '\0';
    tokenLens[i] = tlen;
    idx += packed;
  }

  if (idx + 2 > len) return false;
  uint16_t treeBits = rd16(pkt + idx);
  idx += 2;
  uint32_t treeBytes = ((uint32_t)treeBits + 7) / 8;
  if (idx + treeBytes > len) return false;

  nodeCount = 0;
  BitReader br(pkt + idx, treeBytes);
  int16_t root = parseTree(br, 0);
  if (root < 0) {
    Serial.println(F("# bad huffman tree"));
    return false;
  }
  idx += treeBytes;

  if (idx + 8 > len) return false;
  uint16_t symbolCount = rd16(pkt + idx);
  uint16_t expandedLen = rd16(pkt + idx + 2);
  uint32_t payloadBits = rd32(pkt + idx + 4);
  idx += 8;

  uint32_t payloadBytes = (payloadBits + 7) / 8;
  if (idx + payloadBytes > len) return false;

  char *buf = (char *)malloc((size_t)expandedLen + 1);
  if (!buf) {
    Serial.println(F("# out of memory"));
    return false;
  }

  BitReader pr(pkt + idx, payloadBytes);
  uint32_t written = 0;
  int16_t curr = root;

  // symbolCount counts Huffman symbols; expandedLen counts the bytes
  // they turn into once dictionary tokens are substituted back in.
  for (uint32_t decoded = 0; decoded < symbolCount; ) {
    if (pr.exhausted()) break;
    curr = pr.read() ? nodes[curr].right : nodes[curr].left;
    if (curr < 0) break;
    if (nodes[curr].sym < 0) continue;

    uint16_t sym = (uint16_t)nodes[curr].sym;
    if (sym < 128) {
      if (written >= expandedLen) { written++; break; }
      buf[written++] = (char)sym;
    } else {
      uint8_t t = sym - 128;
      if (t >= tokenCount || written + tokenLens[t] > expandedLen) { written = expandedLen + 1; break; }
      memcpy(buf + written, tokens[t], tokenLens[t]);
      written += tokenLens[t];
    }
    decoded++;
    curr = root;
  }

  if (written != expandedLen) {
    char msg[64];
    snprintf_P(msg, sizeof msg, PSTR("# decode mismatch: %lu bytes, header says %u"),
               (unsigned long)written, (unsigned)expandedLen);
    Serial.println(msg);
    free(buf);
    return false;
  }

  buf[written] = '\0';
  *out = buf;
  *outLen = written;
  return true;
}

// --- Page cache -----------------------------------------------------
// The link runs at ~27 kbit/s, so fetch each page from the cartridge
// once and serve every later hit from RAM.
char     *pageCache[NUM_PAGES] = {nullptr, nullptr, nullptr};
uint32_t  pageLen[NUM_PAGES] = {0, 0, 0};
#endif  // DECODES

// Ask the NES for a page and wait for it.  On success the page is in
// the cache (DECODES) or its raw packet in packetBuf, rxExpected bytes
// long.  Retrying is automatic: the request stays pending through any
// failure, so the next poll carries it again.  This loop only watches
// for a line that has gone quiet mid-transfer, reports what went
// wrong, and decides when to stop.
bool fetchPage(uint8_t id) {
  if (id >= NUM_PAGES) id = PAGE_404_ID;
#if DECODES
  if (pageCache[id]) return true;
#endif

  noInterrupts();
  linkState = LINK_IDLE;
  rxComplete = false;
  pendingId = id;
  requestPending = true;
  d0Write(false);
  uint32_t start = micros();
  uint32_t errorsAtStart = linkErrors;
  uint32_t pollsAtStart = pollCount;
  interrupts();

  uint32_t errorsSeen = errorsAtStart;
  bool silent = false, expired = false;

  for (;;) {
    noInterrupts();
    uint32_t now = micros();
    dropIfQuiet(now);
    bool done = rxComplete;
    uint32_t errors = linkErrors;
    uint8_t lastError = linkError;
    uint32_t quietFor = now - lastActivityUs;
    interrupts();

    if (done) break;
    if (errors != errorsSeen) {
      errorsSeen = errors;
      Serial.print(F("# link: "));
      Serial.print(linkErrorName(lastError));
      Serial.println(F(", retrying"));
    }
    if (now - start > NES_SILENT_US && quietFor > NES_SILENT_US) { silent = true; break; }
    if (now - start > FETCH_DEADLINE_US) { expired = true; break; }
    delayMicroseconds(200);
  }

  char msg[140];
  uint32_t ms = (micros() - start) / 1000;
  if (silent || expired) {
    resetLink();
    noInterrupts();
    uint32_t polls = pollCount - pollsAtStart;
    uint32_t errors = linkErrors - errorsAtStart;
    interrupts();
    if (silent)
      snprintf_P(msg, sizeof msg,
                 PSTR("# NES link timeout: port silent for %lu ms - cable out, NES off, or ROM not running (GET /_link probes the wiring)"),
                 (unsigned long)(NES_SILENT_US / 1000));
    else
      snprintf_P(msg, sizeof msg,
                 PSTR("# NES link timeout: %lu polls, %lu link errors, no good response in %lu ms"),
                 (unsigned long)polls, (unsigned long)errors, (unsigned long)ms);
    Serial.println(msg);
    return false;
  }

  noInterrupts();
  uint32_t len = rxExpected;
  uint32_t retries = linkErrors - errorsAtStart;
  interrupts();

#if DECODES
  char *text;
  uint32_t textLen;
  if (!decodePacket(packetBuf, len, &text, &textLen)) return false;
  pageCache[id] = text;
  pageLen[id] = textLen;
  snprintf_P(msg, sizeof msg, PSTR("# page %u: %lu byte packet -> %lu bytes in %lu ms, %lu retries"),
             (unsigned)id, (unsigned long)len, (unsigned long)textLen, (unsigned long)ms,
             (unsigned long)retries);
#else
  snprintf_P(msg, sizeof msg, PSTR("# page %u: %lu byte packet in %lu ms, %lu retries"),
             (unsigned)id, (unsigned long)len, (unsigned long)ms, (unsigned long)retries);
#endif
  Serial.println(msg);
  return true;
}

// Report the port coming and going, so a hot plug shows up in the log
// even when nothing is being fetched.  Any software that strobes the
// port counts - a flash cart menu as much as the web server ROM.
void watchLink() {
  static bool active = false;
  static uint32_t lastPolls = 0, lastSeen = 0;
  noInterrupts();
  uint32_t polls = pollCount;
  interrupts();
  uint32_t now = micros();
  if (polls != lastPolls) {
    lastPolls = polls;
    lastSeen = now;
    if (!active) {
      active = true;
      Serial.println(F("# NES port active"));
    }
  } else if (active && now - lastSeen > NES_SILENT_US) {
    active = false;
    Serial.println(F("# NES port quiet - cable out, NES off, or nothing polling"));
  }
}

// --- Wiring probe -----------------------------------------------------
// GET /_link watches both inputs for a second and reports what arrived.
// Each port line has its own signature while the web server ROM runs,
// so a wire landing on the wrong NES pin shows up without a scope:
//
//   OUT0        ~400 falling edges a second, high ~85% of the time
//   CLK         ~3600 falling edges a second, high ~100% - its pulses
//               are far too short to add up to any time spent low
//
// With no interrupt on OUT0, its falls are counted from the same
// samples the percentages come from.  Each poll holds OUT0 high for
// ~2ms, so none are missed.
struct LinkProbe {
  uint32_t out0Edges, clockEdges, polls, errors;
  uint32_t out0HighPct, clockHighPct;
};

void measureLink(uint32_t windowUs, LinkProbe *p) {
  noInterrupts();
#if OUT0_INTERRUPT
  uint32_t out0Start = out0Edges;
#endif
  uint32_t clockStart = clockEdges;
  uint32_t pollStart = pollCount, errorStart = linkErrors;
  interrupts();

  uint32_t samples = 0, out0High = 0, clockHigh = 0, out0Falls = 0;
  bool out0Was = dataInLevel();
  uint32_t start = micros();
  do {
    samples++;
    bool out0 = dataInLevel();
    if (out0) out0High++;
    else if (out0Was) out0Falls++;
    out0Was = out0;
    if (clockLevel()) clockHigh++;
    delayMicroseconds(100);
  } while (micros() - start < windowUs);

  noInterrupts();
#if OUT0_INTERRUPT
  p->out0Edges = out0Edges - out0Start;
#else
  p->out0Edges = out0Falls;
#endif
  p->clockEdges = clockEdges - clockStart;
  p->polls = pollCount - pollStart;
  p->errors = linkErrors - errorStart;
  interrupts();
  p->out0HighPct = out0High * 100 / samples;
  p->clockHighPct = clockHigh * 100 / samples;
}

int formatProbe(const LinkProbe *p, char *out, size_t n) {
  int len = snprintf_P(out, n, PSTR(
    "NES link probe, 1 second\n"
    "\n"
    "  " PIN_WORD " %d, OUT0: %6lu falling edges, high %3lu%% of the time\n"
    "  " PIN_WORD " %d, CLK:  %6lu falling edges, high %3lu%% of the time\n"
    "  polls recognised %lu, link errors %lu\n"
    "\n"
    "With the web server ROM running, a healthy link reads about:\n"
    "  OUT0     400 falling edges, high  85%%\n"
    "  CLK     3600 falling edges, high 100%%\n"
    "\n"
    "Other signatures:\n"
#if defined(ARDUINO_ARCH_ESP32)
    "  no edges, high 100%%   wire is on +5V, or on CLK with its pulses lost\n"
    "  no edges, low 100%%    wire is on D3, D4, or not making contact\n"
#else
    "  no edges, high 100%%   no contact (the pull-up holds it), or on +5V\n"
    "  no edges, low 100%%    wire is on GND\n"
#endif
    ),
    PIN_NES_DATA_IN, (unsigned long)p->out0Edges, (unsigned long)p->out0HighPct,
    PIN_NES_CLOCK, (unsigned long)p->clockEdges, (unsigned long)p->clockHighPct,
    (unsigned long)p->polls, (unsigned long)p->errors);
  if (len < 0) return 0;
  return len < (int)n ? len : (int)n - 1;
}

// --- HTTP over the serial link to the host --------------------------
struct Route { const char *path; uint8_t id; const char *mime; };

const Route routes[] = {
  {"/",           0, "text/html"},
  {"/index.html", 0, "text/html"},
  {"/style.css",  1, "text/css"},
};

// The serial link to the host on its own, with no NES involved: a known
// pattern that scripts/serial_test.py checks byte for byte.
#define SERIAL_TEST_BYTES 16384U
static inline uint8_t serialTestByte(uint16_t i) { return (uint8_t)i ^ (uint8_t)(i >> 8); }

void serve(const char *path) {
  if (strcmp(path, "/_serial") == 0) {
    Serial.print(F("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "));
    Serial.print(SERIAL_TEST_BYTES);
    Serial.print(F("\r\n\r\n"));
    for (uint16_t i = 0; i < SERIAL_TEST_BYTES; i++) Serial.write(serialTestByte(i));
    return;
  }

  if (strcmp(path, "/_link") == 0) {
    LinkProbe p;
    char report[640];
    measureLink(1000000, &p);
    int len = formatProbe(&p, report, sizeof report);
    Serial.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "));
    Serial.print(len);
    Serial.print(F("\r\n\r\n"));
    Serial.write((const uint8_t *)report, len);
    return;
  }

  uint8_t id = PAGE_404_ID;
  const char *mime = "text/html";
  for (const Route &r : routes) {
    if (strcmp(path, r.path) == 0) { id = r.id; mime = r.mime; break; }
  }

  if (!fetchPage(id)) {
    Serial.print(F("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"));
    return;
  }

#if DECODES
  const uint8_t *body = (const uint8_t *)pageCache[id];
  uint32_t len = pageLen[id];
#else
  // Once a response has checked out the request is cleared, so the
  // interrupt handlers leave packetBuf alone while it goes out.
  const uint8_t *body = packetBuf;
  noInterrupts();
  uint32_t len = rxExpected;
  interrupts();
#endif

  Serial.print(id != PAGE_404_ID ? F("HTTP/1.1 200 OK\r\n") : F("HTTP/1.1 404 Not Found\r\n"));
  Serial.print(F("Content-Type: "));
  Serial.print(mime);
#if !DECODES
  Serial.print(F("\r\nX-NES-Packet: nhf2"));   // serial_bridge.py decompresses it
#endif
  Serial.print(F("\r\nContent-Length: "));
  Serial.print(len);
  Serial.print(F("\r\n\r\n"));
  Serial.write(body, len);
}

// Accept either a bare path or a request line like "GET /x HTTP/1.1".
void handleRequest(char *line) {
  char *path = strchr(line, '/');
  if (!path) return;
  path[strcspn(path, " ?")] = '\0';
  serve(path);
}

void setup() {
  // 16MHz divides 250000 exactly, where 115200 comes out 2.1% fast, so
  // any USB chip on the other end agrees with it.  The SuperMini's
  // native USB ignores the baud rate.
  Serial.begin(250000);
#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif
  setupPins();

  pinMode(PIN_NES_DATA_IN, NES_INPUT_MODE);
  pinMode(PIN_NES_CLOCK, NES_INPUT_MODE);
  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);

#if CLOCK_PCINT
  PCMSK0 |= _BV(CLOCK_PCMSK_BIT);
  PCIFR = _BV(PCIF0);
  PCICR |= _BV(PCIE0);
#else
  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK), onClock, FALLING);
#endif
#if OUT0_INTERRUPT
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);
#endif

  Serial.println(F("# " BOARD_NAME " gateway online. Send a path, e.g. /index.html"));
}

void loop() {
  watchLink();

  static char line[96];
  static uint8_t lineLen = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (lineLen < sizeof line - 1) line[lineLen++] = c;
      continue;
    }
    line[lineLen] = '\0';
    lineLen = 0;
    handleRequest(line);
  }
}
