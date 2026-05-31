/* mm/pmm.c  -  physical memory manager (frame bitmap). */
#include "pmm.h"
#include "../lib/string.h"

#define MAX_FRAMES   (1024 * 1024)        /* up to 4 GiB / 4 KiB */
#define BITMAP_WORDS (MAX_FRAMES / 32)

static uint32_t frame_bitmap[BITMAP_WORDS];
static uint32_t total_frames;
static uint32_t used_frames;

static void bitmap_set(uint32_t frame)
{
    frame_bitmap[frame / 32] |= (1u << (frame % 32));
}

static void bitmap_clear(uint32_t frame)
{
    frame_bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static bool bitmap_test(uint32_t frame)
{
    return (frame_bitmap[frame / 32] & (1u << (frame % 32))) != 0;
}

void pmm_init(uint32_t total_memory_bytes)
{
    total_frames = total_memory_bytes / PMM_FRAME_SIZE;
    if (total_frames > MAX_FRAMES)
        total_frames = MAX_FRAMES;

    /* Mark everything used initially, then free what's available. */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = total_frames;

    /* Free all frames in range (they'll be re-reserved by reserve_region). */
    for (uint32_t i = 0; i < total_frames; i++) {
        bitmap_clear(i);
    }
    used_frames = 0;

    /* Reserve the first 1 MiB (BIOS/IVT/VGA) and the kernel + heap region.
     * Kernel lives at 1 MiB; reserve up to 256 MiB to cover kernel + the
     * static 96 MiB heap arena conservatively. */
    pmm_reserve_region(0x00000000, 256 * 1024 * 1024);
}

void pmm_reserve_region(uint32_t addr, uint32_t len)
{
    uint32_t start = addr / PMM_FRAME_SIZE;
    uint32_t end   = (addr + len + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint32_t f = start; f < end && f < total_frames; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            return i * PMM_FRAME_SIZE;
        }
    }
    return 0;   /* out of memory */
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t frame = addr / PMM_FRAME_SIZE;
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        if (used_frames > 0)
            used_frames--;
    }
}

uint32_t pmm_get_total_memory(void) { return total_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_used_memory(void)  { return used_frames * PMM_FRAME_SIZE; }
uint32_t pmm_get_free_memory(void)
{
    return (total_frames - used_frames) * PMM_FRAME_SIZE;
}
