#include <retro_atomic.h>

#include "Common/GPU/GPUBackendCommon.h"

// Global push buffer tracker for GPU memory profiling.
// Don't want to manually dig up all the active push buffers.
// Slots are claimed and cleared with CAS; the list is only read for the debug overlay.
enum { MAX_PUSH_BUFFERS = 128 };
static retro_atomic_ptr_t g_pushBuffers[MAX_PUSH_BUFFERS];

std::vector<GPUMemoryManager *> GetActiveGPUMemoryManagers() {
	std::vector<GPUMemoryManager *> buffers;
	for (int i = 0; i < MAX_PUSH_BUFFERS; i++) {
		GPUMemoryManager *manager = (GPUMemoryManager *)retro_atomic_load_acquire_ptr(&g_pushBuffers[i]);
		if (manager)
			buffers.push_back(manager);
	}
	return buffers;
}

void RegisterGPUMemoryManager(GPUMemoryManager *manager) {
	for (int i = 0; i < MAX_PUSH_BUFFERS; i++) {
		if (retro_atomic_cas_ptr(&g_pushBuffers[i], nullptr, (void *)manager))
			return;
	}
	// Full: this one just won't show in the overlay.
}

void UnregisterGPUMemoryManager(GPUMemoryManager *manager) {
	for (int i = 0; i < MAX_PUSH_BUFFERS; i++) {
		if (retro_atomic_cas_ptr(&g_pushBuffers[i], (void *)manager, nullptr))
			return;
	}
}
