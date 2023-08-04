#include "test/jemalloc_test.h"

#include "jemalloc/internal/pa.h"
#include "jemalloc/internal/pa_trace_event.h"
#include <sys/stat.h>
#include <sys/mman.h>

static void *
alloc_hook(extent_hooks_t *extent_hooks, void *new_addr, size_t size, size_t alignment, bool *zero,
           bool *commit, unsigned arena_ind) {
	void *ret = pages_map(new_addr, size, alignment, commit);
	return ret;
}

static bool
merge_hook(extent_hooks_t *extent_hooks, void *addr_a, size_t size_a, void *addr_b, size_t size_b,
           bool committed, unsigned arena_ind) {
	return !maps_coalesce;
}

static bool
split_hook(extent_hooks_t *extent_hooks, void *addr, size_t size, size_t size_a, size_t size_b,
           bool committed, unsigned arena_ind) {
	return !maps_coalesce;
}

static void
init_test_extent_hooks(extent_hooks_t *hooks) {
	/*
	 * The default hooks are mostly fine for testing.  A few of them,
	 * though, access globals (alloc for dss setting in an arena, split and
	 * merge touch the global emap to find head state.  The first of these
	 * can be fixed by keeping that state with the hooks, where it logically
	 * belongs.  The second, though, we can only fix when we use the extent
	 * hook API.
	 */
	memcpy(hooks, &ehooks_default_extent_hooks, sizeof(extent_hooks_t));
	hooks->alloc = &alloc_hook;
	hooks->merge = &merge_hook;
	hooks->split = &split_hook;
}

typedef struct pa_shard_bundle_s pa_shard_bundle_t;
/**
 * A `pa_shard_t` along with the various bits of
 * state that it owns and needs to function.
 */
struct pa_shard_bundle_s {
	pa_shard_t shard;
	base_t *base;
	pa_shard_stats_t stats;
	malloc_mutex_t stats_mtx;
	extent_hooks_t hooks;
};

static pa_shard_bundle_t *
init_test_data(size_t num_shards) {
	assert(num_shards > 0);
	/**
	 * All of the state belonging to pa_central, of which there is just one instance.
	 */
	static struct {
		pa_central_t pa_central;
		base_t *base;
		extent_hooks_t hooks;
		emap_t emap;
	} central_state;

	// First initialize pa_central

	init_test_extent_hooks(&central_state.hooks);
	central_state.base = base_new(TSDN_NULL, /* ind */ 1, &central_state.hooks,
	                              /* metadata_use_hooks */ true);
	assert_ptr_not_null(central_state.base, "Failed to initialize base for pa_central_t");

	bool err = pa_central_init(&central_state.pa_central, central_state.base, /* hpa */ true,
	                           &hpa_hooks_default);
	assert_false(err, "Failed to initialize pa_central_t");

	err = emap_init(&central_state.emap, central_state.base, /* zeroed */ true);
	assert_false(err, "Failed to initialize emap for pa_central_t");

	// Then initialize each shard

	pa_shard_bundle_t *shard_bundles = calloc(num_shards, sizeof(pa_shard_bundle_t));
	assert_ptr_not_null(shard_bundles, "Failed to allocate memory for shards");
	for (size_t i = 0; i < num_shards; i++) {
		pa_shard_bundle_t *data = &shard_bundles[i];

		init_test_extent_hooks(&data->hooks);
		uint32_t shard_index = (uint32_t)i + 2;
		base_t *base = base_new(TSDN_NULL, /* ind */ shard_index, &data->hooks,
		                        /* metadata_use_hooks */ true);
		assert_ptr_not_null(base, "Failed to allocate base for shard %lu", i);
		data->base = base;

		nstime_t time;
		nstime_init(&time, 0);
		const size_t pa_oversize_threshold = 8 * 1024 * 1024;
		// These are irrelevant since we are only replaying allocations for the HPA
		const ssize_t dirty_decay_ms = 0;
		const ssize_t muzzy_decay_ms = 0;
		err = pa_shard_init(TSDN_NULL, &data->shard, &central_state.pa_central, &central_state.emap,
		                    data->base, /* ind */ shard_index, &data->stats, &data->stats_mtx,
		                    &time, pa_oversize_threshold, dirty_decay_ms, muzzy_decay_ms);
		assert_false(err, "Failed to initialize shard %lu", i);
		err = pa_shard_enable_hpa(TSDN_NULL, &data->shard, &opt_hpa_opts, &opt_hpa_sec_opts);
		assert_false(err, "Failed to enable HPA for shard %lu", i);
	}

	return shard_bundles;
}

static size_t
compute_hpa_dirty_memory(size_t num_shards, pa_shard_bundle_t shard_bundles[num_shards]) {
	size_t dirty_pages = 0;
	for (size_t i = 0; i < num_shards; i++) {
		psset_t *psset = &shard_bundles[i].shard.hpa_shard.psset;
		dirty_pages += psset->merged_stats.ndirty;
	}
	// Multiplying to translate from pages to KiB
	return dirty_pages * (PAGE >> 10);
}

#define MAX_NUM_EDATAS ((1UL << 30) / sizeof(edata_t *))
#define MEASURE_DIRTY_MEMORY_INTERVAL 65536

// These would all be passed to `page_allocator_replay` as arguments,
// but the way Jemalloc's test framework works makes that difficult. So instead
// we resort to making them static globals, with the understanding that they
// should not be used outside of `page_allocator_replay` and the code in `main`
// that populates them.
static size_t num_shards;
static size_t num_edatas;
static size_t num_events;
static pa_trace_event_t *events;

TEST_BEGIN(test_page_allocator_replay) {
	assert_true(opt_hpa, "HPA is not enabled. Please ensure MALLOC_CONF is set correctly.");
	fprintf(stderr, "Replaying %lu events on %lu shards\n", num_events, num_shards);
	assert(num_edatas < MAX_NUM_EDATAS);
	static edata_t *edatas[MAX_NUM_EDATAS];
	pa_shard_bundle_t *shard_bundles = init_test_data(num_shards);
	size_t num_invalid_events = 0;
	const size_t num_dirty_memory_measurements = num_events / MEASURE_DIRTY_MEMORY_INTERVAL;
	size_t *dirty_memory_measurements = malloc(sizeof(size_t[num_dirty_memory_measurements]));
	for (size_t i = 0; i < num_events; i++) {
		if (i > 0 && i % MEASURE_DIRTY_MEMORY_INTERVAL == 0) {
			fprintf(stderr, "\rProgress: %.1f%%", ((double)i) / ((double)num_events) * 100.0);
			dirty_memory_measurements[i / MEASURE_DIRTY_MEMORY_INTERVAL] =
			    compute_hpa_dirty_memory(num_shards, shard_bundles);
		}
		pa_trace_event_t *event = &events[i];
		if (event->edata == 0) {
			continue;
		}
		pa_shard_t *shard = &shard_bundles[event->arena_index].shard;
		if (event->is_alloc) {
			assert(edatas[event->edata] == NULL);
			edatas[event->edata] =
			    pa_alloc(TSDN_NULL, shard, event->size, event->alignment, event->slab, event->szind,
			             event->zero, event->guarded, &(bool){false});
			num_invalid_events += (edatas[event->edata] == NULL);
		} else {
			if (likely(edatas[event->edata] != NULL)) {
				pa_dalloc(TSDN_NULL, shard, edatas[event->edata], &(bool){false});
				edatas[event->edata] = NULL;
			} else {
				num_invalid_events++;
			}
		}
	}

	double percent_invalid = (((double)num_invalid_events) / ((double)num_events)) * 100.0;
	if (percent_invalid >= 0.01) {
		fprintf(stderr, "Warning: %.2f%% of events were invalid\n", percent_invalid);
	}

	size_t average_fragmentation = 0;
	size_t max_fragmentation = 0;
	for (size_t i = 0; i < num_dirty_memory_measurements; i++) {
		average_fragmentation += dirty_memory_measurements[i];
		max_fragmentation = dirty_memory_measurements[i] > max_fragmentation
		                        ? dirty_memory_measurements[i]
		                        : max_fragmentation;
	}
	average_fragmentation /= num_dirty_memory_measurements;

	printf("Average HPA dirty memory: %lu KiB\n", average_fragmentation);
	printf("Maximum HPA dirty memory: %lu KiB\n", max_fragmentation);
	printf("All %lu dirty memory measurements:", num_dirty_memory_measurements);
	for (size_t i = 0; i < (num_events / MEASURE_DIRTY_MEMORY_INTERVAL); i++) {
		printf("%lu\n", dirty_memory_measurements[i]);
	}
}
TEST_END

int
main(int argc, const char **argv) {
	if (argc != 2) {
		fputs("Error: please provide exactly one page allocator trace file as input\n", stderr);
		return 1;
	}
	const int trace_fd = open(argv[1], O_RDONLY);
	if (trace_fd == -1) {
		perror(argv[1]);
		return 1;
	}
	struct stat stat_buffer;
	if (fstat(trace_fd, &stat_buffer) == -1) {
		perror("fstat");
		return 1;
	}
	if (stat_buffer.st_size == 0) {
		fputs("Error: trace file was empty\n", stderr);
		return 1;
	}
	void *data = mmap(NULL, stat_buffer.st_size, PROT_READ, MAP_PRIVATE | MAP_FILE, trace_fd, 0);
	if (data == (void *)-1) {
		perror("mmap");
		return 1;
	}
	if (madvise(data, stat_buffer.st_size, MADV_SEQUENTIAL) == -1) {
		perror("madvise");
		return 1;
	}

	num_shards = ((size_t *)data)[0];
	num_edatas = ((size_t *)data)[1];
	num_events =
	    (stat_buffer.st_size - sizeof(num_shards) - sizeof(num_edatas)) / sizeof(pa_trace_event_t);
	if (num_shards == 0 || num_edatas == 0 || num_events == 0) {
		fputs("Error: trace file was empty\n", stderr);
		return 1;
	}
	events = (pa_trace_event_t *)(((uint8_t *)data) + sizeof(num_shards) + sizeof(num_edatas));
	return test_no_reentrancy(test_page_allocator_replay);
}
