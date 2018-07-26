/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <inttypes.h>

#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_jhash.h>
#include <rte_fbk_hash.h>
#include <rte_random.h>
#include <rte_string_fns.h>

#include "test.h"

#define MAX_ENTRIES (1 << 19)
#define KEYS_TO_ADD (MAX_ENTRIES * 3 / 4) /* 75% table utilization */
#define NUM_LOOKUPS (KEYS_TO_ADD * 5) /* Loop among keys added, several times */
#define BUCKET_SIZE 4
#define NUM_BUCKETS (MAX_ENTRIES / BUCKET_SIZE)
#define MAX_KEYSIZE 64
#define NUM_KEYSIZES 10
#define NUM_SHUFFLES 10
#define BURST_SIZE 16

enum operations {
	ADD = 0,
	LOOKUP,
	LOOKUP_MULTI,
	DELETE,
	NUM_OPERATIONS
};

static uint32_t hashtest_key_lens[] = {
	/* standard key sizes */
	4, 8, 16, 32, 48, 64,
	/* IPv4 SRC + DST + protocol, unpadded */
	9,
	/* IPv4 5-tuple, unpadded */
	13,
	/* IPv6 5-tuple, unpadded */
	37,
	/* IPv6 5-tuple, padded to 8-byte boundary */
	40
};

struct rte_hash *h[NUM_KEYSIZES];

/* Array that stores if a slot is full */
uint8_t slot_taken[MAX_ENTRIES];

/* Array to store number of cycles per operation */
uint64_t cycles[NUM_KEYSIZES][NUM_OPERATIONS][2][2];

/* Array to store all input keys */
uint8_t keys[KEYS_TO_ADD][MAX_KEYSIZE];

/* Array to store the precomputed hash for 'keys' */
hash_sig_t signatures[KEYS_TO_ADD];

/* Array to store how many busy entries have each bucket */
uint8_t buckets[NUM_BUCKETS];

/* Array to store the positions where keys are added */
int32_t positions[KEYS_TO_ADD];

/* Parameters used for hash table in unit test functions. */
static struct rte_hash_parameters ut_params = {
	.entries = MAX_ENTRIES,
	.hash_func = rte_jhash,
	.hash_func_init_val = 0,
};

static int
create_table(unsigned with_data, unsigned table_index)
{
	char name[RTE_HASH_NAMESIZE];

	if (with_data)
		/* Table will store 8-byte data */
		sprintf(name, "test_hash%d_data", hashtest_key_lens[table_index]);
	else
		sprintf(name, "test_hash%d", hashtest_key_lens[table_index]);

	ut_params.name = name;
	ut_params.key_len = hashtest_key_lens[table_index];
	ut_params.socket_id = rte_socket_id();
	h[table_index] = rte_hash_find_existing(name);
	if (h[table_index] != NULL)
		/*
		 * If table was already created, free it to create it again,
		 * so we force it is empty
		 */
		rte_hash_free(h[table_index]);
	h[table_index] = rte_hash_create(&ut_params);
	if (h[table_index] == NULL) {
		printf("Error creating table\n");
		return -1;
	}
	return 0;

}

/* Shuffle the keys that have been added, so lookups will be totally random */
static void
shuffle_input_keys(unsigned table_index)
{
	unsigned i;
	uint32_t swap_idx;
	uint8_t temp_key[MAX_KEYSIZE];
	hash_sig_t temp_signature;
	int32_t temp_position;

	for (i = KEYS_TO_ADD - 1; i > 0; i--) {
		swap_idx = rte_rand() % i;

		memcpy(temp_key, keys[i], hashtest_key_lens[table_index]);
		temp_signature = signatures[i];
		temp_position = positions[i];

		memcpy(keys[i], keys[swap_idx], hashtest_key_lens[table_index]);
		signatures[i] = signatures[swap_idx];
		positions[i] = positions[swap_idx];

		memcpy(keys[swap_idx], temp_key, hashtest_key_lens[table_index]);
		signatures[swap_idx] = temp_signature;
		positions[swap_idx] = temp_position;
	}
}

/*
 * Looks for random keys which
 * ALL can fit in hash table (no errors)
 */
static int
get_input_keys(unsigned with_pushes, unsigned table_index)
{
	unsigned i, j;
	unsigned bucket_idx, incr, success = 1;
	uint8_t k = 0;
	int32_t ret;
	const uint32_t bucket_bitmask = NUM_BUCKETS - 1;

	/* Reset all arrays */
	for (i = 0; i < MAX_ENTRIES; i++)
		slot_taken[i] = 0;

	for (i = 0; i < NUM_BUCKETS; i++)
		buckets[i] = 0;

	for (j = 0; j < hashtest_key_lens[table_index]; j++)
		keys[0][j] = 0;

	/*
	 * Add only entries that are not duplicated and that fits in the table
	 * (cannot store more than BUCKET_SIZE entries in a bucket).
	 * Regardless a key has been added correctly or not (success),
	 * the next one to try will be increased by 1.
	 */
	for (i = 0; i < KEYS_TO_ADD;) {
		incr = 0;
		if (i != 0) {
			keys[i][0] = ++k;
			/* Overflow, need to increment the next byte */
			if (keys[i][0] == 0)
				incr = 1;
			for (j = 1; j < hashtest_key_lens[table_index]; j++) {
				/* Do not increase next byte */
				if (incr == 0)
					if (success == 1)
						keys[i][j] = keys[i - 1][j];
					else
						keys[i][j] = keys[i][j];
				/* Increase next byte by one */
				else {
					if (success == 1)
						keys[i][j] = keys[i-1][j] + 1;
					else
						keys[i][j] = keys[i][j] + 1;
					if (keys[i][j] == 0)
						incr = 1;
					else
						incr = 0;
				}
			}
		}
		success = 0;
		signatures[i] = rte_hash_hash(h[table_index], keys[i]);
		bucket_idx = signatures[i] & bucket_bitmask;
		/*
		 * If we are not inserting keys in secondary location,
		 * when bucket is full, do not try to insert the key
		 */
		if (with_pushes == 0)
			if (buckets[bucket_idx] == BUCKET_SIZE)
				continue;

		/* If key can be added, leave in successful key arrays "keys" */
		ret = rte_hash_add_key_with_hash(h[table_index], keys[i],
						signatures[i]);
		if (ret >= 0) {
			/* If key is already added, ignore the entry and do not store */
			if (slot_taken[ret])
				continue;
			else {
				/* Store the returned position and mark slot as taken */
				slot_taken[ret] = 1;
				positions[i] = ret;
				buckets[bucket_idx]++;
				success = 1;
				i++;
			}
		}
	}

	/* Reset the table, so we can measure the time to add all the entries */
	rte_hash_free(h[table_index]);
	h[table_index] = rte_hash_create(&ut_params);

	return 0;
}

static int
timed_adds(unsigned with_hash, unsigned with_data, unsigned table_index)
{
	unsigned i;
	const uint64_t start_tsc = rte_rdtsc();
	void *data;
	int32_t ret;

	for (i = 0; i < KEYS_TO_ADD; i++) {
		data = (void *) ((uintptr_t) signatures[i]);
		if (with_hash && with_data) {
			ret = rte_hash_add_key_with_hash_data(h[table_index],
						(const void *) keys[i],
						signatures[i], data);
			if (ret < 0) {
				printf("Failed to add key number %u\n", ret);
				return -1;
			}
		} else if (with_hash && !with_data) {
			ret = rte_hash_add_key_with_hash(h[table_index],
						(const void *) keys[i],
						signatures[i]);
			if (ret >= 0)
				positions[i] = ret;
			else {
				printf("Failed to add key number %u\n", ret);
				return -1;
			}
		} else if (!with_hash && with_data) {
			ret = rte_hash_add_key_data(h[table_index],
						(const void *) keys[i],
						data);
			if (ret < 0) {
				printf("Failed to add key number %u\n", ret);
				return -1;
			}
		} else {
			ret = rte_hash_add_key(h[table_index], keys[i]);
			if (ret >= 0)
				positions[i] = ret;
			else {
				printf("Failed to add key number %u\n", ret);
				return -1;
			}
		}
	}

	const uint64_t end_tsc = rte_rdtsc();
	const uint64_t time_taken = end_tsc - start_tsc;

	cycles[table_index][ADD][with_hash][with_data] = time_taken/KEYS_TO_ADD;

	return 0;
}

static int
timed_lookups(unsigned with_hash, unsigned with_data, unsigned table_index)
{
	unsigned i, j;
	const uint64_t start_tsc = rte_rdtsc();
	void *ret_data;
	void *expected_data;
	int32_t ret;

	for (i = 0; i < NUM_LOOKUPS/KEYS_TO_ADD; i++) {
		for (j = 0; j < KEYS_TO_ADD; j++) {
			if (with_hash && with_data) {
				ret = rte_hash_lookup_with_hash_data(h[table_index],
							(const void *) keys[j],
							signatures[j], &ret_data);
				if (ret < 0) {
					printf("Key number %u was not found\n", j);
					return -1;
				}
				expected_data = (void *) ((uintptr_t) signatures[j]);
				if (ret_data != expected_data) {
					printf("Data returned for key number %u is %p,"
					       " but should be %p\n", j, ret_data,
						expected_data);
					return -1;
				}
			} else if (with_hash && !with_data) {
				ret = rte_hash_lookup_with_hash(h[table_index],
							(const void *) keys[j],
							signatures[j]);
				if (ret < 0 || ret != positions[j]) {
					printf("Key looked up in %d, should be in %d\n",
						ret, positions[j]);
					return -1;
				}
			} else if (!with_hash && with_data) {
				ret = rte_hash_lookup_data(h[table_index],
							(const void *) keys[j], &ret_data);
				if (ret < 0) {
					printf("Key number %u was not found\n", j);
					return -1;
				}
				expected_data = (void *) ((uintptr_t) signatures[j]);
				if (ret_data != expected_data) {
					printf("Data returned for key number %u is %p,"
					       " but should be %p\n", j, ret_data,
						expected_data);
					return -1;
				}
			} else {
				ret = rte_hash_lookup(h[table_index], keys[j]);
				if (ret < 0 || ret != positions[j]) {
					printf("Key looked up in %d, should be in %d\n",
						ret, positions[j]);
					return -1;
				}
			}
		}
	}

	const uint64_t end_tsc = rte_rdtsc();
	const uint64_t time_taken = end_tsc - start_tsc;

	cycles[table_index][LOOKUP][with_hash][with_data] = time_taken/NUM_LOOKUPS;

	return 0;
}

static int
timed_lookups_multi(unsigned with_data, unsigned table_index)
{
	unsigned i, j, k;
	int32_t positions_burst[BURST_SIZE];
	const void *keys_burst[BURST_SIZE];
	void *expected_data[BURST_SIZE];
	void *ret_data[BURST_SIZE];
	uint64_t hit_mask;
	int ret;

	const uint64_t start_tsc = rte_rdtsc();

	for (i = 0; i < NUM_LOOKUPS/KEYS_TO_ADD; i++) {
		for (j = 0; j < KEYS_TO_ADD/BURST_SIZE; j++) {
			for (k = 0; k < BURST_SIZE; k++)
				keys_burst[k] = keys[j * BURST_SIZE + k];
			if (with_data) {
				ret = rte_hash_lookup_bulk_data(h[table_index],
					(const void **) keys_burst,
					BURST_SIZE,
					&hit_mask,
					ret_data);
				if (ret != BURST_SIZE) {
					printf("Expect to find %u keys,"
					       " but found %d\n", BURST_SIZE, ret);
					return -1;
				}
				for (k = 0; k < BURST_SIZE; k++) {
					if ((hit_mask & (1ULL << k))  == 0) {
						printf("Key number %u not found\n",
							j * BURST_SIZE + k);
						return -1;
					}
					expected_data[k] = (void *) ((uintptr_t) signatures[j * BURST_SIZE + k]);
					if (ret_data[k] != expected_data[k]) {
						printf("Data returned for key number %u is %p,"
						       " but should be %p\n", j * BURST_SIZE + k,
							ret_data[k], expected_data[k]);
						return -1;
					}
				}
			} else {
				rte_hash_lookup_bulk(h[table_index],
						(const void **) keys_burst,
						BURST_SIZE,
						positions_burst);
				for (k = 0; k < BURST_SIZE; k++) {
					if (positions_burst[k] != positions[j * BURST_SIZE + k]) {
						printf("Key looked up in %d, should be in %d\n",
							positions_burst[k],
							positions[j * BURST_SIZE + k]);
						return -1;
					}
				}
			}
		}
	}

	const uint64_t end_tsc = rte_rdtsc();
	const uint64_t time_taken = end_tsc - start_tsc;

	cycles[table_index][LOOKUP_MULTI][0][with_data] = time_taken/NUM_LOOKUPS;

	return 0;
}

static int
timed_deletes(unsigned with_hash, unsigned with_data, unsigned table_index)
{
	unsigned i;
	const uint64_t start_tsc = rte_rdtsc();
	int32_t ret;

	for (i = 0; i < KEYS_TO_ADD; i++) {
		/* There are no delete functions with data, so just call two functions */
		if (with_hash)
			ret = rte_hash_del_key_with_hash(h[table_index],
							(const void *) keys[i],
							signatures[i]);
		else
			ret = rte_hash_del_key(h[table_index],
							(const void *) keys[i]);
		if (ret >= 0)
			positions[i] = ret;
		else {
			printf("Failed to add key number %u\n", ret);
			return -1;
		}
	}

	const uint64_t end_tsc = rte_rdtsc();
	const uint64_t time_taken = end_tsc - start_tsc;

	cycles[table_index][DELETE][with_hash][with_data] = time_taken/KEYS_TO_ADD;

	return 0;
}

static void
free_table(unsigned table_index)
{
	rte_hash_free(h[table_index]);
}

static void
reset_table(unsigned table_index)
{
	rte_hash_reset(h[table_index]);
}

static int
run_all_tbl_perf_tests(unsigned with_pushes)
{
	unsigned i, j, with_data, with_hash;

	printf("Measuring performance, please wait");
	fflush(stdout);

	for (with_data = 0; with_data <= 1; with_data++) {
		for (i = 0; i < NUM_KEYSIZES; i++) {
			if (create_table(with_data, i) < 0)
				return -1;

			if (get_input_keys(with_pushes, i) < 0)
				return -1;
			for (with_hash = 0; with_hash <= 1; with_hash++) {
				if (timed_adds(with_hash, with_data, i) < 0)
					return -1;

				for (j = 0; j < NUM_SHUFFLES; j++)
					shuffle_input_keys(i);

				if (timed_lookups(with_hash, with_data, i) < 0)
					return -1;

				if (timed_lookups_multi(with_data, i) < 0)
					return -1;

				if (timed_deletes(with_hash, with_data, i) < 0)
					return -1;

				/* Print a dot to show progress on operations */
				printf(".");
				fflush(stdout);

				reset_table(i);
			}
			free_table(i);
		}
	}

	printf("\nResults (in CPU cycles/operation)\n");
	printf("-----------------------------------\n");
	for (with_data = 0; with_data <= 1; with_data++) {
		if (with_data)
			printf("\n Operations with 8-byte data\n");
		else
			printf("\n Operations without data\n");
		for (with_hash = 0; with_hash <= 1; with_hash++) {
			if (with_hash)
				printf("\nWith pre-computed hash values\n");
			else
				printf("\nWithout pre-computed hash values\n");

			printf("\n%-18s%-18s%-18s%-18s%-18s\n",
			"Keysize", "Add", "Lookup", "Lookup_bulk", "Delete");
			for (i = 0; i < NUM_KEYSIZES; i++) {
				printf("%-18d", hashtest_key_lens[i]);
				for (j = 0; j < NUM_OPERATIONS; j++)
					printf("%-18"PRIu64, cycles[i][j][with_hash][with_data]);
				printf("\n");
			}
		}
	}
	return 0;
}

/* Control operation of performance testing of fbk hash. */
#define LOAD_FACTOR 0.667	/* How full to make the hash table. */
#define TEST_SIZE 1000000	/* How many operations to time. */
#define TEST_ITERATIONS 30	/* How many measurements to take. */
#define ENTRIES (1 << 15)	/* How many entries. */

static int
fbk_hash_perf_test(void)
{
	struct rte_fbk_hash_params params = {
		.name = "fbk_hash_test",
		.entries = ENTRIES,
		.entries_per_bucket = 4,
		.socket_id = rte_socket_id(),
	};
	struct rte_fbk_hash_table *handle = NULL;
	uint32_t *keys = NULL;
	unsigned indexes[TEST_SIZE];
	uint64_t lookup_time = 0;
	unsigned added = 0;
	unsigned value = 0;
	uint32_t key;
	uint16_t val;
	unsigned i, j;

	handle = rte_fbk_hash_create(&params);
	if (handle == NULL) {
		printf("Error creating table\n");
		return -1;
	}

	keys = rte_zmalloc(NULL, ENTRIES * sizeof(*keys), 0);
	if (keys == NULL) {
		printf("fbk hash: memory allocation for key store failed\n");
		return -1;
	}

	/* Generate random keys and values. */
	for (i = 0; i < ENTRIES; i++) {
		key = (uint32_t)rte_rand();
		key = ((uint64_t)key << 32) | (uint64_t)rte_rand();
		val = (uint16_t)rte_rand();

		if (rte_fbk_hash_add_key(handle, key, val) == 0) {
			keys[added] = key;
			added++;
		}
		if (added > (LOAD_FACTOR * ENTRIES))
			break;
	}

	for (i = 0; i < TEST_ITERATIONS; i++) {
		uint64_t begin;
		uint64_t end;

		/* Generate random indexes into keys[] array. */
		for (j = 0; j < TEST_SIZE; j++)
			indexes[j] = rte_rand() % added;

		begin = rte_rdtsc();
		/* Do lookups */
		for (j = 0; j < TEST_SIZE; j++)
			value += rte_fbk_hash_lookup(handle, keys[indexes[j]]);

		end = rte_rdtsc();
		lookup_time += (double)(end - begin);
	}

	printf("\n\n *** FBK Hash function performance test results ***\n");
	/*
	 * The use of the 'value' variable ensures that the hash lookup is not
	 * being optimised out by the compiler.
	 */
	if (value != 0)
		printf("Number of ticks per lookup = %g\n",
			(double)lookup_time /
			((double)TEST_ITERATIONS * (double)TEST_SIZE));

	rte_fbk_hash_free(handle);

	return 0;
}

static int
test_hash_perf(void)
{
	unsigned with_pushes;

	for (with_pushes = 0; with_pushes <= 1; with_pushes++) {
		if (with_pushes == 0)
			printf("\nALL ELEMENTS IN PRIMARY LOCATION\n");
		else
			printf("\nELEMENTS IN PRIMARY OR SECONDARY LOCATION\n");
		if (run_all_tbl_perf_tests(with_pushes) < 0)
			return -1;
	}
	if (fbk_hash_perf_test() < 0)
		return -1;

	return 0;
}

static struct test_command hash_perf_cmd = {
		.command = "hash_perf_autotest",
		.callback = test_hash_perf,
};
REGISTER_TEST_COMMAND(hash_perf_cmd);
