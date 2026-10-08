#include <mutex>

extern "C" {

static std::mutex drain_mutex;
void init_drain_mutex(){
}
void lock_drain_mutex(){
	drain_mutex.lock();
}
void unlock_drain_mutex(){
	drain_mutex.unlock();
}

}
