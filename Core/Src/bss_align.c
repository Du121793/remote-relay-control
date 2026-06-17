/* Force 8-byte alignment of the FreeRTOS heap by placing an aligned
   guard variable in BSS.  The linker orders .bss contributions by object
   file, so this file must appear in the Keil project immediately before
   heap_4.c inside the FreeRTOS source group. */
#include <stdint.h>
static uint64_t __attribute__((aligned(8))) bss_align_guard;
