#ifndef JEMALLOC_INTERNAL_RTREE_H
#define JEMALLOC_INTERNAL_RTREE_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree_tsd.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/tsd.h"

/*
 * This radix tree implementation is tailored to the singular purpose of
 * associating metadata with extents that are currently owned by jemalloc.
 *
 *******************************************************************************
 */

/* Number of high insignificant bits. */
#define RTREE_NHIB ((1U << (LG_SIZEOF_PTR+3)) - LG_VADDR)
/* Number of low insigificant bits. */
#define RTREE_NLIB LG_PAGE
/* Number of significant bits. */
#define RTREE_NSB (LG_VADDR - RTREE_NLIB)
/* Number of levels in radix tree. */
#if RTREE_NSB <= 10
#  define RTREE_HEIGHT 1
#elif RTREE_NSB <= 36
#  define RTREE_HEIGHT 2
#elif RTREE_NSB <= 52
#  define RTREE_HEIGHT 3
#else
#  error Unsupported number of significant virtual address bits
#endif
/* Use compact leaf representation if virtual address encoding allows. */
#if RTREE_NHIB == 16 && LG_CEIL(SC_NSIZES) <= 8
#  define RTREE_LEAF_COMPACT
#endif

typedef struct rtree_node_elm_s rtree_node_elm_t;
struct rtree_node_elm_s {
	atomic_p_t	child; /* (rtree_{node,leaf}_elm_t *) */
};

#ifdef RTREE_LEAF_COMPACT
typedef uint16_t metadata_bits_t;
typedef uint8_t metadata_szind_t;
#else
typedef uint32_t metadata_bits_t;
typedef uint16_t metadata_szind_t;
#endif

typedef struct rtree_metadata_s rtree_metadata_t;
struct rtree_metadata_s {
	metadata_szind_t szind;
	bool slab;
	bool is_head; /* Mirrors edata->is_head. */
	extent_state_t state; /* Mirrors edata->state. */
};

typedef struct rtree_contents_s rtree_contents_t;
struct rtree_contents_s {
	edata_t *edata;
	rtree_metadata_t metadata;
};

#define RTREE_LEAF_STATE_WIDTH EDATA_BITS_STATE_WIDTH
#define RTREE_LEAF_STATE_SHIFT EXTENT_STATE_SHIFT
#define RTREE_LEAF_STATE_MASK MASK(RTREE_LEAF_STATE_WIDTH, RTREE_LEAF_STATE_SHIFT)

struct rtree_leaf_elm_s {
#ifdef RTREE_LEAF_COMPACT
	/*
	 * Single pointer-width field containing all five leaf element fields.
	 * For example, on a 64-bit x64 system with 48 significant virtual
	 * memory address bits, an `rtree_contents_t` is packed as such:
	 *
	 * e: edata
	 * s: state
	 * h: is_head
	 * b: slab
	 * x: index
	 *
	 *   eeeeeeee eeeeeeee [...] eeeeeeee sssssshb xxxxxxxx
	 */
	atomic_p_t	le_bits;
#else
	atomic_p_t	le_edata; /* (edata_t *) */
	/*
	 * Packed in the same format as the lower 16 bits of
	 * `le_bits` as shown in the diagram above.
	 */
	atomic_u_t	le_metadata;
#endif
};

typedef struct rtree_level_s rtree_level_t;
struct rtree_level_s {
	/* Number of key bits distinguished by this level. */
	unsigned		bits;
	/*
	 * Cumulative number of key bits distinguished by traversing to
	 * corresponding tree level.
	 */
	unsigned		cumbits;
};

typedef struct rtree_s rtree_t;
struct rtree_s {
	base_t			*base;
	malloc_mutex_t		init_lock;
	/* Number of elements based on rtree_levels[0].bits. */
#if RTREE_HEIGHT > 1
	rtree_node_elm_t	root[1U << (RTREE_NSB/RTREE_HEIGHT)];
#else
	rtree_leaf_elm_t	root[1U << (RTREE_NSB/RTREE_HEIGHT)];
#endif
};

/*
 * Split the bits into one to three partitions depending on number of
 * significant bits.  It the number of bits does not divide evenly into the
 * number of levels, place one remainder bit per level starting at the leaf
 * level.
 */
static const rtree_level_t rtree_levels[] = {
#if RTREE_HEIGHT == 1
	{RTREE_NSB, RTREE_NHIB + RTREE_NSB}
#elif RTREE_HEIGHT == 2
	{RTREE_NSB/2, RTREE_NHIB + RTREE_NSB/2},
	{RTREE_NSB/2 + RTREE_NSB%2, RTREE_NHIB + RTREE_NSB}
#elif RTREE_HEIGHT == 3
	{RTREE_NSB/3, RTREE_NHIB + RTREE_NSB/3},
	{RTREE_NSB/3 + RTREE_NSB%3/2,
	    RTREE_NHIB + RTREE_NSB/3*2 + RTREE_NSB%3/2},
	{RTREE_NSB/3 + RTREE_NSB%3 - RTREE_NSB%3/2, RTREE_NHIB + RTREE_NSB}
#else
#  error Unsupported rtree height
#endif
};

bool rtree_new(rtree_t *rtree, base_t *base, bool zeroed);

rtree_leaf_elm_t *rtree_leaf_elm_lookup_hard(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);

JEMALLOC_ALWAYS_INLINE unsigned
rtree_leaf_maskbits(void) {
	unsigned ptrbits = ZU(1) << (LG_SIZEOF_PTR+3);
	unsigned cumbits = (rtree_levels[RTREE_HEIGHT-1].cumbits -
	    rtree_levels[RTREE_HEIGHT-1].bits);
	return ptrbits - cumbits;
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_leafkey(uintptr_t key) {
	uintptr_t mask = ~((ZU(1) << rtree_leaf_maskbits()) - 1);
	return (key & mask);
}

JEMALLOC_ALWAYS_INLINE size_t
rtree_cache_direct_map(uintptr_t key) {
	return (size_t)((key >> rtree_leaf_maskbits()) &
	    (RTREE_CTX_NCACHE - 1));
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_subkey(uintptr_t key, unsigned level) {
	unsigned ptrbits = ZU(1) << (LG_SIZEOF_PTR+3);
	unsigned cumbits = rtree_levels[level].cumbits;
	unsigned shiftbits = ptrbits - cumbits;
	unsigned maskbits = rtree_levels[level].bits;
	uintptr_t mask = (ZU(1) << maskbits) - 1;
	return ((key >> shiftbits) & mask);
}

JEMALLOC_ALWAYS_INLINE rtree_metadata_t
rtree_leaf_elm_metadata_bits_decode(metadata_bits_t bits) {
	rtree_metadata_t metadata;
	metadata.szind = (metadata_szind_t)bits;
	metadata.slab = (bool)(bits & 0x100);
	metadata.is_head = (bool)(bits & 0x200);
	metadata.state = bits & RTREE_LEAF_STATE_MASK;
	/*
	 * Clang doesn't do a great job taking advantage of the semantics of
	 * booleans for optimization unless you hold its hand like this, and
	 * even still the generated code isn't perfect.
	 */
	util_assume(*(uint8_t *)(&metadata.slab) == 0 ||
	            *(uint8_t *)(&metadata.slab) == 1);
	util_assume(*(uint8_t *)(&metadata.is_head) == 0 ||
	            *(uint8_t *)(&metadata.is_head) == 1);
	return metadata;
}

JEMALLOC_ALWAYS_INLINE metadata_bits_t
rtree_leaf_elm_metadata_bits_encode(rtree_metadata_t metadata) {
	metadata_bits_t szind_bits = (metadata_bits_t)metadata.szind;
	metadata_bits_t slab_bits = ((metadata_bits_t)metadata.slab) << 8;
	metadata_bits_t is_head_bits = ((metadata_bits_t)metadata.is_head) << 9;
	metadata_bits_t state_bits = (metadata_bits_t)metadata.state;
	return szind_bits | state_bits | is_head_bits | slab_bits;
}

/*
 * Atomic getters.
 *
 * dependent: Reading a value on behalf of a pointer to a valid allocation
 *            is guaranteed to be a clean read even without synchronization,
 *            because the rtree update became visible in memory before the
 *            pointer came into existence.
 * !dependent: An arbitrary read, e.g. on behalf of ivsalloc(), may not be
 *             dependent on a previous rtree write, which means a stale read
 *             could result if synchronization were omitted here.
 */
#  ifdef RTREE_LEAF_COMPACT
JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_leaf_elm_bits_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool dependent) {
	return (uintptr_t)atomic_load_p(&elm->le_bits, dependent
	    ? ATOMIC_RELAXED : ATOMIC_ACQUIRE);
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_leaf_elm_bits_encode(rtree_contents_t contents) {
	assert((uintptr_t)contents.edata % (uintptr_t)EDATA_ALIGNMENT == 0);
	uintptr_t edata_bits = ((uintptr_t)contents.edata) << 16;
	uintptr_t metadata_bits =
	    rtree_leaf_elm_metadata_bits_encode(contents.metadata);

	assert((edata_bits & metadata_bits) == 0);

	return edata_bits | metadata_bits;
}

JEMALLOC_ALWAYS_INLINE rtree_contents_t
rtree_leaf_elm_bits_decode(uintptr_t bits) {
	rtree_contents_t contents;
	contents.metadata = rtree_leaf_elm_metadata_bits_decode(bits);
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	contents.edata = (edata_t *) (bits >> 16);
	assert((uintptr_t)contents.edata % (uintptr_t)EDATA_ALIGNMENT == 0);
	return contents;
}

#  endif /* RTREE_LEAF_COMPACT */

JEMALLOC_ALWAYS_INLINE rtree_contents_t
rtree_leaf_elm_read(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool dependent) {
#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm, dependent);
	return rtree_leaf_elm_bits_decode(bits);
#else
	rtree_contents_t contents;
	metadata_bits_t metadata_bits = atomic_load_u(&elm->le_metadata, dependent
	    ? ATOMIC_RELAXED : ATOMIC_ACQUIRE);
	contents.metadata = rtree_leaf_elm_metadata_bits_decode(metadata_bits);
	contents.edata = (edata_t *)atomic_load_p(&elm->le_edata, dependent
	    ? ATOMIC_RELAXED : ATOMIC_ACQUIRE);

	return contents;
#endif
}

JEMALLOC_ALWAYS_INLINE void
rtree_contents_encode(rtree_contents_t contents, void **bits,
    metadata_bits_t *additional) {
#ifdef RTREE_LEAF_COMPACT
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	*bits = (void *)rtree_leaf_elm_bits_encode(contents);
	/* Suppress spurious warning from static analysis */
	if (config_debug) {
		*additional = 0;
	}
#else
	*additional = rtree_leaf_elm_metadata_bits_encode(contents.metadata);
	*bits = contents.edata;
#endif
}

JEMALLOC_ALWAYS_INLINE void
rtree_leaf_elm_write_commit(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, void *bits, metadata_bits_t additional) {
#ifdef RTREE_LEAF_COMPACT
	atomic_store_p(&elm->le_bits, bits, ATOMIC_RELEASE);
#else
	atomic_store_u(&elm->le_metadata, additional, ATOMIC_RELEASE);
	/*
	 * Write edata last, since the element is atomically considered valid
	 * as soon as the edata field is non-NULL.
	 */
	atomic_store_p(&elm->le_edata, bits, ATOMIC_RELEASE);
#endif
}

JEMALLOC_ALWAYS_INLINE void
rtree_leaf_elm_write(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, rtree_contents_t contents) {
	assert((uintptr_t)contents.edata % EDATA_ALIGNMENT == 0);
	void *bits;
	metadata_bits_t additional;
	rtree_contents_encode(contents, &bits, &additional);
	rtree_leaf_elm_write_commit(tsdn, rtree, elm, bits, additional);
}

/* The state field can be updated independently (and more frequently). */
JEMALLOC_ALWAYS_INLINE void
rtree_leaf_elm_state_update(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm1, rtree_leaf_elm_t *elm2, extent_state_t state) {
	assert(elm1 != NULL);
#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm1,
	    /* dependent */ true);
	bits &= ~RTREE_LEAF_STATE_MASK;
	bits |= state;
	/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
	atomic_store_p(&elm1->le_bits, (void *)bits, ATOMIC_RELEASE);
	if (elm2 != NULL) {
		/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
		atomic_store_p(&elm2->le_bits, (void *)bits, ATOMIC_RELEASE);
	}
#else
	metadata_bits_t bits = atomic_load_u(&elm1->le_metadata, ATOMIC_RELAXED);
	bits &= ~RTREE_LEAF_STATE_MASK;
	bits |= state;
	atomic_store_u(&elm1->le_metadata, bits, ATOMIC_RELEASE);
	if (elm2 != NULL) {
		atomic_store_u(&elm2->le_metadata, bits, ATOMIC_RELEASE);
	}
#endif
}

/*
 * Tries to look up the key in the L1 cache, returning false if there's a hit, or
 * true if there's a miss.
 * Key is allowed to be NULL; returns true in this case.
 */
JEMALLOC_ALWAYS_INLINE bool
rtree_leaf_elm_lookup_fast(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, rtree_leaf_elm_t **elm) {
	size_t slot = rtree_cache_direct_map(key);
	uintptr_t leafkey = rtree_leafkey(key);
	assert(leafkey != RTREE_LEAFKEY_INVALID);

	if (unlikely(rtree_ctx->cache[slot].leafkey != leafkey)) {
		return true;
	}

	rtree_leaf_elm_t *leaf = rtree_ctx->cache[slot].leaf;
	assert(leaf != NULL);
	uintptr_t subkey = rtree_subkey(key, RTREE_HEIGHT-1);
	*elm = &leaf[subkey];

	return false;
}

JEMALLOC_ALWAYS_INLINE rtree_leaf_elm_t *
rtree_leaf_elm_lookup(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	assert(key != 0);
	assert(!dependent || !init_missing);

	size_t slot = rtree_cache_direct_map(key);
	uintptr_t leafkey = rtree_leafkey(key);
	assert(leafkey != RTREE_LEAFKEY_INVALID);

	/* Fast path: L1 direct mapped cache. */
	if (likely(rtree_ctx->cache[slot].leafkey == leafkey)) {
		rtree_leaf_elm_t *leaf = rtree_ctx->cache[slot].leaf;
		assert(leaf != NULL);
		uintptr_t subkey = rtree_subkey(key, RTREE_HEIGHT-1);
		return &leaf[subkey];
	}
	/*
	 * Search the L2 LRU cache.  On hit, swap the matching element into the
	 * slot in L1 cache, and move the position in L2 up by 1.
	 */
#define RTREE_CACHE_CHECK_L2(i) do {					\
	if (likely(rtree_ctx->l2_cache[i].leafkey == leafkey)) {	\
		rtree_leaf_elm_t *leaf = rtree_ctx->l2_cache[i].leaf;	\
		assert(leaf != NULL);					\
		if (i > 0) {						\
			/* Bubble up by one. */				\
			rtree_ctx->l2_cache[i].leafkey =		\
				rtree_ctx->l2_cache[i - 1].leafkey;	\
			rtree_ctx->l2_cache[i].leaf =			\
				rtree_ctx->l2_cache[i - 1].leaf;	\
			rtree_ctx->l2_cache[i - 1].leafkey =		\
			    rtree_ctx->cache[slot].leafkey;		\
			rtree_ctx->l2_cache[i - 1].leaf =		\
			    rtree_ctx->cache[slot].leaf;		\
		} else {						\
			rtree_ctx->l2_cache[0].leafkey =		\
			    rtree_ctx->cache[slot].leafkey;		\
			rtree_ctx->l2_cache[0].leaf =			\
			    rtree_ctx->cache[slot].leaf;		\
		}							\
		rtree_ctx->cache[slot].leafkey = leafkey;		\
		rtree_ctx->cache[slot].leaf = leaf;			\
		uintptr_t subkey = rtree_subkey(key, RTREE_HEIGHT-1);	\
		return &leaf[subkey];					\
	}								\
} while (0)
	/* Check the first cache entry. */
	RTREE_CACHE_CHECK_L2(0);
	/* Search the remaining cache elements. */
	for (unsigned i = 1; i < RTREE_CTX_NCACHE_L2; i++) {
		RTREE_CACHE_CHECK_L2(i);
	}
#undef RTREE_CACHE_CHECK_L2

	return rtree_leaf_elm_lookup_hard(tsdn, rtree, rtree_ctx, key,
	    dependent, init_missing);
}

/*
 * Returns true on lookup failure.
 */
static inline bool
rtree_read_independent(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, rtree_contents_t *r_contents) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, /* dependent */ false, /* init_missing */ false);
	if (elm == NULL) {
		return true;
	}
	*r_contents = rtree_leaf_elm_read(tsdn, rtree, elm,
	    /* dependent */ false);
	return false;
}

static inline rtree_contents_t
rtree_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, /* dependent */ true, /* init_missing */ false);
	assert(elm != NULL);
	return rtree_leaf_elm_read(tsdn, rtree, elm, /* dependent */ true);
}

static inline rtree_metadata_t
rtree_metadata_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, /* dependent */ true, /* init_missing */ false);
	assert(elm != NULL);
	return rtree_leaf_elm_read(tsdn, rtree, elm,
	    /* dependent */ true).metadata;
}

/*
 * Returns true when the request cannot be fulfilled by fastpath.
 */
static inline bool
rtree_metadata_try_read_fast(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, rtree_metadata_t *r_rtree_metadata) {
	rtree_leaf_elm_t *elm;
	/*
	 * Should check the bool return value (lookup success or not) instead of
	 * elm == NULL (which will result in an extra branch).  This is because
	 * when the cache lookup succeeds, there will never be a NULL pointer
	 * returned (which is unknown to the compiler).
	 */
	if (rtree_leaf_elm_lookup_fast(tsdn, rtree, rtree_ctx, key, &elm)) {
		return true;
	}
	assert(elm != NULL);
	*r_rtree_metadata = rtree_leaf_elm_read(tsdn, rtree, elm,
	    /* dependent */ true).metadata;
	return false;
}

JEMALLOC_ALWAYS_INLINE void
rtree_write_range_impl(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t base, uintptr_t end, rtree_contents_t contents, bool clearing) {
	assert((base & PAGE_MASK) == 0 && (end & PAGE_MASK) == 0);
	/*
	 * Only used for emap_(de)register_interior, which implies the
	 * boundaries have been registered already.  Therefore all the lookups
	 * are dependent w/o init_missing, assuming the range spans across at
	 * most 2 rtree leaf nodes (each covers 1 GiB of vaddr).
	 */
	void *bits;
	metadata_bits_t additional;
	rtree_contents_encode(contents, &bits, &additional);

	rtree_leaf_elm_t *elm = NULL; /* Dead store. */
	for (uintptr_t addr = base; addr <= end; addr += PAGE) {
		if (addr == base ||
		    (addr & ((ZU(1) << rtree_leaf_maskbits()) - 1)) == 0) {
			elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx, addr,
			    /* dependent */ true, /* init_missing */ false);
			assert(elm != NULL);
		}
		assert(elm == rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx, addr,
		    /* dependent */ true, /* init_missing */ false));
		assert(!clearing || rtree_leaf_elm_read(tsdn, rtree, elm,
		    /* dependent */ true).edata != NULL);
		rtree_leaf_elm_write_commit(tsdn, rtree, elm, bits, additional);
		elm++;
	}
}

JEMALLOC_ALWAYS_INLINE void
rtree_write_range(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t base, uintptr_t end, rtree_contents_t contents) {
	rtree_write_range_impl(tsdn, rtree, rtree_ctx, base, end, contents,
	    /* clearing */ false);
}

JEMALLOC_ALWAYS_INLINE bool
rtree_write(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx, uintptr_t key,
    rtree_contents_t contents) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, /* dependent */ false, /* init_missing */ true);
	if (elm == NULL) {
		return true;
	}

	rtree_leaf_elm_write(tsdn, rtree, elm, contents);

	return false;
}

static inline void
rtree_clear(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, /* dependent */ true, /* init_missing */ false);
	assert(elm != NULL);
	assert(rtree_leaf_elm_read(tsdn, rtree, elm,
	    /* dependent */ true).edata != NULL);
	rtree_contents_t contents;
	contents.edata = NULL;
	contents.metadata.szind = SC_NSIZES;
	contents.metadata.slab = false;
	contents.metadata.is_head = false;
	contents.metadata.state = (extent_state_t)0;
	rtree_leaf_elm_write(tsdn, rtree, elm, contents);
}

static inline void
rtree_clear_range(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t base, uintptr_t end) {
	rtree_contents_t contents;
	contents.edata = NULL;
	contents.metadata.szind = SC_NSIZES;
	contents.metadata.slab = false;
	contents.metadata.is_head = false;
	contents.metadata.state = (extent_state_t)0;
	rtree_write_range_impl(tsdn, rtree, rtree_ctx, base, end, contents,
	    /* clearing */ true);
}

#endif /* JEMALLOC_INTERNAL_RTREE_H */
