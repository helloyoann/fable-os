/* heap.c — kernel heap allocator (see include/heap.h for the contract).
 *
 * ALGORITHM
 *   A first-fit free-list over a single contiguous arena. Every block — free or
 *   in use — carries a fixed 32-byte header and is threaded into one doubly
 *   linked list ordered by address (the "physical" list). Because blocks are
 *   physically adjacent, freeing a block can coalesce with its immediate
 *   neighbours in O(1), which is what keeps long-lived churn (mbedTLS handshakes
 *   allocate and free thousands of bignums) from fragmenting the arena.
 *
 *   Splitting: an allocation carves the requested amount off the front of a
 *   large-enough free block and leaves the remainder as a smaller free block,
 *   provided the remainder can still hold a header plus a minimum payload.
 *
 * SAFETY
 *   Each header holds a magic word. It is checked on free/realloc so a stray
 *   write or double free is caught immediately instead of corrupting silently.
 *   Freed payloads are poisoned with 0xDE so use-after-free reads are obvious.
 *
 * This file is the only place that knows the block layout; swap it wholesale to
 * change allocators.
 */

#include "heap.h"
#include "kernel.h"

/* Compiled-in backing store, used until/unless heap_init() is given another
 * region. 16 MiB matches the previous bump arena so behaviour is unchanged for
 * existing workloads — it just gets reclaimed now. */
#define DEFAULT_ARENA_SIZE (16 * 1024 * 1024)
static uint8_t default_arena[DEFAULT_ARENA_SIZE] __attribute__((aligned(16)));

#define BLOCK_MAGIC   0x424C4B21u   /* "BLK!" — header sentinel            */
#define POISON_BYTE   0xDE          /* fills freed payloads                */
#define ALIGN         16u           /* payload alignment guarantee         */
#define MIN_PAYLOAD   16u           /* smallest split remainder worth keeping */

typedef struct block {
    uint32_t      magic;   /* BLOCK_MAGIC while this header is valid */
    uint32_t      free;    /* 1 = free, 0 = in use                   */
    size_t        size;    /* payload bytes (excludes this header)   */
    struct block *prev;    /* physically preceding block, or NULL    */
    struct block *next;    /* physically following block, or NULL    */
} block_t;

/* Header size rounded up so every payload starts 16-byte aligned. */
#define HDR (( (sizeof(block_t) + ALIGN - 1) / ALIGN) * ALIGN)

static block_t *heap_head;      /* first block in the arena */
static size_t   arena_bytes;    /* total managed bytes      */

/* Live statistics, maintained incrementally so heap_stats() is O(1). */
static size_t   stat_in_use, stat_peak, stat_used_blocks, stat_free_blocks;
static uint64_t stat_allocs, stat_frees;

static inline size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static inline void *payload_of(block_t *b) { return (uint8_t *)b + HDR; }

static block_t *block_of(void *p) {
    return (block_t *)((uint8_t *)p - HDR);
}

static void check_header(block_t *b, const char *who) {
    if (b->magic != BLOCK_MAGIC) {
        kprintf("heap: bad magic %x in %s\n", b->magic, who);
        panic("heap corruption / invalid free");
    }
}

void heap_init(void *base, size_t size) {
    if (size <= HDR + MIN_PAYLOAD) panic("heap_init: region too small");

    /* Align the base upward and account for any bytes lost to alignment. */
    uintptr_t start = align_up((uintptr_t)base, ALIGN);
    size -= (size_t)(start - (uintptr_t)base);

    heap_head = (block_t *)start;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->free  = 1;
    heap_head->size  = size - HDR;
    heap_head->prev  = NULL;
    heap_head->next  = NULL;

    arena_bytes      = size;
    stat_in_use      = 0;
    stat_peak        = 0;
    stat_used_blocks = 0;
    stat_free_blocks = 1;
    stat_allocs      = 0;
    stat_frees       = 0;
}

/* Lazily install the default arena so callers never have to sequence us. */
static void ensure_init(void) {
    if (!heap_head) heap_init(default_arena, DEFAULT_ARENA_SIZE);
}

/* Split `b` so it holds exactly `need` payload bytes, spinning the remainder
 * off into a new free block when there is room for one. */
static void split(block_t *b, size_t need) {
    if (b->size < need + HDR + MIN_PAYLOAD) return;   /* remainder too small */

    block_t *rest = (block_t *)((uint8_t *)payload_of(b) + need);
    rest->magic = BLOCK_MAGIC;
    rest->free  = 1;
    rest->size  = b->size - need - HDR;
    rest->prev  = b;
    rest->next  = b->next;
    if (rest->next) rest->next->prev = rest;
    b->next = rest;
    b->size = need;

    stat_free_blocks++;
}

void *kmalloc(size_t n) {
    ensure_init();
    if (n == 0) n = 1;
    n = align_up(n, ALIGN);

    for (block_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        split(b, n);
        b->free = 0;
        stat_free_blocks--;
        stat_used_blocks++;
        stat_in_use += b->size;
        if (stat_in_use > stat_peak) stat_peak = stat_in_use;
        stat_allocs++;
        return payload_of(b);
    }
    panic("kmalloc: out of memory");
    return NULL;
}

/* Merge `b` with its next neighbour if that neighbour is also free. */
static void coalesce_next(block_t *b) {
    block_t *n = b->next;
    if (!n || !n->free) return;
    check_header(n, "coalesce");
    b->size += HDR + n->size;
    b->next  = n->next;
    if (n->next) n->next->prev = b;
    n->magic = 0;                 /* invalidate the absorbed header */
    stat_free_blocks--;
}

void kfree(void *p) {
    if (!p) return;
    ensure_init();

    block_t *b = block_of(p);
    check_header(b, "kfree");
    if (b->free) panic("kfree: double free");

    b->free = 1;
    stat_used_blocks--;
    stat_free_blocks++;
    stat_in_use -= b->size;
    stat_frees++;

    memset(payload_of(b), POISON_BYTE, b->size);

    coalesce_next(b);                         /* merge forward  */
    if (b->prev && b->prev->free) {           /* merge backward */
        coalesce_next(b->prev);
    }
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return NULL; }

    block_t *b = block_of(p);
    check_header(b, "krealloc");
    size_t want = align_up(n, ALIGN);

    if (b->size >= want) {                    /* shrink in place */
        split(b, want);
        if (b->next && b->next->free) {
            /* the split remainder can merge with a following free block */
            coalesce_next(b);
        }
        /* stat_in_use reflects the (possibly reduced) size after split */
        return p;
    }

    /* Try to grow into an adjacent free block before copying. */
    if (b->next && b->next->free && b->size + HDR + b->next->size >= want) {
        size_t old = b->size;
        stat_in_use -= old;
        coalesce_next(b);
        split(b, want);
        b->free = 0;
        stat_in_use += b->size;
        if (stat_in_use > stat_peak) stat_peak = stat_in_use;
        return p;
    }

    void *np = kmalloc(n);
    memcpy(np, p, b->size < n ? b->size : n);
    kfree(p);
    return np;
}

void *kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    if (size && total / size != count) panic("kcalloc: size overflow");
    void *p = kmalloc(total);
    memset(p, 0, total);
    return p;
}

void *kmemalign(size_t align, size_t n) {
    ensure_init();
    if (align <= ALIGN) return kmalloc(n);
    if (align & (align - 1)) panic("kmemalign: alignment not power of two");

    n = align_up(n, ALIGN);

    for (block_t *b = heap_head; b; b = b->next) {
        if (!b->free) continue;
        uintptr_t pl = (uintptr_t)payload_of(b);

        /* Whether this block's payload is ALREADY aligned is a separate
         * question from whether the front gap is empty, and conflating the two
         * is how this used to hand back misaligned memory: when `pl + HDR`
         * happened to be align-aligned the gap came out zero, the "no carving
         * needed" branch fired, and `pl` — not `aligned` — was returned. Which
         * of the two fired depended on the arena's link address, so it was
         * invisible until something moved .bss. */
        int       already = (pl & (uintptr_t)(align - 1)) == 0;
        uintptr_t aligned;
        size_t    front_gap;

        if (already) {
            aligned   = pl;
            front_gap = 0;
        } else {
            /* Room for a fresh header at `aligned - HDR`, and the carved-off
             * front must itself remain a valid free block — so it needs a
             * payload of its own, not zero bytes. */
            aligned = align_up(pl + HDR, align);
            if (aligned - HDR == pl) aligned += align;
            front_gap = aligned - HDR - pl;
            if (front_gap < MIN_PAYLOAD) continue;
        }

        if (aligned + n > (uintptr_t)payload_of(b) + b->size) continue;

        block_t *target;
        if (front_gap == 0) {
            target = b;
        } else {
            /* Shrink b to the front gap; place the aligned block after it. */
            target = (block_t *)(aligned - HDR);
            target->magic = BLOCK_MAGIC;
            target->free  = 1;
            target->size  = (size_t)(((uintptr_t)payload_of(b) + b->size) - aligned);
            target->prev  = b;
            target->next  = b->next;
            if (target->next) target->next->prev = target;
            b->next = target;
            b->size = front_gap;
            stat_free_blocks++;
        }

        split(target, n);
        target->free = 0;
        stat_free_blocks--;
        stat_used_blocks++;
        stat_in_use += target->size;
        if (stat_in_use > stat_peak) stat_peak = stat_in_use;
        stat_allocs++;
        return payload_of(target);
    }
    panic("kmemalign: out of memory");
    return NULL;
}

void heap_stats(heap_stats_t *out) {
    ensure_init();
    out->arena_size   = arena_bytes;
    out->bytes_in_use = stat_in_use;
    out->overhead     = (stat_used_blocks + stat_free_blocks) * HDR;
    out->bytes_free   = arena_bytes - stat_in_use - out->overhead;
    out->peak_in_use  = stat_peak;
    out->free_blocks  = stat_free_blocks;
    out->used_blocks  = stat_used_blocks;
    out->total_allocs = stat_allocs;
    out->total_frees  = stat_frees;
}

void heap_dump(void) {
    heap_stats_t s;
    heap_stats(&s);
    kprintf("heap: %u/%u KiB used (peak %u KiB), %u live / %u free blocks, "
            "%u allocs %u frees\n",
            (unsigned)(s.bytes_in_use / 1024), (unsigned)(s.arena_size / 1024),
            (unsigned)(s.peak_in_use / 1024),
            (unsigned)s.used_blocks, (unsigned)s.free_blocks,
            (unsigned)s.total_allocs, (unsigned)s.total_frees);
}

int heap_check(void) {
    ensure_init();
    size_t counted = 0;
    for (block_t *b = heap_head; b; b = b->next) {
        check_header(b, "heap_check");
        counted += HDR + b->size;
        if (b->next && b->next->prev != b) panic("heap: broken link");
        if (b->free && b->next && b->next->free)
            panic("heap: uncoalesced free blocks");
    }
    if (counted != arena_bytes) panic("heap: size accounting mismatch");
    return 0;
}
