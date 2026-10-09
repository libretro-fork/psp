#define VMA_IMPLEMENTATION

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "Common/GPU/Vulkan/VulkanLoader.h"

using namespace PPSSPP_VK;

// The allocator has one owning thread (see VulkanContext::CreateDevice), so VMA gets no-op
// mutexes and plain counters instead of std::mutex and std::atomic.
#include <cassert>
#include <cstdint>
#define VMA_CONFIGURATION_USER_INCLUDES_H <algorithm>

class VmaOwnerMutex {
public:
	void Lock() {}
	void Unlock() {}
	bool TryLock() { return true; }
	void LockRead() {}
	void UnlockRead() {}
	bool TryLockRead() { return true; }
	void LockWrite() {}
	void UnlockWrite() {}
	bool TryLockWrite() { return true; }
};

template <typename T>
class VmaOwnerValue {
public:
	VmaOwnerValue() : v_() {}
	VmaOwnerValue(T v) : v_(v) {}
	VmaOwnerValue(const VmaOwnerValue &) = delete;
	VmaOwnerValue &operator=(const VmaOwnerValue &) = delete;
	T load() const { return v_; }
	void store(T v) { v_ = v; }
	T fetch_add(T d) { T o = v_; v_ += d; return o; }
	T fetch_sub(T d) { T o = v_; v_ -= d; return o; }
	bool compare_exchange_strong(T &expected, T desired) {
		if (v_ == expected) {
			v_ = desired;
			return true;
		}
		expected = v_;
		return false;
	}
	bool compare_exchange_weak(T &expected, T desired) { return compare_exchange_strong(expected, desired); }
	T operator=(T v) { v_ = v; return v; }
	operator T() const { return v_; }
	T operator++() { return ++v_; }
	T operator--() { return --v_; }
	T operator++(int) { return v_++; }
	T operator--(int) { return v_--; }
	T operator+=(T d) { return v_ += d; }
	T operator-=(T d) { return v_ -= d; }
private:
	T v_;
};

#define VMA_MUTEX VmaOwnerMutex
#define VMA_RW_MUTEX VmaOwnerMutex
#define VMA_ATOMIC_UINT32 VmaOwnerValue<uint32_t>
#define VMA_ATOMIC_UINT64 VmaOwnerValue<uint64_t>
#define VMA_ATOMIC_BOOL VmaOwnerValue<bool>

#undef VK_NO_PROTOTYPES
#include "vk_mem_alloc.h"
#define VK_NO_PROTOTYPES


// This chunk should be added to vk_mem_alloc.h when upgrading, right below the #ifndef at the top:

/*

// BEGIN PPSSPP HACKS !!!!!
#ifdef USE_CRT_DBG
#undef new
#endif

#if defined(__APPLE__)
#include <AvailabilityMacros.h>

#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && (!defined(__IPHONE_10_0) || __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_10_0)
#define VMA_USE_STL_SHARED_MUTEX 0
#endif
#if defined(MAC_OS_X_VERSION_MIN_REQUIRED) && (!defined(MAC_OS_X_VERSION_10_12) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12)
#define VMA_USE_STL_SHARED_MUTEX 0
#endif

#endif
// END PPSSPP HACKS


*/
