// ===================================================================
//  NES WEB SERVER - Arduino Mega 2560 gateway
//
//  The same gateway as src/firmware/NES_router (ESP32-C3), for a 5V
//  board.  The link layer is a straight port of that one and must stay
//  in step with it: docs/protocol.md has the wire format, docs/mega.md
//  this board.
//
//    CLK   (port pin 2) -> pin 2   INT4: one falling edge per $4016 read
//    OUT0  (port pin 3) -> pin 3   INT5: NES data out + poll strobe
//    D0    (port pin 4) <- pin 4   our data into the NES
//    GND   (port pin 1) -- GND
//    +5V   (port pin 7)    not connected: the Mega runs from USB
//
//  Each signal goes through a 1k series resistor.  The Mega is a 5V
//  part, so nothing needs level shifting; the resistors are only there
//  to limit the current when one side is powered and the other is not.
//
//  How it differs from the ESP32 version:
//
//  - 8KB of RAM is not enough to decompress pages.  Each page goes to
//    the host exactly as the cartridge stores it, marked with an
//    "X-NES-Packet: nhf2" header, and scripts/serial_bridge.py
//    decompresses it.  Nothing is cached: every request goes to the NES.
//  - The inputs use the internal pull-ups, so a pulled cable leaves them
//    high and quiet instead of floating.
//  - Opening the serial port resets the board, so the bridge waits for
//    the "online" line before sending its first request.
//
//  D0 is INVERTED by the console, as for a real pad: wire low reads as
//  1.  So an idle gateway holds the wire high.  See d0Write().
//
//  The cable can go in or come out at any time.  Every poll from the
//  NES is a resync point, a request stays pending until a response
//  checks out, and a line that goes quiet mid-transfer is dropped.
// ===================================================================

#include <string.h>

// Forward declaration: the Arduino builder auto-generates function
// prototypes at the top of the translation unit, before the
// definitions below - without this, the generated prototype for
// measureLink(..., LinkProbe*) fails to compile.
struct LinkProbe;

#if defined(RAMEND) && RAMEND < 0x2000
#error "NES_router_mega needs the 8KB of RAM on an Arduino Mega 2560 - an Uno or Nano has 2KB"
#endif

// Only pins 2, 3 and 18-21 have external interrupts on a Mega, and
// 18-21 are Serial1 and I2C.  2 is INT4 and 3 is INT5; INT4 has the
// higher priority, which is the one the clock wants.
#define PIN_NES_CLOCK     2
#define PIN_NES_DATA_IN   3
#define PIN_NES_DATA_OUT  4

#define MAX_PACKET      4096   // largest page packet we will accept: half the RAM
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

// --- Direct port access ---------------------------------------------
// digitalRead() and digitalWrite() cost several microseconds each on
// AVR, which the interrupt handlers cannot spare - the ROM leaves ~33us
// between clock pulses.  So each pin's port register and bit are looked
// up once, in setupPins(), and the pin numbers above stay the only
// place the wiring is written down.
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

// D0 is inverted by the console, exactly as for a real pad: a pressed
// button pulls the wire low and the game reads a 1.  Everything else
// in this file speaks in the value the NES will read; this is the only
// place that knows the wire is upside down.  Idle is "NES reads 0",
// which means the wire is held HIGH.
//
// The port register is shared with other pins, so outside an interrupt
// handler call this with interrupts off.
static inline void d0Write(bool nesReads) {
  if (nesReads) *dataOutReg &= ~dataOutMask;
  else          *dataOutReg |= dataOutMask;
}

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
volatile uint32_t out0Edges = 0;
volatile uint32_t clockEdges = 0;

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
void linkAbort(uint8_t why) {
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
void linkGiveUp() {
  if (linkState == LINK_RECEIVING)
    linkAbort(rxIndex == 0 && rxBitCount == 0 ? LE_REJECTED : LE_CUT);
  else
    linkAbort(LE_NONE);
}

// Falling edge on OUT0.  Most of these are data bits: the NES puts
// every response bit on OUT0 right beside a clock pulse.  A poll is
// the one OUT0 edge that follows a quiet line, and it is the resync
// point for everything - whatever we thought was going on, the NES
// has just started a fresh poll, so start a fresh frame.
void onStrobe() {
  uint32_t now = micros();
  out0Edges++;
  if (now - lastClockUs < LINK_QUIET_US) return;   // a data bit, not a poll

  lastActivityUs = now;
  pollCount++;
  if (linkState != LINK_IDLE) linkGiveUp();
  if (!requestPending) return;

  idFrame = (1UL << 8) | ((uint32_t)pendingId << 9) |
            ((uint32_t)(uint8_t)~pendingId << 17);
  d0Write(idFrame & 1);
  idBit = 1;
  linkState = LINK_SENDING_ID;
}

// One falling edge per $4016 read.  The NES samples the line during
// the pulse itself, so by the time we get here it is safe to present
// the next bit; in the other direction the NES holds its data stable
// for the whole bit period, so sampling here is comfortably inside
// the valid window.
void onClock() {
  uint32_t now = micros();
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

void resetLink() {
  noInterrupts();
  linkState = LINK_IDLE;
  requestPending = false;
  rxComplete = false;
  d0Write(false);
  interrupts();
}

// Ask the NES for a page and wait for it.  On success the packet is in
// packetBuf, rxExpected bytes long.  Retrying is automatic: the request
// stays pending through any failure, so the next poll carries it again.
// This loop only watches for a line that has gone quiet mid-transfer,
// reports what went wrong, and decides when to stop.
bool fetchPage(uint8_t id) {
  if (id >= NUM_PAGES) id = PAGE_404_ID;

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
    if (linkState != LINK_IDLE && now - lastActivityUs > LINK_QUIET_US)
      linkGiveUp();                 // cable pulled, or the NES stopped
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

  char msg[120];
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
  snprintf_P(msg, sizeof msg, PSTR("# page %u: %lu byte packet in %lu ms, %lu retries"),
             (unsigned)id, (unsigned long)len, (unsigned long)ms, (unsigned long)retries);
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
//   no contact  no edges, high 100% (the pull-up holds it), as does +5V
//   GND         no edges, low 100%
struct LinkProbe {
  uint32_t out0Edges, clockEdges, polls, errors;
  uint32_t out0HighPct, clockHighPct;
};

void measureLink(uint32_t windowUs, LinkProbe *p) {
  noInterrupts();
  uint32_t out0Start = out0Edges, clockStart = clockEdges;
  uint32_t pollStart = pollCount, errorStart = linkErrors;
  interrupts();

  uint32_t samples = 0, out0High = 0, clockHigh = 0;
  uint32_t start = micros();
  do {
    samples++;
    if (dataInLevel()) out0High++;
    if (clockLevel()) clockHigh++;
    delayMicroseconds(100);
  } while (micros() - start < windowUs);

  noInterrupts();
  p->out0Edges = out0Edges - out0Start;
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
    "  pin %d, OUT0: %6lu falling edges, high %3lu%% of the time\n"
    "  pin %d, CLK:  %6lu falling edges, high %3lu%% of the time\n"
    "  polls recognised %lu, link errors %lu\n"
    "\n"
    "With the web server ROM running, a healthy link reads about:\n"
    "  OUT0     400 falling edges, high  85%%\n"
    "  CLK     3600 falling edges, high 100%%\n"
    "\n"
    "Other signatures:\n"
    "  no edges, high 100%%   no contact (the pull-up holds it), or on +5V\n"
    "  no edges, low 100%%    wire is on GND\n"),
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

void serve(const char *path) {
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

  // Once a response has checked out the request is cleared, so the
  // interrupt handlers leave packetBuf alone while it goes out.
  noInterrupts();
  uint32_t len = rxExpected;
  interrupts();

  Serial.print(id != PAGE_404_ID ? F("HTTP/1.1 200 OK\r\n") : F("HTTP/1.1 404 Not Found\r\n"));
  Serial.print(F("Content-Type: "));
  Serial.print(mime);
  Serial.print(F("\r\nX-NES-Packet: nhf2\r\nContent-Length: "));
  Serial.print(len);
  Serial.print(F("\r\n\r\n"));
  Serial.write(packetBuf, len);
}

// Accept either a bare path or a request line like "GET /x HTTP/1.1".
void handleRequest(char *line) {
  char *path = strchr(line, '/');
  if (!path) return;
  path[strcspn(path, " ?")] = '\0';
  serve(path);
}

void setup() {
  Serial.begin(115200);
  setupPins();

  pinMode(PIN_NES_DATA_IN, INPUT_PULLUP);
  pinMode(PIN_NES_CLOCK, INPUT_PULLUP);
  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);

  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK), onClock, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);

  Serial.println(F("# Arduino Mega gateway online. Send a path, e.g. /index.html"));
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
