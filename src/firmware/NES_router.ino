// ===================================================================
//  NES WEB SERVER - ESP32-C3 gateway
//
//  Talks to the cartridge over controller port 1 only.  See
//  docs/protocol.md for the wire format; the short version:
//
//    CLK      (port pin 2) input   one falling edge per $4016 read
//    DATA_IN  (port pin 3) input   OUT0: NES data out + poll strobe
//    DATA_OUT (port pin 4) output  D0: our data into the NES
//
//  The clock pulse is only ~0.56us wide - about one 6502 cycle - and
//  cannot be widened in software, so both directions are driven from
//  a GPIO interrupt.  Polling with digitalRead() misses edges.
//
//  The NES port is 5V.  The C3 is NOT 5V tolerant: CLK and DATA_IN
//  need level shifting (a 10k/20k divider is enough for these
//  speeds).  DATA_OUT can drive the NES directly at 3.3V.
// ===================================================================

#include <string.h>
#include "soc/gpio_reg.h"

// Safe GPIOs on an ESP32-C3 SuperMini: 2/8/9 are strapping pins,
// 18/19 are the native USB pair, 20/21 are UART0.
#define PIN_NES_CLOCK     4
#define PIN_NES_DATA_IN   5
#define PIN_NES_DATA_OUT  6

#define MAX_PACKET      8192   // largest page packet we will accept
#define MAX_TOKENS       128
#define MAX_TOKEN_LEN     31
#define MAX_NODES        511   // 2*256-1, the worst case Huffman tree
#define MAX_TREE_DEPTH    40   // a 5KB page cannot exceed ~19
#define NUM_PAGES          3   // index.html, style.css, 404
#define PAGE_404_ID        2

#define LINK_TIMEOUT_US  500000UL

// --- Raw register GPIO: digitalRead/Write are too slow for the ISR ---
static inline bool pinLevel(uint8_t pin) {
  return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}
static inline void pinSet(uint8_t pin, bool level) {
  REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << pin);
}

// --- Link layer state (shared with the ISRs) ------------------------
enum LinkState { LINK_IDLE, LINK_SENDING_ID, LINK_RECEIVING };

volatile LinkState linkState = LINK_IDLE;
volatile bool     requestPending = false;
volatile uint8_t  pendingId = 0;

volatile uint16_t idFrame = 0;        // ready flag + 8 id bits, LSB first
volatile uint8_t  idBit = 0;

volatile uint8_t  rxByte = 0;
volatile uint8_t  rxBitCount = 0;
volatile uint32_t rxIndex = 0;        // bytes seen, including the 2 length bytes
volatile uint32_t rxExpected = 0;
volatile bool     rxComplete = false;
volatile bool     rxOverflow = false;
volatile uint32_t lastEdgeMicros = 0;

uint8_t packetBuf[MAX_PACKET];

// The NES strobes OUT0 before every poll.  We arm the outgoing frame
// on that falling edge, which is what keeps the two sides in step -
// while a response is streaming, OUT0 is carrying data, so we ignore
// it unless we are idle.
void IRAM_ATTR onStrobe() {
  if (linkState != LINK_IDLE || !requestPending) return;
  idFrame = (uint16_t)(pendingId << 1) | 1;   // bit 0 = "I have a request"
  pinSet(PIN_NES_DATA_OUT, idFrame & 1);
  idBit = 1;
  linkState = LINK_SENDING_ID;
  lastEdgeMicros = micros();
}

// One falling edge per $4016 read.  The NES samples the line during
// the pulse itself, so by the time we get here it is safe to present
// the next bit; in the other direction the NES holds its data stable
// for the whole bit period, so sampling here is comfortably inside
// the valid window.
void IRAM_ATTR onClock() {
  lastEdgeMicros = micros();

  if (linkState == LINK_SENDING_ID) {
    if (idBit < 9) {
      pinSet(PIN_NES_DATA_OUT, (idFrame >> idBit) & 1);
      idBit++;
    } else {
      pinSet(PIN_NES_DATA_OUT, 0);      // idle low again
      requestPending = false;           // the NES has taken it
      rxByte = 0; rxBitCount = 0; rxIndex = 0; rxExpected = 0;
      rxComplete = false; rxOverflow = false;
      linkState = LINK_RECEIVING;
    }
    return;
  }

  if (linkState != LINK_RECEIVING) return;

  rxByte = (rxByte >> 1) | (pinLevel(PIN_NES_DATA_IN) ? 0x80 : 0x00);  // LSB first
  if (++rxBitCount < 8) return;
  rxBitCount = 0;

  uint8_t b = rxByte;
  rxByte = 0;

  if (rxIndex == 0) {
    rxExpected = b;                     // length low
  } else if (rxIndex == 1) {
    rxExpected |= (uint32_t)b << 8;     // length high
    if (rxExpected == 0 || rxExpected > MAX_PACKET) {
      rxOverflow = true;
      linkState = LINK_IDLE;
      return;
    }
  } else {
    packetBuf[rxIndex - 2] = b;
    if (rxIndex - 1 >= rxExpected) {
      rxComplete = true;
      linkState = LINK_IDLE;
      return;
    }
  }
  rxIndex++;
}

void resetLink() {
  noInterrupts();
  linkState = LINK_IDLE;
  requestPending = false;
  rxComplete = false;
  rxOverflow = false;
  rxBitCount = 0;
  rxIndex = 0;
  interrupts();
  pinSet(PIN_NES_DATA_OUT, 0);
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

bool fetchPage(uint8_t id) {
  if (id >= NUM_PAGES) id = PAGE_404_ID;
  if (pageCache[id]) return true;

  resetLink();
  noInterrupts();
  pendingId = id;
  requestPending = true;
  interrupts();

  uint32_t start = micros();
  lastEdgeMicros = start;
  while (!rxComplete && !rxOverflow) {
    if (micros() - lastEdgeMicros > LINK_TIMEOUT_US) {
      resetLink();
      Serial.println("# NES link timeout");
      return false;
    }
    delayMicroseconds(200);
  }

  if (rxOverflow) {
    resetLink();
    Serial.println("# bad packet length from NES");
    return false;
  }

  uint32_t len = rxExpected;
  rxComplete = false;

  char *text;
  uint32_t textLen;
  if (!decodePacket(packetBuf, len, &text, &textLen)) return false;

  pageCache[id] = text;
  pageLen[id] = textLen;
  Serial.printf("# page %u: %u byte packet -> %u bytes in %lu ms\n",
                id, len, textLen, (unsigned long)((micros() - start) / 1000));
  return true;
}

// --- HTTP over the serial link to the host --------------------------
struct Route { const char *path; uint8_t id; const char *mime; };

const Route routes[] = {
  {"/",           0, "text/html"},
  {"/index.html", 0, "text/html"},
  {"/style.css",  1, "text/css"},
};

void serve(const String &path) {
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
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  pinSet(PIN_NES_DATA_OUT, 0);

  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK), onClock, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);

  Serial.println("# ESP32-C3 gateway online. Send a path, e.g. /index.html");
}

void loop() {
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
