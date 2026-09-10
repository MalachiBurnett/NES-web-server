// ===================================================================
//  NES WEB SERVER - link layer bring-up tester
//
//  Companion to src/nes/debug.asm.  This is NOT the gateway: it does
//  nothing but measure the three controller port lines, so each can
//  be proved or ruled out on its own.
//
//  It lives in its own folder on purpose.  Arduino concatenates every
//  .ino in a sketch folder into one file, so this cannot sit next to
//  NES_router.ino without colliding with its setup()/loop().
//
//  Wiring is unchanged - controller port 1:
//    CLK      (port pin 2) -> GPIO 4   via 10k/20k divider
//    DATA_IN  (port pin 3) -> GPIO 3   via 10k/20k divider   (OUT0)
//    DATA_OUT (port pin 4) <- GPIO 6   via 1k series         (D0)
//
//  Prints a counter summary twice a second.  Counters are per window,
//  not cumulative, so a burst shows up as one big number and then
//  drops back to zero.
//
//  Serial commands:
//    d0 0     NES reads 0     - ROM phase 1 should show solid red
//    d0 1     NES reads 1     - ROM phase 1 should show solid green
//    d0 sq    1Hz square      - ROM phase 1 should alternate cleanly
//    zero     reset counters
//    ?        command list and what to expect per phase
//
//  The console inverts D0, so "NES reads 1" means the wire is low.
//  The tester starts at NES reads 0, wire high.
// ===================================================================

#include "soc/gpio_reg.h"

#define PIN_NES_CLOCK     4
#define PIN_NES_DATA_IN   3
#define PIN_NES_DATA_OUT  6

#define REPORT_MS       500
#define SQUARE_HALF_MS  500     // 500ms per half = 1Hz square

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

// Counted unconditionally - no state machine, nothing to get stuck in.
volatile uint32_t clkEdges  = 0;
volatile uint32_t out0Edges = 0;

void IRAM_ATTR onClock()  { clkEdges++; }
void IRAM_ATTR onStrobe() { out0Edges++; }

enum D0Mode { D0_LOW, D0_HIGH, D0_SQUARE };
D0Mode   d0Mode     = D0_LOW;
bool     squareHigh = false;
uint32_t lastReport = 0;
uint32_t lastToggle = 0;

const char *d0Name() {
  if (d0Mode == D0_LOW)  return "0";
  if (d0Mode == D0_HIGH) return "1";
  return "sq";
}

void help() {
  Serial.println("# commands: d0 0 | d0 1 | d0 sq | zero | ?");
  Serial.println("# expected per ROM phase (PAL figures):");
  Serial.println("#   1 D0 SENSE  red/green   clk ~25/window   out0 0");
  Serial.println("#   2 OUT0      blue        clk 0            out0 ~3/window");
  Serial.println("#   3 CLK FAST  yellow      clk 1000 total   out0 0");
  Serial.println("#   4 CLK SLOW  orange      clk 1000 total   out0 0");
  Serial.println("#   5 IDLE      black       clk 0            out0 0");
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  pinMode(PIN_NES_DATA_IN,  INPUT);
  pinMode(PIN_NES_CLOCK,    INPUT);
  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);

  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK),   onClock,  FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);

  Serial.println("# ESP32-C3 link tester online. NES reads 0 (wire high).");
  help();
}

void handleCommand(String c) {
  c.trim();
  if (c.length() == 0) return;

  if (c == "d0 0") {
    d0Mode = D0_LOW;
    Serial.println("# NES reads 0 (wire high)");
  } else if (c == "d0 1") {
    d0Mode = D0_HIGH;
    Serial.println("# NES reads 1 (wire low)");
  } else if (c == "d0 sq") {
    d0Mode = D0_SQUARE;
    Serial.println("# D0 1Hz square");
  } else if (c == "zero") {
    noInterrupts();
    clkEdges = 0;
    out0Edges = 0;
    interrupts();
    Serial.println("# counters zeroed");
  } else if (c == "?") {
    help();
  } else {
    Serial.printf("# unknown command: %s\n", c.c_str());
  }
}

void loop() {
  if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));

  uint32_t now = millis();

  // --- drive D0 ---
  if (d0Mode == D0_LOW) {
    d0Write(false);
  } else if (d0Mode == D0_HIGH) {
    d0Write(true);
  } else if (now - lastToggle >= SQUARE_HALF_MS) {
    lastToggle = now;
    squareHigh = !squareHigh;
    d0Write(squareHigh);
  }

  // --- report ---
  if (now - lastReport >= REPORT_MS) {
    lastReport = now;
    noInterrupts();
    uint32_t c = clkEdges, o = out0Edges;
    clkEdges = 0;
    out0Edges = 0;
    interrupts();

    Serial.printf("clk=%-6lu out0=%-6lu | clkLvl=%d out0Lvl=%d d0=%s\n",
                  (unsigned long)c, (unsigned long)o,
                  pinLevel(PIN_NES_CLOCK)   ? 1 : 0,
                  pinLevel(PIN_NES_DATA_IN) ? 1 : 0,
                  d0Name());
  }
}
