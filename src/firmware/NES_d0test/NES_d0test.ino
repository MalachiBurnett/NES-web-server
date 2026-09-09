// ===================================================================
//  NES WEB SERVER - D0 line test, the smallest useful gateway
//
//  Companion to src/nes/d0test.asm.  Does one thing: drives the data
//  line to the NES so you can watch the screen follow it.
//
//    1     drive D0 high   -> NES screen should go GREEN
//    0     drive D0 low    -> NES screen should go RED
//    sq    1Hz square wave -> screen should alternate
//    ?     this list
//
//  Prints a status line every 2s.  clkLvl and inLvl are the two NES
//  input pins, here as a sanity check.  The ROM never strobes OUT0,
//  so inLvl going high while d0 is high means pins 3 and 4 are
//  shorted.  Both reading low together is normal and means nothing.
//
//  Own folder on purpose: Arduino concatenates every .ino in a sketch
//  folder, so this cannot sit beside the other sketches.
//
//  Port 1 wiring, unchanged:
//    CLK      (pin 2) -> GPIO 4   via 10k/20k divider
//    DATA_IN  (pin 3) -> GPIO 3   via 10k/20k divider   (OUT0)
//    DATA_OUT (pin 4) <- GPIO 6   via 100R series       (D0)
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

bool     square     = false;
bool     squareHigh = false;
bool     level      = false;
uint32_t lastStatus = 0;
uint32_t lastToggle = 0;

void help() {
  Serial.println("# 1 = D0 high (screen GREEN) | 0 = D0 low (screen RED)");
  Serial.println("# sq = 1Hz square | ? = this list");
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  pinMode(PIN_NES_DATA_IN,  INPUT);
  pinMode(PIN_NES_CLOCK,    INPUT);
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  pinSet(PIN_NES_DATA_OUT, 0);

  Serial.println("# D0 test online. D0 low, NES should be RED.");
  help();
}

void apply(bool high) {
  level = high;
  pinSet(PIN_NES_DATA_OUT, high);
}

void loop() {
  if (Serial.available()) {
    String c = Serial.readStringUntil('\n');
    c.trim();
    if (c == "1")       { square = false; apply(true);  Serial.println("# D0 HIGH  -> expect GREEN"); }
    else if (c == "0")  { square = false; apply(false); Serial.println("# D0 LOW   -> expect RED"); }
    else if (c == "sq") { square = true;                Serial.println("# D0 square -> expect alternating"); }
    else if (c == "?")  { help(); }
    else if (c.length()) { Serial.printf("# unknown: %s\n", c.c_str()); }
  }

  uint32_t now = millis();

  if (square && now - lastToggle >= SQUARE_HALF_MS) {
    lastToggle = now;
    squareHigh = !squareHigh;
    apply(squareHigh);
  }

  if (now - lastStatus >= STATUS_MS) {
    lastStatus = now;
    Serial.printf("d0=%d (%s) | clkLvl=%d inLvl=%d%s\n",
                  level ? 1 : 0,
                  level ? "expect GREEN" : "expect RED",
                  pinLevel(PIN_NES_CLOCK)   ? 1 : 0,
                  pinLevel(PIN_NES_DATA_IN) ? 1 : 0,
                  (level && pinLevel(PIN_NES_DATA_IN)) ? "   <-- SHORT: inLvl high because d0 is" : "");
  }
}
