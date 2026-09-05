#pragma once
// Test double for the ESP32 GPIO registers.
#include <stdint.h>
#define GPIO_IN_REG        0u
#define GPIO_OUT_W1TS_REG  1u
#define GPIO_OUT_W1TC_REG  2u
extern uint32_t g_gpio_in;
extern uint32_t g_gpio_out;
static inline uint32_t REG_READ(uint32_t reg) { return reg == GPIO_IN_REG ? g_gpio_in : g_gpio_out; }
static inline void REG_WRITE(uint32_t reg, uint32_t val) {
  if (reg == GPIO_OUT_W1TS_REG) g_gpio_out |= val;
  else if (reg == GPIO_OUT_W1TC_REG) g_gpio_out &= ~val;
}
