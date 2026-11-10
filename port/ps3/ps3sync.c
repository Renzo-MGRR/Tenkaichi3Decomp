/*
 * Newlib's default lock hooks create LV2 lightweight mutexes. During RPCS3
 * startup their kernel-side lock state faults in 0xD000xxxx. Use PPU atomics
 * for libc's short critical sections while leaving PSL1GHT mutex APIs intact.
 * r13 is the PPU thread pointer and supplies a per-thread owner token.
 */
#include <errno.h>
#include <stdint.h>
#include <sys/lock.h>

static uint64_t current_thread_token(void) {
    uint64_t token;
    __asm__ volatile("mr %0,13" : "=r"(token));
    return token != 0 ? token : 1;
}

static int lock_try(__libc_lock_t *lock, int recursive) {
    uint64_t owner, expected = 0;
    if (lock == 0) return EINVAL;
    owner = current_thread_token();
    if (__atomic_load_n(&lock->lock_var, __ATOMIC_RELAXED) == owner) {
        if (!recursive) return EBUSY;
        lock->recursive_count++;
        return 0;
    }
    if (!__atomic_compare_exchange_n(&lock->lock_var, &expected, owner, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return EBUSY;
    }
    lock->recursive_count = 0;
    return 0;
}

static int lock_acquire(__libc_lock_t *lock, int recursive) {
    int rc;
    if (lock == 0) return EINVAL;
    for (;;) {
        rc = lock_try(lock, recursive);
        if (rc == 0) return 0;
        if (rc != EBUSY) return rc;
        __asm__ volatile("" ::: "memory");
    }
}

static int lock_release(__libc_lock_t *lock) {
    if (lock == 0) return EINVAL;
    if (__atomic_load_n(&lock->lock_var, __ATOMIC_RELAXED) != current_thread_token()) return EPERM;
    if (lock->recursive_count != 0) {
        lock->recursive_count--;
        return 0;
    }
    __atomic_store_n(&lock->lock_var, 0, __ATOMIC_RELEASE);
    return 0;
}

static int lock_close(__libc_lock_t *lock) {
    if (lock == 0) return EINVAL;
    if (__atomic_load_n(&lock->lock_var, __ATOMIC_ACQUIRE) != 0) return EBUSY;
    lock->attribute = 0;
    lock->recursive_count = 0;
    lock->sleep_queue = 0;
    lock->_pad = 0;
    return 0;
}

int __libc_lock_init(__libc_lock_t *lock) {
    if (lock == 0) return EINVAL;
    __atomic_store_n(&lock->lock_var, 0, __ATOMIC_RELAXED);
    lock->attribute = 0;
    lock->recursive_count = 0;
    lock->sleep_queue = 0;
    lock->_pad = 0;
    return 0;
}

int __libc_lock_init_recursive(__libc_lock_recursive_t *lock) {
    return __libc_lock_init(lock);
}

int __libc_lock_close(__libc_lock_t *lock) { return lock_close(lock); }
int __libc_lock_close_recursive(__libc_lock_recursive_t *lock) { return lock_close(lock); }
int __libc_lock_acquire(__libc_lock_t *lock) { return lock_acquire(lock, 0); }
int __libc_lock_acquire_recursive(__libc_lock_recursive_t *lock) { return lock_acquire(lock, 1); }
int __libc_lock_try_acquire(__libc_lock_t *lock) { return lock_try(lock, 0); }
int __libc_lock_try_acquire_recursive(__libc_lock_recursive_t *lock) { return lock_try(lock, 1); }
int __libc_lock_release(__libc_lock_t *lock) { return lock_release(lock); }
int __libc_lock_release_recursive(__libc_lock_recursive_t *lock) { return lock_release(lock); }
