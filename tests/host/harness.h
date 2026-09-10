// Shared by the host tests: pass/fail reporting, and finding the page
// packets in a ROM image.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;

static inline void check(bool ok, const char *what) {
  printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

static inline int finish() {
  printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
  return failures ? 1 : 0;
}

static inline std::vector<uint8_t> readFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> v(n);
  if (fread(v.data(), 1, n, f) != (size_t)n) exit(2);
  fclose(f);
  return v;
}

// Where each NHF2 packet starts.  The ROM stores its length in the two
// bytes in front of it.
static inline std::vector<size_t> findPackets(const std::vector<uint8_t> &rom) {
  std::vector<size_t> offsets;
  for (size_t i = 0; i + 4 <= rom.size(); i++)
    if (memcmp(&rom[i], "NHF2", 4) == 0) offsets.push_back(i);
  return offsets;
}

static inline uint16_t storedLength(const std::vector<uint8_t> &rom, size_t offset) {
  return rom[offset - 2] | (rom[offset - 1] << 8);
}
