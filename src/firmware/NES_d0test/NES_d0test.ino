// ===================================================================
//  NES WEB SERVER - D0 line test, the smallest useful gateway
//
//  Companion to src/nes/d0test.asm.  Does one thing: drives the data
//  line to the NES so you can watch the screen follow it.
//
//    1     NES reads 1 (wire low)   -> screen should go GREEN
//    0     NES reads 0 (wire high)  -> screen should go RED
//    sq    1Hz square wave          -> screen should alternate
//    ?     this list
//
//  The console inverts D0, as it does for a real pad: a pressed button
//  pulls the wire low and the game reads 1.  This starts at 0, wire
//  high, so with it plugged in the screen should be red and a flash
//  cart menu should behave as though nothing were plugged in at all.
//
//  Prints a status line every 2s with the physical wire level, so a
//  multimeter on NES pin 4 can be checked against it: wire=high should
//  measure about 3.3V, wire=low about 0V.
//
//  Own folder on purpose: Arduino concatenates every .ino in a sketch
//  folder, so this cannot sit beside the other sketches.
//
//  Port 1 wiring:
//    CLK      (pin 2) -> GPIO 4   via 10k/20k divider
//    DATA_IN  (pin 3) -> GPIO 3   via 10k/20k divider   (OUT0)
//    DATA_OUT (pin 4) <- GPIO 6   via 1k series         (D0)
// ===================================================================

#include "soc/gpio_reg.h"

#define PIN_NES_CLOCK     4
#define PIN_NES_DATA_IN   3
#define PIN_NES_DATA_OUT  6

#define STATUS_MS       2000
#define SQUARE_HALF_MS   500

static inline bool pinLevel(uint8_t pin) {
  return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}
static inline void pinSet(uint8_t pin, bool level) {
  REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << pin);
}

// The only place that knows the wire is upside down.
static inline void d0Write(bool nesReads) {
  pinSet(PIN_NES_DATA_OUT, !nesReads);
}

bool     square     = false;
bool     squareHigh = false;
bool     reads      = false;     // what the NES reads, not the wire level
uint32_t lastStatus = 0;
uint32_t lastToggle = 0;

void help() {
  Serial.println("# 1 = NES reads 1, screen GREEN | 0 = NES reads 0, screen RED");
  Serial.println("# sq = 1Hz square | ? = this list");
}

void apply(bool nesReads) {
  reads = nesReads;
  d0Write(nesReads);
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  pinMode(PIN_NES_DATA_IN, INPUT);
  pinMode(PIN_NES_CLOCK,   INPUT);
  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  apply(false);

  Serial.println("# D0 test online. NES reads 0 (wire high), screen should be RED.");
  help();
}

void loop() {
  if (Serial.available()) {
    String c = Serial.readStringUntil('\n');
    c.trim();
    if (c == "1")         { square = false; apply(true);  Serial.println("# NES reads 1 (wire low)  -> expect GREEN"); }
    else if (c == "0")    { square = false; apply(false); Serial.println("# NES reads 0 (wire high) -> expect RED"); }
    else if (c == "sq")   { square = true;                Serial.println("# square wave -> expect alternating"); }
    else if (c == "?")    { help(); }
    else if (c.length())  { Serial.printf("# unknown: %s\n", c.c_str()); }
  }

  uint32_t now = millis();

  if (square && now - lastToggle >= SQUARE_HALF_MS) {
    lastToggle = now;
    squareHigh = !squareHigh;
    apply(squareHigh);
  }

  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("NES reads %d (expect %s) | wire=%s | clkLvl=%d inLvl=%d\n",
                  reads ? 1 : 0,
                  reads ? "GREEN" : "RED",
                  reads ? "low" : "high",
                  pinLevel(PIN_NES_CLOCK)   ? 1 : 0,
                  pinLevel(PIN_NES_DATA_IN) ? 1 : 0);
  }
}
