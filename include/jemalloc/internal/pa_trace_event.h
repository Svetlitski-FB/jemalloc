#ifndef JEMALLOC_INTERNAL_PA_TRACE_EVENT_H
#define JEMALLOC_INTERNAL_PA_TRACE_EVENT_H

struct pa_trace_event_s {
    uint64_t timestamp;
    uintptr_t edata;
    size_t size;
    size_t alignment;
    uint32_t szind;
    uint16_t arena_index;
    bool is_alloc : 1;
    bool slab : 1;
    bool zero : 1;
    bool guarded : 1;
};

typedef struct pa_trace_event_s pa_trace_event_t;

#endif /* JEMALLOC_INTERNAL_PA_TRACE_EVENT_H */
