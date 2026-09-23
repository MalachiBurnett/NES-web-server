// ===================================================================
//  NES WEB SERVER - link layer bring-up tester
//
//  Companion to src/nes/debug.asm (docs/bring-up.md).  This is NOT the
//  gateway: it does nothing but measure the three controller port
//  lines, so each can be proved or ruled out on its own.  The board is
//  whichever you compile for, and the wiring is the gateway's - see the
//  build options in src/firmware/NES_router/NES_router.ino, which this
//  file's must match.
//
//  Prints a summary twice a second: edges counted since the last line,
//  and the share of that time each input spent high.  Counters are per
//  window, not cumulative, so a burst shows up as one big number and
//  then drops back to zero.
//
//  It works with the web server ROM too, as a quick wiring check.  A
//  sound cable reads about clk=1800 out0=200 clkHigh=100% out0High=85%
//  per line.
//
//  Serial commands:
//    d0 0     NES reads 0     - ROM phase 1 should show solid red
//    d0 1     NES reads 1     - ROM phase 1 should show solid green
//    d0 sq    1Hz square      - ROM phase 1 should alternate cleanly
//    zero     reset counters
//    pins     watch every free pin for a second: finds a wire that
//             landed somewhere other than where it should have
//    drive    (ESP32) drive the CLK and OUT0 pads and read them back
//    ?        command list and what to expect per phase
//
//  The console inverts D0, so "NES reads 1" means the wire is low.
//  The tester starts at NES reads 0, wire high.
//
//  With the D0 test ROM (build_test_roms.py d0) only the d0 commands
//  matter: the screen is red for 0 and green for 1.
// ===================================================================

// --- Build options: keep in step with NES_router.ino ---------------
//   --build-property "compiler.cpp.extra_flags=-DNES_WIRING_RJ45"
//
// #define NES_WIRING_RJ45

#if defined(ARDUINO_ARCH_ESP32)
  #include "soc/gpio_reg.h"
  #include "soc/io_mux_reg.h"
  #include "soc/gpio_periph.h"
  #include "driver/gpio.h"
  #define BOARD_NAME        "ESP32-C3"
  #define PIN_WORD          "GPIO"
  #define PIN_NES_CLOCK     4
  #define PIN_NES_DATA_IN   3
  #define PIN_NES_DATA_OUT  6
  #define NES_INPUT_MODE    INPUT
  #define OUT0_INTERRUPT    1
  #define CLOCK_PCINT       0
  // Every GPIO on the SuperMini header except D0 (6), USB (18/19) and
  // UART0 (20/21).  Reading a strapping pin as an input is harmless.
  const uint8_t watched[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10};
#elif defined(__AVR_ATmega2560__)
  #define BOARD_NAME        "Arduino Mega"
  #define PIN_WORD          "pin"
  #define NES_INPUT_MODE    INPUT_PULLUP
  #define IRAM_ATTR
  #if defined(NES_WIRING_RJ45)
    #define PIN_NES_CLOCK     12         // PB6, PCINT6
    #define PIN_NES_DATA_IN   9          // PH6, no interrupt
    #define PIN_NES_DATA_OUT  13         // PB7, also the L LED
    #define OUT0_INTERRUPT    0
    #define CLOCK_PCINT       1
    #define CLOCK_PCMSK_BIT   PCINT6
    const uint8_t watched[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  #else
    #define PIN_NES_CLOCK     2          // INT4
    #define PIN_NES_DATA_IN   3          // INT5
    #define PIN_NES_DATA_OUT  4
    #define OUT0_INTERRUPT    1
    #define CLOCK_PCINT       0
    const uint8_t watched[] = {2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  #endif
#else
  #error "NES_debug is for an ESP32-C3 or an Arduino Mega 2560"
#endif

#define REPORT_MS       500
#define SQUARE_HALF_MS  500     // 500ms per half = 1Hz square
constexpr size_t NUM_WATCHED = sizeof watched / sizeof watched[0];

// --- Port access ----------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)
static inline bool pinLevel(uint8_t pin) { return (REG_READ(GPIO_IN_REG) >> pin) & 1; }
static inline void d0Wire(bool high) {
  REG_WRITE(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << PIN_NES_DATA_OUT);
}
#else
static inline bool pinLevel(uint8_t pin) {
  return *portInputRegister(digitalPinToPort(pin)) & digitalPinToBitMask(pin);
}
static inline void d0Wire(bool high) { digitalWrite(PIN_NES_DATA_OUT, high ? HIGH : LOW); }
#endif

// D0 is inverted by the console: wire low reads as 1.
static inline void d0Write(bool nesReads) { d0Wire(!nesReads); }

// --- Edge counting ----------------------------------------------------
// Counted unconditionally - no state machine, nothing to get stuck in.
// OUT0 with no interrupt is counted from loop()'s samples instead: its
// levels last milliseconds, so none are missed.
volatile uint32_t clkEdges  = 0;
volatile uint32_t out0Edges = 0;

void IRAM_ATTR onClock()  { clkEdges++; }
void IRAM_ATTR onStrobe() { out0Edges++; }

#if CLOCK_PCINT
// Fires on both edges.  Wait out the rising one, then clear the flag it
// set, so one pulse is one count.  See NES_router.ino.
ISR(PCINT0_vect) {
  for (uint8_t n = 0; n < 255 && !pinLevel(PIN_NES_CLOCK); n++) {}
  __builtin_avr_delay_cycles(6);
  PCIFR = _BV(PCIF0);
  clkEdges++;
}
#endif

void watchNesPins() {
  pinMode(PIN_NES_DATA_IN, NES_INPUT_MODE);
  pinMode(PIN_NES_CLOCK,   NES_INPUT_MODE);
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
}

// A clock pulse is over before its interrupt handler returns, so while
// the handlers are live nothing else can ever see CLK low.
void unwatchNesPins() {
#if CLOCK_PCINT
  PCICR &= ~_BV(PCIE0);
#else
  detachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK));
#endif
#if OUT0_INTERRUPT
  detachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN));
#endif
}

enum D0Mode { D0_LOW, D0_HIGH, D0_SQUARE };
D0Mode   d0Mode     = D0_LOW;
bool     squareHigh = false;
uint32_t lastReport = 0;
uint32_t lastToggle = 0;

// Level samples taken by loop() since the last report.
uint32_t samples = 0, clkHigh = 0, out0High = 0;
bool out0Was = true;

const char *d0Name() {
  if (d0Mode == D0_LOW)  return "0 (wire high)";
  if (d0Mode == D0_HIGH) return "1 (wire low)";
  return "sq";
}

void help() {
  Serial.println(F("# commands: d0 0 | d0 1 | d0 sq | zero | pins"
#if defined(ARDUINO_ARCH_ESP32)
                   " | drive"
#endif
                   " | ?"));
  Serial.println(F("# expected per ROM phase (PAL figures):"));
  Serial.println(F("#   1 D0 SENSE  red/green   clk ~25/window   out0 0"));
  Serial.println(F("#   2 OUT0      blue        clk 0            out0 ~3/window"));
  Serial.println(F("#   3 CLK FAST  yellow      clk 1000 total   out0 0"));
  Serial.println(F("#   4 CLK SLOW  orange      clk 1000 total   out0 0"));
  Serial.println(F("#   5 IDLE      black       clk 0            out0 0"));
  Serial.println(F("# with the web server ROM: clk ~1800, out0 ~200, clkHigh ~100%, out0High ~85%"));
}

// --- pins: where did the wires land? --------------------------------
// Every free pin is sampled as fast as the board allows for a second.
// OUT0 shows up as a few hundred falls a second and high ~85%.  CLK is
// high ~100% with falls; its pulses are shorter than a Mega's trip
// round this loop, so there it catches only some of them - but any at
// all is unmistakable next to a pin with nothing on it.
//
// To keep the loop tight, each sample is every watched pin at once, as
// one word: GPIO_IN_REG on the ESP32, and up to four port registers
// side by side on the Mega.
#if defined(ARDUINO_ARCH_ESP32)
static inline uint32_t sampleAll() { return REG_READ(GPIO_IN_REG); }
static uint32_t pinBit(uint8_t pin) { return 1UL << pin; }
static void sampleSetup() {}
#else
volatile uint8_t *samplePorts[4];
uint8_t numSamplePorts = 0;

static uint32_t pinBit(uint8_t pin) {
  volatile uint8_t *reg = portInputRegister(digitalPinToPort(pin));
  uint8_t slot = 0;
  while (slot < numSamplePorts && samplePorts[slot] != reg) slot++;
  if (slot == numSamplePorts) samplePorts[numSamplePorts++] = reg;   // pins 2-13 span four
  return (uint32_t)digitalPinToBitMask(pin) << (8 * slot);
}
static void sampleSetup() {
  numSamplePorts = 0;
  samplePorts[0] = portInputRegister(digitalPinToPort(watched[0]));
  for (uint8_t i = 1; i < 4; i++) samplePorts[i] = samplePorts[0];
}
static inline uint32_t sampleAll() {
  return *samplePorts[0] | ((uint32_t)*samplePorts[1] << 8) |
         ((uint32_t)*samplePorts[2] << 16) | ((uint32_t)*samplePorts[3] << 24);
}
#endif

const char *pinLabel(uint8_t pin) {
  if (pin == PIN_NES_DATA_IN) return "  <- OUT0 should be here";
  if (pin == PIN_NES_CLOCK)   return "  <- CLK should be here";
#if defined(ARDUINO_ARCH_ESP32)
  if (pin == 8) return "  (onboard LED)";
  if (pin == 9) return "  (BOOT button)";
#endif
  return "";
}

void scanPins() {
  for (uint8_t pin : watched)
    if (pin != PIN_NES_CLOCK && pin != PIN_NES_DATA_IN) pinMode(pin, NES_INPUT_MODE);

  unwatchNesPins();
  sampleSetup();
  uint32_t bits[NUM_WATCHED], high[NUM_WATCHED] = {0}, falls[NUM_WATCHED] = {0};
  for (size_t i = 0; i < NUM_WATCHED; i++) bits[i] = pinBit(watched[i]);

  // Falls are looked for in every sample; levels, and the time, only in
  // one in 64, which is plenty for a percentage.
  uint32_t n = 0, levelSamples = 0;
  uint32_t was = sampleAll();
  uint32_t start = micros();
  for (;;) {
    uint32_t now = sampleAll();
    uint32_t fell = was & ~now;
    was = now;
    if (fell)
      for (size_t i = 0; i < NUM_WATCHED; i++)
        if (fell & bits[i]) falls[i]++;
    if ((++n & 63) == 0) {
      for (size_t i = 0; i < NUM_WATCHED; i++)
        if (now & bits[i]) high[i]++;
      levelSamples++;
      if (micros() - start >= 1000000UL) break;
    }
  }
  watchNesPins();

  char line[96];
  Serial.println(F("# " PIN_WORD "  now  high   falls/s"));
  for (size_t i = 0; i < NUM_WATCHED; i++) {
    snprintf_P(line, sizeof line, PSTR("# %4u  %3d  %3lu%%  %8lu%s"), watched[i],
               pinLevel(watched[i]) ? 1 : 0, (unsigned long)(high[i] * 100 / levelSamples),
               (unsigned long)falls[i], pinLabel(watched[i]));
    Serial.println(line);
  }
#if defined(ARDUINO_ARCH_ESP32)
  // An input that is set up right reads function 1, input enable 1,
  // pulls 0, output enable 0.
  for (uint8_t pin : {(uint8_t)PIN_NES_DATA_IN, (uint8_t)PIN_NES_CLOCK}) {
    uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[pin]);
    snprintf(line, sizeof line, "# GPIO %u: function %lu, input enable %lu, pull-up %lu, pull-down %lu, output enable %lu",
             pin, (unsigned long)((mux >> MCU_SEL_S) & MCU_SEL), (unsigned long)((mux >> FUN_IE_S) & 1),
             (unsigned long)((mux >> FUN_PU_S) & 1), (unsigned long)((mux >> FUN_PD_S) & 1),
             (unsigned long)((REG_READ(GPIO_ENABLE_REG) >> pin) & 1));
    Serial.println(line);
  }
#endif
  snprintf_P(line, sizeof line, PSTR("# %lu samples, one every %lu ns"),
             (unsigned long)n, (unsigned long)(1000000000UL / n));
  Serial.println(line);
}

#if defined(ARDUINO_ARCH_ESP32)
// Drives the CLK and OUT0 pads as outputs and reads each level back
// through the input path (Arduino's OUTPUT leaves input enable on).  The
// weakest drive strength is used, so a pad shorted to ground is not
// stressed; it still pulls a 10k/20k divider to the rail with room to
// spare.  A pad that reads back what it was driven to is alive.  One
// that does not is being held by something outside the chip.
void driveTest() {
  const uint8_t pins[] = {PIN_NES_DATA_IN, PIN_NES_CLOCK};
  unwatchNesPins();
  for (uint8_t pin : pins) {
    pinMode(pin, OUTPUT);
    gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);
  }

  Serial.println("# drive test, NES should be OFF. Meter each pad against GND.");
  for (int step = 0; step < 4; step++) {
    bool level = (step % 2) == 0;
    for (uint8_t pin : pins) digitalWrite(pin, level);
    delay(20);
    uint32_t in = REG_READ(GPIO_IN_REG);
    for (uint8_t pin : pins) {
      bool back = (in >> pin) & 1;
      Serial.printf("#   GPIO %u driven %s, reads back %d%s\n", pin,
                    level ? "HIGH (meter 3.3V)" : "LOW  (meter 0V)  ", back ? 1 : 0,
                    back == level ? "" : "  <-- MISMATCH: something outside the chip holds this pad");
    }
    delay(3000);
  }

  watchNesPins();
  Serial.println("# drive test done, back to inputs");
}
#endif

void handleCommand(const char *c) {
  if (c[0] == '\0') return;

  if (strcmp(c, "d0 0") == 0) {
    d0Mode = D0_LOW;
    Serial.println(F("# NES reads 0 (wire high)"));
  } else if (strcmp(c, "d0 1") == 0) {
    d0Mode = D0_HIGH;
    Serial.println(F("# NES reads 1 (wire low)"));
  } else if (strcmp(c, "d0 sq") == 0) {
    d0Mode = D0_SQUARE;
    Serial.println(F("# D0 1Hz square"));
  } else if (strcmp(c, "zero") == 0) {
    noInterrupts();
    clkEdges = 0;
    out0Edges = 0;
    interrupts();
    Serial.println(F("# counters zeroed"));
  } else if (strcmp(c, "pins") == 0) {
    scanPins();
#if defined(ARDUINO_ARCH_ESP32)
  } else if (strcmp(c, "drive") == 0) {
    driveTest();
#endif
  } else if (strcmp(c, "?") == 0) {
    help();
  } else {
    Serial.print(F("# unknown command: "));
    Serial.println(c);
  }
}

void setup() {
  Serial.begin(250000);   // as NES_router.ino, for the same reason
#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  d0Write(false);                     // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);
  watchNesPins();
  out0Was = pinLevel(PIN_NES_DATA_IN);

  Serial.print(F("# " BOARD_NAME " link tester online. CLK on " PIN_WORD " "));
  Serial.print(PIN_NES_CLOCK);
  Serial.print(F(", OUT0 on "));
  Serial.print(PIN_NES_DATA_IN);
  Serial.print(F(", D0 on "));
  Serial.print(PIN_NES_DATA_OUT);
  Serial.println(F(". NES reads 0 (wire high)."));
  help();
}

void loop() {
  static char cmd[16];
  static uint8_t cmdLen = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmd[cmdLen] = '\0';
      cmdLen = 0;
      handleCommand(cmd);
    } else if (cmdLen < sizeof cmd - 1) {
      cmd[cmdLen++] = c;
    }
  }

  bool out0 = pinLevel(PIN_NES_DATA_IN);
#if !OUT0_INTERRUPT
  if (out0Was && !out0) out0Edges++;
#endif
  out0Was = out0;
  samples++;
  if (pinLevel(PIN_NES_CLOCK)) clkHigh++;
  if (out0) out0High++;

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

    char line[96];
    snprintf_P(line, sizeof line, PSTR("clk=%-6lu out0=%-6lu | clkHigh=%3lu%% out0High=%3lu%% d0=%s"),
               (unsigned long)c, (unsigned long)o,
               (unsigned long)(clkHigh * 100 / samples), (unsigned long)(out0High * 100 / samples),
               d0Name());
    Serial.println(line);
    samples = clkHigh = out0High = 0;
  }
}
