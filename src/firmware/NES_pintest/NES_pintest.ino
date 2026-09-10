// ===================================================================
//  NES WEB SERVER - pin test
//
//  The smallest possible check of the two NES -> gateway inputs.  No
//  protocol and no state machine: every free GPIO is a plain input,
//  and once a second this prints, for each one, what digitalRead()
//  says, the share of time GPIO_IN_REG saw it high, and how many
//  falling edges its interrupt counted.
//
//  Every free pin is watched, not just 3 and 4, so a CLK or OUT0 wire
//  that landed on the wrong header pin shows up where it really is.
//  With the web server ROM running, expect:
//
//    CLK    thousands of falling edges, high ~100%
//    OUT0   hundreds of falling edges, high ~85%
//
//  It also prints how GPIO 3 and 4 are actually configured, straight
//  from the IO_MUX and GPIO_ENABLE registers.  An input that is set up
//  right reads: function 1, input enable 1, pulls 0, output enable 0.
//
//  GPIO 6 holds D0 idle (the wire high) so the ROM stays cyan.
//
//  Serial commands:
//    drive   drive GPIO 3 and 4 high, low, high, low for 3s each and
//            read each level back, then return to inputs.  Proves the
//            pads themselves work, and gives a known voltage to find
//            them with a meter.  Run it with the NES off.
//    ?       this list
// ===================================================================

#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "driver/gpio.h"

#define PIN_NES_CLOCK     4
#define PIN_NES_DATA_IN   3
#define PIN_NES_DATA_OUT  6

// Every GPIO on the SuperMini header except D0 (6), USB (18/19) and
// UART0 (20/21).  Reading a strapping pin as an input is harmless.
const uint8_t watched[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10};
constexpr size_t NUM_WATCHED = sizeof watched / sizeof watched[0];

volatile uint32_t edges[NUM_WATCHED];
uint32_t highCount[NUM_WATCHED];
uint32_t samples = 0;
uint32_t lastReport = 0;

void IRAM_ATTR onEdge(void *arg) { edges[(uintptr_t)arg]++; }

void help() {
  Serial.println("# NES pin test. Commands: drive | ?");
  Serial.println("# With the web server ROM running, expect:");
  Serial.println("#   GPIO 4 (CLK)   thousands of edges/s, high ~100%");
  Serial.println("#   GPIO 3 (OUT0)  hundreds of edges/s,  high ~85%");
  Serial.println("# 'drive' needs the NES OFF; it checks the pads themselves.");
}

void watchInputs() {
  for (size_t i = 0; i < NUM_WATCHED; i++) {
    pinMode(watched[i], INPUT);
    attachInterruptArg(digitalPinToInterrupt(watched[i]), onEdge, (void *)(uintptr_t)i, FALLING);
  }
}

void resetCounts() {
  noInterrupts();
  for (size_t i = 0; i < NUM_WATCHED; i++) edges[i] = 0;
  interrupts();
  for (size_t i = 0; i < NUM_WATCHED; i++) highCount[i] = 0;
  samples = 0;
  lastReport = millis();
}

const char *label(uint8_t pin) {
  switch (pin) {
    case PIN_NES_DATA_IN: return "  <- OUT0 should be here";
    case PIN_NES_CLOCK:   return "  <- CLK should be here";
    case 8:               return "  (onboard LED)";
    case 9:               return "  (BOOT button)";
    default:              return "";
  }
}

void printConfig(uint8_t pin, uint32_t muxReg) {
  uint32_t mux = REG_READ(muxReg);
  Serial.printf("GPIO %u config: function %lu (1 = GPIO), input enable %lu, pull-up %lu, pull-down %lu, output enable %lu\n",
                pin,
                (unsigned long)((mux >> MCU_SEL_S) & MCU_SEL),
                (unsigned long)((mux >> FUN_IE_S) & 1),
                (unsigned long)((mux >> FUN_PU_S) & 1),
                (unsigned long)((mux >> FUN_PD_S) & 1),
                (unsigned long)((REG_READ(GPIO_ENABLE_REG) >> pin) & 1));
}

void report() {
  uint32_t counts[NUM_WATCHED];
  noInterrupts();
  for (size_t i = 0; i < NUM_WATCHED; i++) {
    counts[i] = edges[i];
    edges[i] = 0;
  }
  interrupts();

  Serial.println("GPIO  digitalRead  high   falling edges/s");
  for (size_t i = 0; i < NUM_WATCHED; i++) {
    uint8_t pin = watched[i];
    unsigned long pct = samples ? (unsigned long)((uint64_t)highCount[i] * 100 / samples) : 0;
    Serial.printf("%4u  %11d  %3lu%%  %8lu%s\n", pin, digitalRead(pin), pct,
                  (unsigned long)counts[i], label(pin));
    highCount[i] = 0;
  }
  samples = 0;
  printConfig(PIN_NES_DATA_IN, IO_MUX_GPIO3_REG);
  printConfig(PIN_NES_CLOCK, IO_MUX_GPIO4_REG);
  Serial.println();
}

// Drives GPIO 3 and 4 as outputs and reads each level back through the
// input path (Arduino's OUTPUT leaves input enable on).  The weakest
// drive strength is used, so a pad shorted to ground is not stressed;
// it still pulls a 10k/20k divider to the rail with room to spare.
// A pad that reads back what it was driven to is alive.  One that does
// not is being held by something outside the chip.
void driveTest() {
  const uint8_t pins[] = {PIN_NES_DATA_IN, PIN_NES_CLOCK};
  for (uint8_t pin : pins) {
    detachInterrupt(digitalPinToInterrupt(pin));
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

  watchInputs();
  resetCounts();
  Serial.println("# drive test done, back to inputs");
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);   // do not stall when no host is attached
#endif

  // D0 idle, the wire high, exactly as the gateway holds it.
  REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << PIN_NES_DATA_OUT);
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << PIN_NES_DATA_OUT);

  watchInputs();
  help();
  resetCounts();
}

void loop() {
  uint32_t in = REG_READ(GPIO_IN_REG);
  for (size_t i = 0; i < NUM_WATCHED; i++) highCount[i] += (in >> watched[i]) & 1;
  samples++;

  if (Serial.available()) {
    String c = Serial.readStringUntil('\n');
    c.trim();
    if (c == "drive") driveTest();
    else if (c.length()) help();
  }

  if (millis() - lastReport >= 1000) {
    lastReport = millis();
    report();
  }
}
