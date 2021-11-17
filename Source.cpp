//#include "Scheduler.h"
//#include "JobSystem.h"
#include "Atomics.h"
#include <random>
#include <concepts>
#include <limits>
#include <assert.h>
#include <thread>
#include <numbers>
#include <vector>
#include <iostream>

constexpr size_t operator"" uz(size_t s) { return s; }


using uint = unsigned int;

static auto randSource = std::random_device{};

template<std::integral T> T random(T min, T max) {
	return std::uniform_int_distribution<T>(min, max)(randSource);
}
template<std::floating_point T> T random(T min, T max) {
	return std::uniform_real_distribution<T>(min, max)(randSource);
}

constexpr auto TRIALS = 10000000uz;
constexpr auto THREADS = 2uz;

class DebugFloat
{
  private:
	float v;

  public:
	DebugFloat() { v = 0.0f; }
	DebugFloat(float val) { v = val; }
	DebugFloat(const DebugFloat& val) { v = val.v; }
	DebugFloat(DebugFloat&& val) noexcept {
		v = val.v;
		val.v = std::nanf("");
	}

	operator float() { return v; }
	~DebugFloat() noexcept { v = std::nanf(""); }
};


int main() {
	// mainLoop();
	// coro::mainJobLoop();


	return 0;
}
