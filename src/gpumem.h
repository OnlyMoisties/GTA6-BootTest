/* GPU-visible "physical memory" shared by xmem.dll (owner) and d3d12_xs.dll (reader). Enabled by GTA6_GPUMEM=1.
   Physical pages (64 KB) live in pagefile-backed sections of 2 GB each (D3D12 can open a section under 4 GB as a heap).
   XMemMapPhysicalPages maps views of those pages into the game's placeholder reservations, so the CPU sees them at the
   game's addresses and the GPU reads the same bytes through the section heaps: zero copy, and aliasing works.
   Page table, two levels over the 48-bit address space:
     dir[va >> 32] -> 65536 entries, one per 64 KB page of that 4 GB chunk
     entry = (section + 1) << 24 | page index inside the section; 0 = not GPU visible */
#pragma once
#include <stdint.h>
#define GM_PAGE        0x10000ull
#define GM_SECPAGES    32768u                 /* 2 GB per section */
#define GM_MAXSEC      256
#define GM_PFN0        0x1001ull              /* first physical page number handed out by XMemAllocatePhysicalPages */
typedef struct {
    uint32_t magic;                           /* 'GMEM' */
    volatile long nsec;                       /* sections created so far */
    void* sec[GM_MAXSEC];                     /* section handles */
    uint32_t* volatile dir[65536];
    volatile long chunkgen[65536];            /* bumped when a chunk changes */
    volatile long long gen;                   /* bumped on any change */
} GpuMem;
