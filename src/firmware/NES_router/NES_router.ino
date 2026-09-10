// ===================================================================
//  NES WEB SERVER - ESP32-C3 gateway
//
//  Talks to the cartridge over controller port 1.  The port pinout is
//  identical either way, so this file needs no change to move ports -
//  the ROM chooses, by reading $4016 or $4017.  See
//  docs/protocol.md for the wire format; the short version:
//
//    CLK      (port pin 2) input   one falling edge per $4016 read
//    DATA_IN  (port pin 3) input   OUT0: NES data out + poll strobe
//                                  -> GPIO 3 (not 5)
//    DATA_OUT (port pin 4) output  D0: our data into the NES
//
//  The clock pulse is only ~0.56us wide - about one 6502 cycle - and
//  cannot be widened in software, so both directions are driven from
//  a GPIO interrupt.  Polling with digitalRead() misses edges.
//
//  The NES port is 5V.  The C3 is NOT 5V tolerant: CLK and DATA_IN
//  need level shifting (a 10k/20k divider is enough for these
//  speeds).  DATA_OUT drives the NES directly at 3.3V through 1k, which
//  limits the current into the console's input when it is switched
//  off and this is not.
//
//  D0 is INVERTED by the console, as for a real pad: wire low reads as
//  1.  So an idle gateway holds the wire high.  See d0Write().
//
//  The cable can go in or come out at any time.  Every poll from the
//  NES is a resync point, a request stays pending until a response
//  checks out, and a line that goes quiet mid-transfer is dropped.
// ===================================================================

#include <string.h>
#include "soc/gpio_reg.h"

// Forward declarations: the Arduino builder auto-generates function
// prototypes at the top of the translation unit, before the
// definitions below - without these, the generated prototypes for
// parseTree(BitReader&, ...) and measureLink(..., LinkProbe*) fail to
// compile.
struct BitReader;
struct LinkProbe;

// Safe GPIOs on an ESP32-C3 SuperMini: 2/8/9 are strapping pins,
// 18/19 are the native USB pair, 20/21 are UART0.  GPIO 3 has no
// strapping role and is free to use.
//
// DATA_IN is on 3, not 5.  An unconnected input next to a switching
// output couples to it hard enough to look like a shorted wire, so
// if this ever reads as a copy of DATA_OUT, suspect the pin number
// before you suspect the solder.
#define PIN_NES_CLOCK     4
#define PIN_NES_DATA_IN   3
#define PIN_NES_DATA_OUT  6

#define MAX_PACKET      8192   // largest page packet we will accept
#define MAX_TOKENS       128
#define MAX_TOKEN_LEN     31
#define MAX_NODES        511   // 2*256-1, the worst case Huffman tree
#define MAX_TREE_DEPTH    40   // a 5KB page cannot exceed ~19
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

// --- Raw register GPIO: digitalRead/Write are too slow for the ISR ---
static inline bool pinLevel(uint8_t pin) {
  return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}
static inline void pinSet(uint8_t pin, bool level) {
  REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << pin);
}

// D0 is inverted by the console, exactly as for a real pad: a pressed
// button pulls the wire low and the game reads a 1.  Everything else
// in this file speaks in the value the NES will read; this is the only
// place that knows the wire is upside down.  Idle is "NES reads 0",
// which means the wire is held HIGH.  Get it backwards and an idle
// gateway looks like every button held down.
static inline void d0Write(bool nesReads) {
  pinSet(PIN_NES_DATA_OUT, !nesReads);
}

// --- Link layer state (shared with the ISRs) ------------------------
enum LinkState { LINK_IDLE, LINK_SENDING_ID, LINK_RECEIVING };

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

volatile uint32_t idFrame = 0;
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

const char *linkErrorName(uint8_t e) {
  switch (e) {
    case LE_REJECTED: return "NES did not accept the request frame";
    case LE_CUT:      return "response cut short";
    case LE_ECHO:     return "response was for a different page";
    case LE_LENGTH:   return "bad length prefix";
    case LE_CHECKSUM: return "checksum mismatch";
    default:          return "no error";
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

// Falling edge on OUT0.  Most of these are data bits: the NES puts
// every response bit on OUT0 right beside a clock pulse.  A poll is
// the one OUT0 edge that follows a quiet line, and it is the resync
// point for everything - whatever we thought was going on, the NES
// has just started a fresh poll, so start a fresh frame.
void IRAM_ATTR onStrobe() {
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
void IRAM_ATTR onClock() {
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
      d0Write((idFrame >> idBit) & 1);
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
  rxByte = (rxByte >> 1) | (pinLevel(PIN_NES_DATA_IN) ? 0x80 : 0x00);  // LSB first
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

// --- Packet decoding ------------------------------------------------
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
    Serial.println("# bad magic");
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
    Serial.println("# bad huffman tree");
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
    Serial.println("# out of memory");
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
    Serial.printf("# decode mismatch: %u bytes, header says %u\n", written, expandedLen);
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

// Ask the NES for a page and wait for it.  Retrying is automatic: the
// request stays pending through any failure, so the next poll carries
// it again.  This loop only watches for a line that has gone quiet
// mid-transfer, reports what went wrong, and decides when to stop.
bool fetchPage(uint8_t id) {
  if (id >= NUM_PAGES) id = PAGE_404_ID;
  if (pageCache[id]) return true;

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
      Serial.printf("# link: %s, retrying\n", linkErrorName(lastError));
    }
    if (now - start > NES_SILENT_US && quietFor > NES_SILENT_US) { silent = true; break; }
    if (now - start > FETCH_DEADLINE_US) { expired = true; break; }
    delayMicroseconds(200);
  }

  uint32_t ms = (micros() - start) / 1000;
  if (silent || expired) {
    resetLink();
    if (silent)
      Serial.printf("# NES link timeout: port silent for %lu ms - cable out, NES off, or ROM not running (GET /_link probes the wiring)\n",
                    (unsigned long)(NES_SILENT_US / 1000));
    else
      Serial.printf("# NES link timeout: %lu polls, %lu link errors, no good response in %lu ms\n",
                    (unsigned long)(pollCount - pollsAtStart),
                    (unsigned long)(linkErrors - errorsAtStart), (unsigned long)ms);
    return false;
  }

  noInterrupts();
  uint32_t len = rxExpected;
  rxComplete = false;
  interrupts();

  char *text;
  uint32_t textLen;
  if (!decodePacket(packetBuf, len, &text, &textLen)) return false;

  pageCache[id] = text;
  pageLen[id] = textLen;
  Serial.printf("# page %u: %lu byte packet -> %lu bytes in %lu ms, %lu retries\n",
                id, (unsigned long)len, (unsigned long)textLen, (unsigned long)ms,
                (unsigned long)(linkErrors - errorsAtStart));
  return true;
}

// Report the port coming and going, so a hot plug shows up in the log
// even when nothing is being fetched.  Any software that strobes the
// port counts - a flash cart menu as much as the web server ROM.
void watchLink() {
  static bool active = false;
  static uint32_t lastPolls = 0, lastSeen = 0;
  uint32_t now = micros();
  uint32_t polls = pollCount;
  if (polls != lastPolls) {
    lastPolls = polls;
    lastSeen = now;
    if (!active) {
      active = true;
      Serial.println("# NES port active");
    }
  } else if (active && now - lastSeen > NES_SILENT_US) {
    active = false;
    Serial.println("# NES port quiet - cable out, NES off, or nothing polling");
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
//   +5V         no edges, high 100%
//   D3, D4, or  no edges, low 100% (the divider pulls the pin down)
//   no contact
struct LinkProbe {
  uint32_t out0Edges, clockEdges, polls, errors;
  uint32_t out0HighPct, clockHighPct;
};

void measureLink(uint32_t windowUs, LinkProbe *p) {
  uint32_t out0Start = out0Edges, clockStart = clockEdges;
  uint32_t pollStart = pollCount, errorStart = linkErrors;
  uint32_t samples = 0, out0High = 0, clockHigh = 0;
  uint32_t start = micros();
  do {
    uint32_t in = REG_READ(GPIO_IN_REG);
    samples++;
    out0High += (in >> PIN_NES_DATA_IN) & 1;
    clockHigh += (in >> PIN_NES_CLOCK) & 1;
    delayMicroseconds(100);
  } while (micros() - start < windowUs);

  p->out0Edges = out0Edges - out0Start;
  p->clockEdges = clockEdges - clockStart;
  p->polls = pollCount - pollStart;
  p->errors = linkErrors - errorStart;
  p->out0HighPct = out0High * 100 / samples;
  p->clockHighPct = clockHigh * 100 / samples;
}

int formatProbe(const LinkProbe *p, char *out, size_t n) {
  int len = snprintf(out, n,
    "NES link probe, 1 second\n"
    "\n"
    "  GPIO %d, OUT0: %6lu falling edges, high %3lu%% of the time\n"
    "  GPIO %d, CLK:  %6lu falling edges, high %3lu%% of the time\n"
    "  polls recognised %lu, link errors %lu\n"
    "\n"
    "With the web server ROM running, a healthy link reads about:\n"
    "  OUT0     400 falling edges, high  85%%\n"
    "  CLK     3600 falling edges, high 100%%\n"
    "\n"
    "Other signatures:\n"
    "  no edges, high 100%%   wire is on +5V, or on CLK with its pulses lost\n"
    "  no edges, low 100%%    wire is on D3, D4, or not making contact\n",
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

void serve(const String &path) {
  if (path == "/_link") {
    LinkProbe p;
    char report[1024];
    measureLink(1000000, &p);
    int len = formatProbe(&p, report, sizeof report);
    Serial.printf("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n", len);
    Serial.write((const uint8_t *)report, len);
    return;
  }

  uint8_t id = PAGE_404_ID;
  const char *mime = "text/html";
  for (const Route &r : routes) {
    if (path == r.path) { id = r.id; mime = r.mime; break; }
  }

  if (!fetchPage(id)) {
    Serial.print("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
    return;
  }

  bool found = (id != PAGE_404_ID);
  Serial.printf("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n",
                found ? "200 OK" : "404 Not Found", mime, pageLen[id]);
  Serial.write((const uint8_t *)pageCache[id], pageLen[id]);
}

void setup() {
  Serial.begin(115200);   // native USB CDC on the SuperMini: baud is ignored
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  pinMode(PIN_NES_DATA_IN, INPUT);
  pinMode(PIN_NES_CLOCK, INPUT);
  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);

  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK), onClock, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);

  Serial.println("# ESP32-C3 gateway online. Send a path, e.g. /index.html");
}

void loop() {
  watchLink();
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // Accept either a bare path or a request line like "GET /x HTTP/1.1"
  int slash = line.indexOf('/');
  if (slash < 0) return;
  int end = line.indexOf(' ', slash);
  String path = (end < 0) ? line.substring(slash) : line.substring(slash, end);
  int query = path.indexOf('?');
  if (query >= 0) path = path.substring(0, query);

  serve(path);
}
