/* SPDX-License-Identifier: Apache-2.0 */
/* ESP-IDF libc locks without M-mode xPortCanYield() CSR reads. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/lock.h>

#include "esp_crypto_periph_clk.h"
#include "hal/sha_ll.h"
#include "s31_rtos/s31_rtos.h"

#define S31_LOCK_SLOTS 32

#define S31_SHA_IDF 1U
#define S31_SHA_LINUX 2U
#define S31_SHA_BLOCK_TIMEOUT_NS 100000U

extern _lock_t s_crypto_sha_aes_lock;
static uint32_t s31_sha_owner;

/*
 * ESP32-S31 TRM ch.17 SHA Accelerator: HMAC and DS share SHA_H_MEM
 * and can reset a SHA operation. The IDF esp_crypto_lock.c SHA/AES
 * mutex covers both users before their clock/reset sequences start.
 */
static void s31_sha_idf_acquire(void)
{
    __atomic_fetch_or(&s31_sha_owner, S31_SHA_IDF, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&s31_sha_owner, __ATOMIC_ACQUIRE) & S31_SHA_LINUX)
        __asm__ volatile ("nop");
}

/*
 * Called by Linux with IRQs masked. Hold the lease for one SHA-256
 * block with caller-supplied SHA_H_MEM chaining words.
 */
int s31_radio_sha256_compress(uint32_t state[8], const uint32_t block[16])
{
    uint32_t idle = 0;
    uint64_t start;
    int ret = 0;

    if (!__atomic_compare_exchange_n(&s31_sha_owner, &idle, S31_SHA_LINUX,
                                     false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return -EBUSY;

    esp_crypto_sha_enable_periph_clk(true);
    sha_ll_set_mode(SHA2_256);
    sha_ll_write_digest(SHA2_256, state, 8);
    sha_ll_fill_text_block(block, 16);
    sha_ll_continue_block(SHA2_256);
    start = s31_linux_time_ns();
    while (sha_ll_busy()) {
        if (s31_linux_time_ns() - start >= S31_SHA_BLOCK_TIMEOUT_NS) {
            ret = -ETIMEDOUT;
            break;
        }
    }
    if (!ret)
        sha_ll_read_digest(SHA2_256, state, 8);
    esp_crypto_sha_enable_periph_clk(false);
    __atomic_fetch_and(&s31_sha_owner, ~S31_SHA_LINUX, __ATOMIC_RELEASE);
    return ret;
}

struct __lock __lock___libc_recursive_mutex;
struct __lock __lock___sinit_recursive_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

struct s31_lock_entry {
	struct __lock *key;
	void *handle;
	uint8_t type;
};

static struct s31_lock_entry s31_locks[S31_LOCK_SLOTS];

static void *s31_lock_get(struct __lock *lock, uint8_t type)
{
	void *handle;
	unsigned int i, free_slot = S31_LOCK_SLOTS;

	if (!lock)
		return NULL;

	vPortEnterCritical();
	for (i = 0; i < S31_LOCK_SLOTS; i++) {
		if (s31_locks[i].key == lock) {
			handle = s31_locks[i].handle;
			vPortExitCritical();
			return handle;
		}
		if (!s31_locks[i].key && free_slot == S31_LOCK_SLOTS)
			free_slot = i;
	}
	if (free_slot == S31_LOCK_SLOTS) {
		vPortExitCritical();
		return NULL;
	}
	handle = xQueueCreateMutex(type);
	if (handle) {
		s31_locks[free_slot].key = lock;
		s31_locks[free_slot].handle = handle;
		s31_locks[free_slot].type = type;
	}
	vPortExitCritical();
	return handle;
}

static struct s31_lock_entry *s31_lock_find(struct __lock *lock)
{
	unsigned int i;

	for (i = 0; i < S31_LOCK_SLOTS; i++)
		if (s31_locks[i].key == lock)
			return &s31_locks[i];
	return NULL;
}

static int s31_lock_take(struct __lock *lock, TickType_t timeout,
			 uint8_t type)
{
	void *handle = s31_lock_get(lock, type);

	if (!handle)
		return -1;
	if (s31_rtos_in_isr())
		return xQueueReceiveFromISR(handle, NULL, NULL) == pdTRUE ? 0 : -1;
	if (type == S31_MUTEX_RECURSIVE)
		return xQueueTakeMutexRecursive(handle, timeout) == pdTRUE ? 0 : -1;
	return xQueueSemaphoreTake(handle, timeout) == pdTRUE ? 0 : -1;
}

static void s31_lock_require(struct __lock *lock, uint8_t type)
{
    if (s31_lock_take(lock, portMAX_DELAY, type) < 0) {
        /* A void libc acquire cannot grant an unprotected continuation. */
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        __builtin_unreachable();
    }
}

static void s31_lock_give(struct __lock *lock)
{
    struct s31_lock_entry *entry = s31_lock_find(lock);
    if (!entry) return;
    if (s31_rtos_in_isr()) xQueueGiveFromISR(entry->handle, NULL);
    else if (entry->type == S31_MUTEX_RECURSIVE)
        xQueueGiveMutexRecursive(entry->handle);
    else
        xQueueGenericSend(entry->handle, NULL, 0, S31_QUEUE_SEND_TO_BACK);
}

void _lock_init(_lock_t *lock)
{
	if (!lock)
		return;
	*lock = s31_rtos_malloc(sizeof(**lock));
	if (*lock)
		memset(*lock, 0, sizeof(**lock));
}

void _lock_init_recursive(_lock_t *lock)
{
	_lock_init(lock);
}

void _lock_close(_lock_t *lock)
{
	if (lock && *lock) {
		unsigned int i;

		for (i = 0; i < S31_LOCK_SLOTS; i++) {
			if (s31_locks[i].key == *lock) {
				vQueueDelete(s31_locks[i].handle);
				memset(&s31_locks[i], 0, sizeof(s31_locks[i]));
				break;
			}
		}
		s31_rtos_free(*lock);
		*lock = NULL;
	}
}

void _lock_close_recursive(_lock_t *lock)
{
	_lock_close(lock);
}

void _lock_acquire(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init(lock);
	s31_lock_require(lock ? *lock : NULL, S31_MUTEX_NORMAL);
	if (lock == &s_crypto_sha_aes_lock)
		s31_sha_idf_acquire();
}

void _lock_acquire_recursive(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init_recursive(lock);
	s31_lock_require(lock ? *lock : NULL, S31_MUTEX_RECURSIVE);
}

int _lock_try_acquire(_lock_t *lock)
{
	int ret;

	if (lock && !*lock)
		_lock_init(lock);
	ret = lock && *lock ? s31_lock_take(*lock, 0, S31_MUTEX_NORMAL) : -1;

	if (!ret && lock == &s_crypto_sha_aes_lock) {
		uint32_t idle = 0;

		if (!__atomic_compare_exchange_n(&s31_sha_owner, &idle, S31_SHA_IDF,
		                                 false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			s31_lock_give(*lock);
			ret = -1;
		}
	}
	return ret;
}

int _lock_try_acquire_recursive(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init_recursive(lock);
	return lock && *lock ? s31_lock_take(*lock, 0, S31_MUTEX_RECURSIVE) : -1;
}

void _lock_release(_lock_t *lock)
{
	if (lock == &s_crypto_sha_aes_lock)
		__atomic_fetch_and(&s31_sha_owner, ~S31_SHA_IDF, __ATOMIC_RELEASE);
	if (lock && *lock)
		s31_lock_give(*lock);
}

void _lock_release_recursive(_lock_t *lock)
{
	_lock_release(lock);
}

void __retarget_lock_init(_LOCK_T *lock)
{
	_lock_init(lock);
	if (lock && *lock)
		s31_lock_get(*lock, S31_MUTEX_NORMAL);
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
	_lock_init_recursive(lock);
	if (lock && *lock)
		s31_lock_get(*lock, S31_MUTEX_RECURSIVE);
}

void __retarget_lock_close(_LOCK_T lock) { _lock_close(&lock); }
void __retarget_lock_close_recursive(_LOCK_T lock) { _lock_close(&lock); }
void __retarget_lock_acquire(_LOCK_T lock)
{
	s31_lock_require(lock, S31_MUTEX_NORMAL);
}
void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
	s31_lock_require(lock, S31_MUTEX_RECURSIVE);
}
int __retarget_lock_try_acquire(_LOCK_T lock)
{
	return s31_lock_take(lock, 0, S31_MUTEX_NORMAL);
}
int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
	return s31_lock_take(lock, 0, S31_MUTEX_RECURSIVE);
}
void __retarget_lock_release(_LOCK_T lock) { s31_lock_give(lock); }
void __retarget_lock_release_recursive(_LOCK_T lock) { s31_lock_give(lock); }

void esp_libc_locks_init(void)
{
	memset(s31_locks, 0, sizeof(s31_locks));
	s31_lock_get(&__lock___libc_recursive_mutex, S31_MUTEX_RECURSIVE);
}

void esp_newlib_locks_init(void) __attribute__((alias("esp_libc_locks_init")));
