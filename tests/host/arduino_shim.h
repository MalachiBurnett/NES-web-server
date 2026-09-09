// Just enough of the Arduino API to compile src/firmware/NES_router/NES_router.ino
// on a PC. The GPIO registers are test doubles (see soc/gpio_reg.h) so
// the tests can drive the firmware's interrupt handlers directly.
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>

uint32_t g_gpio_in = 0;
uint32_t g_gpio_out = 0;

#define IRAM_ATTR
#define INPUT 0
#define OUTPUT 1
#define FALLING 2

static uint32_t g_micros = 0;
static inline uint32_t micros() { return g_micros += 10; }
static inline void delayMicroseconds(uint32_t) {}
static inline void noInterrupts() {}
static inline void interrupts() {}
static inline void pinMode(int, int) {}
static inline int digitalPinToInterrupt(int p) { return p; }
static inline void attachInterrupt(int, void (*)(), int) {}

struct String {
  std::string s;
  String() {}
  String(const char *p) : s(p) {}
  String(const std::string &p) : s(p) {}
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
  }
  size_t length() const { return s.size(); }
  int indexOf(char c, int from = 0) const {
    size_t p = s.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  String substring(int a) const { return String(s.substr(a)); }
  String substring(int a, int b) const { return String(s.substr(a, b - a)); }
  bool operator==(const char *o) const { return s == o; }
};

struct SerialStub {
  void begin(int) {}
  void println(const char *m) { ::printf("    [serial] %s\n", m); }
  void print(const char *m) { (void)m; }
  void printf(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    ::printf("    [serial] ");
    vprintf(f, ap);
    va_end(ap);
  }
  void write(const uint8_t *, size_t) {}
  int available() { return 0; }
  String readStringUntil(char) { return String(); }
} Serial;
