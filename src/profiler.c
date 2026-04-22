#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kos/thread.h>
#include <kos/mutex.h>
#include <kos/tls.h>
#include <arch/timer.h>

#include <dc/perf_monitor.h>

/*
 * Dreamcast Function Profiler – Low-overhead instrumentation for function entry/exit
 * Works in tandem with GCC's -finstrument-functions.
 *
 * This profiler:
 *   ✓ Captures timestamps and performance counters (PRFC0 / PRFC1)
 *   ✓ Computes deltas since the last call per-thread
 *   ✓ Compresses data using unsigned LEB128 encoding
 *   ✓ Divides time deltas by 80 (to match 80ns tick resolution)
 *   ✓ Writes compact variable-length records to /pc/trace.bin via dcload
 *
 * Binary Record Format (per function entry or exit):
 *   uint32_t address
 *     - Bit 31: 1 for entry, 0 for exit
 *     - Bits 30–22: thread ID
 *     - Bits 21–0: compressed function address (>>2 from 0x8C000000)
 *
 *   LEB128 encoded values (1–5 bytes each):
 *     - scaled_time:   delta time / 80ns
 *     - delta_evt0:    delta of PRFC0 (e.g., operand cache misses)
 *     - delta_evt1:    delta of PRFC1 (e.g., instruction cache misses)
 *
 * Memory & Performance:
 *   - Each thread maintains its own 8KB heap-allocated buffer, flushed when full
 *   - All instrumentation functions are marked __no_instrument_function to avoid recursion
 *   - No TLS used; per-thread state stored via thd_key
 *
 * Initialization:
 *   - File opened at startup via constructor (main_constructor)
 *   - Counters started and cleared
 *   - Cleanup handler registered with atexit()
 *
 * Cleanup:
 *   - Flushes remaining buffer contents
 *   - Stops and clears hardware counters
 *   - Closes trace file
 *
 * Paired with `dctrace.py` to decode, resolve symbols, and generate call graphs.
 */

#define BUFFER_SIZE    (1024 * 8)

#define ENTRY_FLAG     0x80000000
#define EXIT_FLAG      0x00000000

#define BASE_ADDRESS   0x8C000000
#define TID_MASK       0x1FF       /* 9 bits */
#define ADDR_MASK      0x003FFFFF  /* 22 bits (compressed address) */

#define MAX_ENTRY_SIZE 19

#define MAKE_ADDRESS(entry, tid, full_addr) \
     (((entry) ? ENTRY_FLAG : 0) | \
     (((tid) & TID_MASK) << 22) | \
     (((((uint32_t)(full_addr)) - BASE_ADDRESS) >> 2) & ADDR_MASK))

/* Per-thread state, heap allocated and stored via kthread_key */
typedef struct {
    uint8_t  buffer[BUFFER_SIZE] __attribute__((aligned(32)));
    uint8_t *ptr;
    size_t   buffer_idx;
    uint32_t thread_id;
    uint64_t last_time;
    uint64_t last_event0;
    uint64_t last_event1;
} thread_prof_t;

static int      fd = -1;
static FILE    *fp = NULL;
static mutex_t  io_lock = MUTEX_INITIALIZER;

/* kthread_key for per-thread profiler state */
static kthread_key_t prof_key;

/* Set once profiler is fully ready to record */
static volatile int profiler_ready = 0;

/* ------------------------------------------------------------------ */

static inline void __attribute__((no_instrument_function))
write_u32_unaligned(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static size_t __attribute__((no_instrument_function))
encode_uleb128(uint32_t value, uint8_t *out) {
    if(value < 0x80) {
        out[0] = (uint8_t)value;
        return 1;
    }
    size_t count = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if(value != 0)
            byte |= 0x80;
        out[count++] = byte;
    } while(value != 0);
    return count;
}

/* Called by thd_key when a thread exits, to free its state block */
static void __attribute__((no_instrument_function))
prof_key_destructor(void *data) {
    thread_prof_t *ts = (thread_prof_t *)data;
    if(!ts) return;

    /* Flush any remaining data */
    if(ts->buffer_idx > 0 && fd >= 0) {
        mutex_lock(&io_lock);
        write(fd, ts->buffer, ts->buffer_idx);
        mutex_unlock(&io_lock);
    }

    free(ts);
}

/* Allocate and initialise per-thread state; called lazily on first hook fire */
static thread_prof_t * __attribute__((no_instrument_function))
init_thread_state(void) {
    kthread_t *th = thd_get_current();
    if(!th) return NULL;

    thread_prof_t *ts = (thread_prof_t *)malloc(sizeof(thread_prof_t));
    if(!ts) return NULL;

    memset(ts, 0, sizeof(thread_prof_t));
    ts->ptr       = ts->buffer;
    ts->thread_id = th->tid & TID_MASK;
    ts->last_time   = timer_ns_gettime64();
    ts->last_event0 = perf_cntr_count(PRFC0);
    ts->last_event1 = perf_cntr_count(PRFC1);

    kthread_setspecific(prof_key, ts);
    return ts;
}

/* ------------------------------------------------------------------ */

static void __attribute__((no_instrument_function, hot))
create_entry(void *this_fn, uint32_t flag) {
    /* Double-checked early-out before any KOS calls */
    if(!profiler_ready || fd < 0)
        return;

    thread_prof_t *ts = (thread_prof_t *)kthread_getspecific(prof_key);
    if(__builtin_expect(!ts, 0)) {
        ts = init_thread_state();
        if(!ts) return;
    }

    uint64_t now = timer_ns_gettime64();
    uint64_t e0  = perf_cntr_count(PRFC0);
    uint64_t e1  = perf_cntr_count(PRFC1);

    uint32_t diff_evt0  = (uint32_t)(e0  - ts->last_event0);
    uint32_t diff_evt1  = (uint32_t)(e1  - ts->last_event1);
    uint32_t delta_time = (uint32_t)(now - ts->last_time);
    uint32_t scaled_time = delta_time / 80;

    uint32_t addr = MAKE_ADDRESS(flag, ts->thread_id, this_fn);
    uint8_t entry_buf[MAX_ENTRY_SIZE];
    uint8_t *entry_ptr = entry_buf;

    write_u32_unaligned(entry_ptr, addr);
    entry_ptr += 4;
    entry_ptr += encode_uleb128(scaled_time, entry_ptr);
    entry_ptr += encode_uleb128(diff_evt0,   entry_ptr);
    entry_ptr += encode_uleb128(diff_evt1,   entry_ptr);

    size_t entry_size = (size_t)(entry_ptr - entry_buf);

    if(__builtin_expect(ts->buffer_idx + entry_size > BUFFER_SIZE, 0)) {
        mutex_lock(&io_lock);
        write(fd, ts->buffer, ts->buffer_idx);
        mutex_unlock(&io_lock);
        ts->ptr        = ts->buffer;
        ts->buffer_idx = 0;
    }

    memcpy(ts->ptr, entry_buf, entry_size);
    ts->ptr += entry_size;
    ts->buffer_idx += entry_size;

    ts->last_time   = now;
    ts->last_event0 = e0;
    ts->last_event1 = e1;
}

/* ------------------------------------------------------------------ */

static void __attribute__((no_instrument_function))
cleanup(void) {
    /* Mark profiler stopped first so no new records are written */
    profiler_ready = 0;

    /* Flush the calling thread's buffer (other threads should be done by now) */
    thread_prof_t *ts = (thread_prof_t *)kthread_getspecific(prof_key);
    if(ts && ts->buffer_idx > 0 && fd >= 0) {
        mutex_lock(&io_lock);
        write(fd, ts->buffer, ts->buffer_idx);
        mutex_unlock(&io_lock);
        ts->buffer_idx = 0;
    }

    perf_cntr_stop(PRFC0);
    perf_cntr_stop(PRFC1);
    perf_cntr_clear(PRFC0);
    perf_cntr_clear(PRFC1);

    if(fp != NULL) {
        fclose(fp);
        fp = NULL;
        fd = -1;
    }
}

/* ------------------------------------------------------------------ */

void __attribute__((no_instrument_function, hot))
__cyg_profile_func_enter(void *this_fn, void *callsite) {
    (void)callsite;
    create_entry(this_fn, ENTRY_FLAG);
}

void __attribute__((no_instrument_function, hot))
__cyg_profile_func_exit(void *this_fn, void *callsite) {
    (void)callsite;
    create_entry(this_fn, EXIT_FLAG);
}

void __attribute__((no_instrument_function, constructor))
main_constructor(void) {
    /* Create the per-thread key with a destructor that flushes on thread exit */
    if(kthread_key_create(&prof_key, prof_key_destructor) != 0) {
        fprintf(stderr, "profiler: kthread_key_create failed\n");
        return;
    }

    fp = fopen("/pc/trace.bin", "wb");
    if(fp == NULL) {
        fprintf(stderr, "profiler: trace.bin not opened\n");
        return;
    }

    fd = fileno(fp);

    atexit(cleanup);

    perf_cntr_timer_disable();
    perf_cntr_clear(PRFC0);
    perf_cntr_clear(PRFC1);
    perf_cntr_start(PRFC0, PMCR_OPERAND_CACHE_MISS_MODE,    PMCR_COUNT_CPU_CYCLES);
    perf_cntr_start(PRFC1, PMCR_INSTRUCTION_CACHE_MISS_MODE, PMCR_COUNT_CPU_CYCLES);

    /* Must be set LAST — nothing above here will be recorded */
    profiler_ready = 1;
}
