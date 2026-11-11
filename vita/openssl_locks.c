/* OpenSSL 1.1.1 creates a lock for every certificate, store and context.
   VitaSDK's pthread rwlock spends several kernel semaphores on each, and
   loading the CA bundle exhausts them: certificates stop loading and
   SSL_CTX_new fails ("curl 27"). Linked with --wrap, OpenSSL's lock calls
   use one native lightweight mutex per lock instead (the approach of
   devnoname120/vita_tls_fixer). Readers and writers share that mutex. */
#include <openssl/crypto.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>

typedef struct {
    SceKernelLwMutexWork work;
} VitaCryptoLock;

CRYPTO_RWLOCK *__wrap_CRYPTO_THREAD_lock_new(void) {
    VitaCryptoLock *lock = calloc(1, sizeof(*lock));
    if (!lock) return NULL;
    if (sceKernelCreateLwMutex(&lock->work, "uaplay-openssl",
                               SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0) {
        free(lock);
        return NULL;
    }
    return (CRYPTO_RWLOCK *)lock;
}

int __wrap_CRYPTO_THREAD_read_lock(CRYPTO_RWLOCK *opaque) {
    VitaCryptoLock *lock = (VitaCryptoLock *)opaque;
    return lock && sceKernelLockLwMutex(&lock->work, 1, NULL) >= 0;
}

int __wrap_CRYPTO_THREAD_write_lock(CRYPTO_RWLOCK *opaque) {
    return __wrap_CRYPTO_THREAD_read_lock(opaque);
}

int __wrap_CRYPTO_THREAD_unlock(CRYPTO_RWLOCK *opaque) {
    VitaCryptoLock *lock = (VitaCryptoLock *)opaque;
    return lock && sceKernelUnlockLwMutex(&lock->work, 1) >= 0;
}

void __wrap_CRYPTO_THREAD_lock_free(CRYPTO_RWLOCK *opaque) {
    VitaCryptoLock *lock = (VitaCryptoLock *)opaque;
    /* A mutex that cannot be deleted keeps its storage. */
    if (lock && sceKernelDeleteLwMutex(&lock->work) >= 0) free(lock);
}

/* Reference counts: an atomic instruction, no lock needed. */
int __wrap_CRYPTO_atomic_add(int *value, int amount, int *result, CRYPTO_RWLOCK *lock) {
    (void)lock;
    if (!value || !result) return 0;
    *result = __atomic_add_fetch(value, amount, __ATOMIC_SEQ_CST);
    return 1;
}
