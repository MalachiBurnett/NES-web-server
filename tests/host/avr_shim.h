// Just enough of the Arduino AVR API to compile
// src/firmware/NES_router_mega/NES_router_mega.ino on a PC.  The port
// registers are test doubles with one "port" per Arduino pin, so every
// pin is bit 0 of its own byte, and the tests drive the firmware's
// interrupt handlers directly.  The ESP32 equivalent is arduino_shim.h.
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>

#define RAMEND 0x21FF              // an ATmega2560
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define FALLING 2

uint8_t g_pin_in[70];              // PINx, as the gateway reads it
uint8_t g_pin_out[70];             // PORTx, as the gateway drives it

static inline uint8_t digitalPinToPort(uint8_t pin) { return pin; }
static inline uint8_t digitalPinToBitMask(uint8_t) { return 1; }
static inline volatile uint8_t *portInputRegister(uint8_t port) { return (volatile uint8_t *)&g_pin_in[port]; }
static inline volatile uint8_t *portOutputRegister(uint8_t port) { return (volatile uint8_t *)&g_pin_out[port]; }

// Pins as the shared test models see them (link_tests.h, nes_sim.h).
static inline bool simInputLevel(int pin) { return g_pin_in[pin] & 1; }
static inline void simSetInput(int pin, bool level) { g_pin_in[pin] = level ? 1 : 0; }
static inline bool simGatewayOutput(int pin) { return g_pin_out[pin] & 1; }
// With the cable out, the internal pull-ups hold the gateway's inputs high.
static const bool SIM_INPUT_UNPLUGGED = true;

// Time is whatever the test says it is, exactly as in arduino_shim.h.
static uint32_t g_micros = 1000000;
static void (*g_delay_hook)(uint32_t us) = nullptr;
static inline uint32_t micros() { return g_micros; }
static inline void delayMicroseconds(uint32_t us) {
  if (g_delay_hook) g_delay_hook(us);
  else g_micros += us;
}
static inline void noInterrupts() {}
static inline void interrupts() {}
static inline void pinMode(uint8_t, uint8_t) {}
static inline int digitalPinToInterrupt(int p) { return p; }
static inline void attachInterrupt(int, void (*)(), int) {}

// Flash strings are plain strings on a PC.
struct __FlashStringHelper;
#define F(s) ((const __FlashStringHelper *)(s))
#define PSTR(s) (s)
#define snprintf_P snprintf

// Everything the firmware writes is kept in g_serial_out, which a test
// clears and inspects.  The most recent complete line is also kept in
// g_serial_last, and log lines ("# ...") are echoed unless g_serial_echo
// is off.  g_serial_in holds bytes for the firmware to read.
static std::string g_serial_out;
static std::string g_serial_in;
static bool g_serial_echo = true;
static char g_serial_last[256] = "";
static std::string g_serial_line;

struct SerialStub {
  void begin(unsigned long) {}

  size_t write(uint8_t b) {
    g_serial_out += (char)b;
    if (b == '\n') {
      if (!g_serial_line.empty() && g_serial_line.back() == '\r') g_serial_line.pop_back();
      snprintf(g_serial_last, sizeof g_serial_last, "%s", g_serial_line.c_str());
      if (g_serial_echo && g_serial_line[0] == '#') printf("    [serial] %s\n", g_serial_line.c_str());
      g_serial_line.clear();
    } else if (g_serial_line.size() < 1024) {
      g_serial_line += (char)b;
    }
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) write(buf[i]);
    return n;
  }

  size_t print(const char *s) { size_t n = 0; while (*s) n += write((uint8_t)*s++); return n; }
  size_t print(const __FlashStringHelper *s) { return print((const char *)s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned long v) { char b[16]; snprintf(b, sizeof b, "%lu", v); return print(b); }
  size_t print(long v) { char b[16]; snprintf(b, sizeof b, "%ld", v); return print(b); }
  size_t print(unsigned int v) { return print((unsigned long)v); }
  size_t print(int v) { return print((long)v); }

  size_t println() { return print("\r\n"); }
  size_t println(const char *s) { return print(s) + println(); }
  size_t println(const __FlashStringHelper *s) { return print(s) + println(); }

  int available() { return (int)g_serial_in.size(); }
  int read() {
    if (g_serial_in.empty()) return -1;
    uint8_t c = (uint8_t)g_serial_in[0];
    g_serial_in.erase(0, 1);
    return c;
  }
} Serial;
