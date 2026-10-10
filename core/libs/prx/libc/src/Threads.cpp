#include "prx/libc/include/general/VabiMacros.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

namespace {

constexpr int ThrdSuccess = 0;
constexpr int ThrdNomem = 1;
constexpr int ThrdBusy = 3;
constexpr int MtxRecursive = 0x100;

class GuestMutex {
public:
    explicit GuestMutex(bool recursive) : recursive(recursive) {}

    int Lock() {
        const auto self = std::this_thread::get_id();
        if (owner.load(std::memory_order_acquire) == self) {
            if (!recursive) return ThrdBusy;
            ++depth;
            return ThrdSuccess;
        }
        native.lock();
        owner.store(self, std::memory_order_release);
        depth = 1;
        return ThrdSuccess;
    }

    void Unlock() {
        if (owner.load(std::memory_order_acquire) != std::this_thread::get_id())
            throw std::runtime_error("_Mtx_unlock: mutex is not owned by the current thread");
        if (--depth > 0) return;
        owner.store(std::thread::id{}, std::memory_order_release);
        native.unlock();
    }

    void lock() {
        native.lock();
        owner.store(std::this_thread::get_id(), std::memory_order_release);
    }

    void unlock() {
        owner.store(std::thread::id{}, std::memory_order_release);
        native.unlock();
    }

    int Release() {
        if (owner.load(std::memory_order_acquire) != std::this_thread::get_id())
            throw std::runtime_error("_Cnd_wait: mutex is not owned by the current thread");
        const int held = depth;
        depth = 0;
        return held;
    }

    void Restore(int held) { depth = held; }

private:
    std::mutex native;
    std::atomic<std::thread::id> owner{};
    int depth = 0;
    const bool recursive;
};

GuestMutex* RequireMutex(GuestMutex* const* slot) {
    if (slot == nullptr || *slot == nullptr) throw std::invalid_argument("_Mtx: mutex is not initialized");
    return *slot;
}

std::condition_variable_any* RequireCond(std::condition_variable_any* const* slot) {
    if (slot == nullptr || *slot == nullptr) throw std::invalid_argument("_Cnd: condition variable is not initialized");
    return *slot;
}

}

extern "C" {

int APS5_VABI _Mtx_init_nid_postfix(GuestMutex** slot, int type) {
    if (slot == nullptr) throw std::invalid_argument("_Mtx_init: null mutex");
    *slot = new (std::nothrow) GuestMutex((type & MtxRecursive) != 0);
    return *slot == nullptr ? ThrdNomem : ThrdSuccess;
}

void APS5_VABI _Mtx_destroy_nid_postfix(GuestMutex** slot) {
    if (slot == nullptr) throw std::invalid_argument("_Mtx_destroy: null mutex");
    delete *slot;
    *slot = nullptr;
}

int APS5_VABI _Mtx_lock_nid_postfix(GuestMutex** slot) {
    return RequireMutex(slot)->Lock();
}

int APS5_VABI _Mtx_unlock_nid_postfix(GuestMutex** slot) {
    RequireMutex(slot)->Unlock();
    return ThrdSuccess;
}

int APS5_VABI _Cnd_init_nid_postfix(std::condition_variable_any** slot) {
    if (slot == nullptr) throw std::invalid_argument("_Cnd_init: null condition variable");
    *slot = new (std::nothrow) std::condition_variable_any();
    return *slot == nullptr ? ThrdNomem : ThrdSuccess;
}

void APS5_VABI _Cnd_destroy_nid_postfix(std::condition_variable_any** slot) {
    if (slot == nullptr) throw std::invalid_argument("_Cnd_destroy: null condition variable");
    delete *slot;
    *slot = nullptr;
}

int APS5_VABI _Cnd_wait_nid_postfix(std::condition_variable_any** cond, GuestMutex** mutex) {
    auto* condition = RequireCond(cond);
    auto* guestMutex = RequireMutex(mutex);
    const int held = guestMutex->Release();
    std::unique_lock lock(*guestMutex, std::adopt_lock);
    condition->wait(lock);
    lock.release();
    guestMutex->Restore(held);
    return ThrdSuccess;
}

int APS5_VABI _Cnd_broadcast_nid_postfix(std::condition_variable_any** cond) {
    RequireCond(cond)->notify_all();
    return ThrdSuccess;
}

}
