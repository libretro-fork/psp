#pragma once
#include <cstdint>

void TimeInit();
void TimeShutdown();  // Not really necessary to call, just for completeness.

// Seconds.
double time_now_d();

// Raw time in nanoseconds.
// The only intended use is to match the timings from VK_GOOGLE_display_timing.
uint64_t time_now_raw();

// This is only interesting for Linux, in relation to VK_GOOGLE_display_timing.
double from_time_raw(uint64_t raw_time);
double from_time_raw_relative(uint64_t raw_time);

// Seconds, Unix UTC time
double time_now_unix_utc();
double time_to_unix_utc(double timeNowSeconds);

#ifndef __LIBRETRO__
// Sleeps, for the standalone frontends, headless tools and tests. The core never sleeps and
// doesn't have this, so a sleep can't creep back into it.
void sleep_ms(int ms, const char *reason);
void sleep_us(int us, const char *reason);
#endif

void GetCurrentTimeFormatted(char formattedTime[13]);

// Most accurate timer possible - no extra double conversions. Only for spans.
class Instant {
public:
	Instant();
	static Instant Now() {
		return Instant();
	}
	double ElapsedSeconds() const;
	double ElapsedMs() const { return ElapsedSeconds() * 1000.0; }
	int64_t ElapsedNanos() const;
private:
	uint64_t nativeStart_;
#ifndef _WIN32
	int64_t nsecs_;
#endif
};

class TimeCollector {
public:
	TimeCollector(double *target, bool enable) : target_(enable ? target : nullptr) {
		if (enable)
			startTime_ = time_now_d();
	}
	~TimeCollector() {
		if (target_) {
			*target_ += time_now_d() - startTime_;
		}
	}
private:
	double startTime_;
	double *target_;
};
