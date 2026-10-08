#include "ppsspp_config.h"

#include <cstdio>
#include <cstdint>

#include "Common/TimeUtil.h"
#include "Common/Log.h"

#ifdef HAVE_LIBNX
#include <switch.h>
#endif // HAVE_LIBNX

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif // __EMSCRIPTEN__

#ifdef _WIN32
#include "CommonWindows.h"
#include <sys/timeb.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include <ctime>

// TODO: https://github.com/floooh/sokol/blob/9a6237fcdf213e6da48e4f9201f144bcb2dcb46f/sokol_time.h#L229-L248

constexpr double micros = 1000000.0;
constexpr double nanos = 1000000000.0;


#if PPSSPP_PLATFORM(WINDOWS)

constexpr int64_t UNIX_TIME_START = 0x019DB1DED53E8000; //January 1, 1970 (start of Unix epoch) in "ticks"
constexpr double TICKS_PER_SECOND = 10000000; //a tick is 100ns

static LARGE_INTEGER frequency;
static double frequencyMult;
static LARGE_INTEGER startTime;
static LARGE_INTEGER startFileTime;

void TimeInit() {
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft); //returns ticks in UTC
	// Copy the low and high parts of FILETIME into a LARGE_INTEGER
	startFileTime.LowPart = ft.dwLowDateTime;
	startFileTime.HighPart = ft.dwHighDateTime;

	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&startTime);
	frequencyMult = 1.0 / frequency.QuadPart;
	// No timeBeginPeriod: nothing sleeps, so the process's timer resolution is left alone.
}

void TimeShutdown() {
}

double time_now_d() {
	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);
	return static_cast<double>(time.QuadPart - startTime.QuadPart) * frequencyMult;
}

// Fake, but usable in a pinch. Don't, though.
uint64_t time_now_raw() {
	return (uint64_t)(time_now_d() * nanos);
}

double from_time_raw(uint64_t raw_time) {
	if (raw_time == 0) {
		return 0.0; // invalid time
	}
	return (double)raw_time * (1.0 / nanos);
}

double from_time_raw_relative(uint64_t raw_time) {
	return from_time_raw(raw_time);
}

double time_now_unix_utc() {
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft); //returns ticks in UTC
	// Copy the low and high parts of FILETIME into a LARGE_INTEGER
	LARGE_INTEGER li;
	li.LowPart = ft.dwLowDateTime;
	li.HighPart = ft.dwHighDateTime;
	//Convert ticks since 1/1/1970 into seconds
	return (double)(li.QuadPart - UNIX_TIME_START) / TICKS_PER_SECOND;
}

// Adds the timestamp to startTime, and converts to seconds from the unix epoch.
double time_to_unix_utc(double timestamp) {
	// Copy the low and high parts of FILETIME into a LARGE_INTEGER
	LARGE_INTEGER li;
	li.LowPart = startFileTime.LowPart;
	li.HighPart = startFileTime.HighPart;
	return (double)(li.QuadPart - UNIX_TIME_START + static_cast<int64_t>(timestamp * TICKS_PER_SECOND)) / TICKS_PER_SECOND;
}

Instant::Instant() {
	_dbg_assert_(frequencyMult != 0.0);
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER *>(&nativeStart_));
}

double Instant::ElapsedSeconds() const {
	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);
	double elapsed = static_cast<double>(time.QuadPart - nativeStart_);
	return elapsed * frequencyMult;
}

int64_t Instant::ElapsedNanos() const {
	return (int64_t)(ElapsedSeconds() * 1000000000.0);
}

#elif PPSSPP_PLATFORM(ANDROID) || PPSSPP_PLATFORM(LINUX) || PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)

void TimeInit() {
	// Nothing to do.
}

void TimeShutdown() {
	// Nothing to do.
}

// The only intended use is to match the timings in VK_GOOGLE_display_timing
uint64_t time_now_raw() {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return tp.tv_sec * 1000000000ULL + tp.tv_nsec;
}

static uint64_t g_startTime;

double from_time_raw(uint64_t raw_time) {
	return (double)(raw_time - g_startTime) * (1.0 / nanos);
}

double time_now_d() {
	uint64_t raw_time = time_now_raw();
	if (g_startTime == 0) {
		g_startTime = raw_time;
	}
	return from_time_raw(raw_time);
}

double from_time_raw_relative(uint64_t raw_time) {
	return (double)raw_time * (1.0 / nanos);
}

double time_now_unix_utc() {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	return (double)tp.tv_sec + (double)tp.tv_nsec / 1000000000.0;
}

Instant::Instant() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	nativeStart_ = ts.tv_sec;
	nsecs_ = ts.tv_nsec;
}

int64_t Instant::ElapsedNanos() const {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t secs = ts.tv_sec - nativeStart_;
	int64_t nsecs = ts.tv_nsec - nsecs_;
	if (nsecs < 0) {
		secs--;
		nsecs += 1000000000;
	}
	return secs * 1000000000ULL + nsecs;
}

double Instant::ElapsedSeconds() const {
	return (double)ElapsedNanos() * (1.0 / nanos);
}

#else

void TimeInit() {
	// Nothing to do.
}

void TimeShutdown() {
	// Nothing to do.
}

static time_t start;

double time_now_d() {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	if (start == 0) {
		start = tv.tv_sec;
	}
	return (double)(tv.tv_sec - start) + (double)tv.tv_usec * (1.0 / micros);
}

uint64_t time_now_raw() {
	// Nanoseconds, like the other platforms - from_time_raw() scales by 1/nanos. This used to
	// build a double of seconds and return it through the uint64_t, so it both lost the fraction
	// and was off by a factor of a billion.
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (uint64_t)tp.tv_sec * 1000000000ULL + tp.tv_nsec;
}

double from_time_raw(uint64_t raw_time) {
	return (double)raw_time * (1.0 / nanos);
}

double from_time_raw_relative(uint64_t raw_time) {
	return from_time_raw(raw_time);
}

double time_now_unix_utc() {
	// Not time_now_raw() - that's a monotonic clock with no relation to the epoch.
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (double)tv.tv_sec + (double)tv.tv_usec * (1.0 / micros);
}

double time_to_unix_utc(double t) {
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (double)tv.tv_sec + (double)tv.tv_usec * (1.0 / micros) + t;
}

Instant::Instant() {
	// Has to be the same clock, and the same unit, as ElapsedNanos() below: this took the wall
	// clock in microseconds while that one subtracts it from a monotonic clock in nanoseconds.
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	nativeStart_ = ts.tv_sec;
	nsecs_ = ts.tv_nsec;
}

int64_t Instant::ElapsedNanos() const {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	int64_t secs = ts.tv_sec - nativeStart_;
	int64_t nsecs = ts.tv_nsec - nsecs_;
	if (nsecs < 0) {
		secs--;
		nsecs += 1000000000;
	}
	return secs * 1000000000 + nsecs;
}

double Instant::ElapsedSeconds() const {
	return (double)ElapsedNanos() * (1.0 / 1000000000.0);
}

#endif

#ifndef __LIBRETRO__
// The headless tools and tests only; see TimeUtil.h.
void sleep_ms(int ms, const char *reason) {
	if (ms <= 0) {
		return;
	}
#if SLEEP_LOG_ENABLED
	INFO_LOG(Log::System, "Sleep %d ms: %s", ms, reason);
#endif
#ifdef _WIN32
	Sleep(ms);
#elif defined(HAVE_LIBNX)
	svcSleepThread(ms * 1000000);
#elif defined(__EMSCRIPTEN__)
	emscripten_sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

void sleep_us(int us, const char *reason) {
	if (us <= 0) {
		return;
	}
#if SLEEP_LOG_ENABLED
	INFO_LOG(Log::System, "Sleep %d us: %s", us, reason);
#endif
#ifdef _WIN32
	Sleep(us / 1000);
#elif defined(HAVE_LIBNX)
	svcSleepThread(us * 1000);
#elif defined(__EMSCRIPTEN__)
	emscripten_sleep(us / 1000);
#else
	usleep(us);
#endif
}
#endif

// Return the current time formatted as Minutes:Seconds:Milliseconds
// in the form 00:00:000.
void GetCurrentTimeFormatted(char formattedTime[13]) {
#ifdef _WIN32
	SYSTEMTIME st;
	GetLocalTime(&st);
	snprintf(formattedTime, 13, "%02d:%02d:%03d", st.wMinute, st.wSecond, st.wMilliseconds);
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	snprintf(formattedTime, 13, "%02d:%02d:%03d", tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
#endif
}

