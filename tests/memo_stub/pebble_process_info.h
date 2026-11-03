/* Host stand-in for the SDK's app header. The real header is the first thing in the app
 * binary, and the loader writes the firmware table's address at sym_table_addr bytes from
 * its start; here that word is a trailing field the test fills in. */
#pragma once
#include <stdint.h>
typedef struct __attribute__((__packed__)) {
  char header[8];
  uint32_t sym_table_addr;
  void *const *loader_table;
} PebbleProcessInfo;
