// ===================================================================
//  NES WEB SERVER - link layer bring-up tester, Arduino Mega 2560
//
//  The Mega twin of src/firmware/NES_debug, and like it a companion to
//  src/nes/debug.asm (docs/bring-up.md).  This is NOT the gateway: it
//  does nothing but measure the three controller port lines, so each
//  can be proved or ruled out on its own.
//
//  Wiring is the Mega gateway's (docs/mega.md) - controller port 1:
//    CLK   (port pin 2) -> pin 2   via 1k
//    OUT0  (port pin 3) -> pin 3   via 1k
//    D0    (port pin 4) <- pin 4   via 1k
//    GND   (port pin 1) -- GND
//
//  Prints a summary twice a second: edges counted since the last line,
//  and the share of that time each input spent high.  Counters are per
//  window, not cumulative, so a burst shows up as one big number and
//  then drops back to zero.
//
//  It works with the web server ROM too, as a quick wiring check.  A
//  sound cable reads about clk=1800 out0=200 clkHigh=100% out0High=85%
//  per line; a wire making no contact reads 0 edges and 100% high,
//  because the pull-up holds it there.
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

#define PIN_NES_CLOCK     2     // INT4
#define PIN_NES_DATA_IN   3     // INT5
#define PIN_NES_DATA_OUT  4

#define REPORT_MS       500
#define SQUARE_HALF_MS  500     // 500ms per half = 1Hz square

// Counted unconditionally - no state machine, nothing to get stuck in.
volatile uint32_t clkEdges  = 0;
volatile uint32_t out0Edges = 0;

void onClock()  { clkEdges++; }
void onStrobe() { out0Edges++; }

// D0 is inverted by the console: wire low reads as 1.
void d0Write(bool nesReads) {
  digitalWrite(PIN_NES_DATA_OUT, nesReads ? LOW : HIGH);
}

enum D0Mode { D0_LOW, D0_HIGH, D0_SQUARE };
D0Mode   d0Mode     = D0_LOW;
bool     squareHigh = false;
uint32_t lastReport = 0;
uint32_t lastToggle = 0;

// Level samples taken by loop() since the last report.
uint32_t samples = 0, clkHigh = 0, out0High = 0;

const char *d0Name() {
  if (d0Mode == D0_LOW)  return "0";
  if (d0Mode == D0_HIGH) return "1";
  return "sq";
}

void help() {
  Serial.println(F("# commands: d0 0 | d0 1 | d0 sq | zero | ?"));
  Serial.println(F("# expected per ROM phase (PAL figures):"));
  Serial.println(F("#   1 D0 SENSE  red/green   clk ~25/window   out0 0"));
  Serial.println(F("#   2 OUT0      blue        clk 0            out0 ~3/window"));
  Serial.println(F("#   3 CLK FAST  yellow      clk 1000 total   out0 0"));
  Serial.println(F("#   4 CLK SLOW  orange      clk 1000 total   out0 0"));
  Serial.println(F("#   5 IDLE      black       clk 0            out0 0"));
  Serial.println(F("# with the web server ROM: clk ~1800, out0 ~200, clkHigh ~100%, out0High ~85%"));
}

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
  } else if (strcmp(c, "?") == 0) {
    help();
  } else {
    Serial.print(F("# unknown command: "));
    Serial.println(c);
  }
}

void setup() {
  Serial.begin(115200);

  // Pull-ups, so a wire making no contact reads high and quiet rather
  // than floating and counting noise.
  pinMode(PIN_NES_DATA_IN, INPUT_PULLUP);
  pinMode(PIN_NES_CLOCK,   INPUT_PULLUP);
  digitalWrite(PIN_NES_DATA_OUT, HIGH);   // latch idle before the driver turns on
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  d0Write(false);

  attachInterrupt(digitalPinToInterrupt(PIN_NES_CLOCK),   onClock,  FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_NES_DATA_IN), onStrobe, FALLING);

  Serial.println(F("# Arduino Mega link tester online. NES reads 0 (wire high)."));
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

  samples++;
  if (digitalRead(PIN_NES_CLOCK))   clkHigh++;
  if (digitalRead(PIN_NES_DATA_IN)) out0High++;

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
