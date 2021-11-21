//#include "Scheduler.h"
//#include "JobSystem.h"
#include "AtomicStructure.h"
#include <random>
#include <concepts>
#include <limits>
#include <assert.h>
#include "vertify.h"
#include <thread>
#include <numbers>
#include <vector>
#include <iostream>
#include <chrono>

// Test result: 45077580 vs 188722359

constexpr size_t operator"" uz(size_t s) { return s; }


using uint = unsigned int;
using ulint = unsigned long long;
// static auto randSource = std::random_device{};
std::mt19937& getRandSource() {
	static thread_local std::mt19937 randSource{};
	return randSource;
}


template<std::integral T> T random(T min, T max) {
	return std::uniform_int_distribution<T>(min, max)(getRandSource());
}
template<std::floating_point T> T random(T min, T max) {
	return std::uniform_real_distribution<T>(min, max)(getRandSource());
}
template<std::integral T> T random() {
	return std::uniform_int_distribution<T>(std::numeric_limits<T>::min(),
	  std::numeric_limits<T>::max())(getRandSource());
}
template<std::floating_point T> T random() {
	return std::uniform_real_distribution<T>(std::numeric_limits<T>::min(),
	  std::numeric_limits<T>::max())(getRandSource());
}
template<std::floating_point T> bool random(T ratio) {
	return random(0.0, 1.0) >= ratio;
}

class DebugFloat
{
  private:
	float v;

  public:
	DebugFloat() { v = 0.0f; }
	DebugFloat(float val) { v = val; }
	DebugFloat(const DebugFloat& val) {
		vertify(std::isfinite(val.v));
		v = val.v;
	}
	DebugFloat(DebugFloat&& val) noexcept {
		vertify(std::isfinite(val.v));
		v = val.v;
		val.v = std::nanf("");
	}

	operator float() { return v; }
	~DebugFloat() noexcept { v = std::nanf(""); }
};

constexpr auto THREAD_COUNT = 8uz;
constexpr auto PUSH_POP_RATIO_A = 0.5;
constexpr auto PUSH_POP_RATIO_B = 0.5;
constexpr auto A_B_RATIO_SWITCH_COUNT = 5uz;
constexpr auto A_B_RATIO_SWITCH_LENGTH = std::chrono::seconds(1);
static atomics::RingQueueBuffer<DebugFloat>* rqb = nullptr;
static std::atomic<double> active_a_b_ratio = 0.0;
static std::atomic_flag stopSignal{};
static std::atomic<ulint> addTotalOp{0};
static std::atomic<ulint> addSuccessOp{0};
static std::atomic<ulint> addContentionOp{0};
static std::atomic<ulint> subTotalOp{0};
static std::atomic<ulint> subSuccessOp{0};
static std::atomic<ulint> subContentionOp{0};

void controlThread() {
	for (auto i = 0uz; i < A_B_RATIO_SWITCH_COUNT; i++)
	{
		double v = random(0.0, 1.0);
		active_a_b_ratio.store(v, std::memory_order_relaxed);
		std::cout << i << ": New ratio: " << v << " (Count: " << rqb->size()
				  << ")\n";
		std::this_thread::sleep_for(A_B_RATIO_SWITCH_LENGTH);
	}
	std::cout << "Done.\n";
	stopSignal.test_and_set(std::memory_order_relaxed);
}

void testThread(double v) {
	ulint locAddTotalOp = 0;
	ulint locAddSuccessOp = 0;
	ulint locAddContentionOp = 0;
	ulint locSubTotalOp = 0;
	ulint locSubSuccessOp = 0;
	ulint locSubContentionOp = 0;
	while (!stopSignal.test(std::memory_order_relaxed))
	{
		bool isA = active_a_b_ratio.load(std::memory_order_relaxed) < v;
		if (random(isA ? PUSH_POP_RATIO_A : PUSH_POP_RATIO_B))
		{
			atomics::Result worked = rqb->push(DebugFloat(random<float>()));
			++locAddTotalOp;
			if (worked == atomics::Result::Success) ++locAddSuccessOp;
			if (worked == atomics::Result::Contention) ++locAddContentionOp;
		} else
		{
			auto u = rqb->pop();
			auto ptr = std::get_if<atomics::Result>(&u);
			atomics::Result worked = ptr ? *ptr : atomics::Result::Success;
			if (!ptr) vertify(std::isfinite(float(std::get<DebugFloat>(u))));
			++locSubTotalOp;
			if (worked == atomics::Result::Success) ++locSubSuccessOp;
			if (worked == atomics::Result::Contention) ++locSubContentionOp;
		}
	}
	addTotalOp.fetch_add(locAddTotalOp, std::memory_order_relaxed);
	addSuccessOp.fetch_add(locAddSuccessOp, std::memory_order_relaxed);
	addContentionOp.fetch_add(locAddContentionOp, std::memory_order_relaxed);
	subTotalOp.fetch_add(locSubTotalOp, std::memory_order_relaxed);
	subSuccessOp.fetch_add(locSubSuccessOp, std::memory_order_relaxed);
	subContentionOp.fetch_add(locSubContentionOp, std::memory_order_relaxed);
}

void atomicBuffTest() {
	stopSignal.clear(std::memory_order_relaxed);
	rqb = new atomics::RingQueueBuffer<DebugFloat>{};
	auto cThread = new std::thread(&controlThread);
	std::vector<std::thread*> tPool{};
	tPool.reserve(THREAD_COUNT);
	for (auto i = 0uz; i < THREAD_COUNT; i++)
		tPool.push_back(new std::thread(&testThread, (1.0 * i) / THREAD_COUNT));
	cThread->join();
	for (auto* t : tPool) t->join();
	auto atOp = addTotalOp.load();
	auto asOp = addSuccessOp.load();
	auto acOp = addContentionOp.load();
	auto stOp = subTotalOp.load();
	auto ssOp = subSuccessOp.load();
	auto scOp = subContentionOp.load();
	auto tOp = atOp + stOp;
	auto sOp = asOp + ssOp;
	auto cOp = acOp + scOp;

	std::cout << "All threads completed.\n";
	std::cout << "\n-----Push-----\n";
	std::cout << "Total Ops: " << atOp << "\nSuccess Ops: " << asOp
			  << "\nContention Ops: " << acOp
			  << "\nInvalidOps: " << (atOp - asOp - acOp) << '\n';
	std::cout << "SuccessOp/ValidOp ratio: " << asOp / double(asOp + acOp)
			  << "\nValidOp/TotalOp ratio: " << (asOp + acOp) / double(atOp)
			  << '\n';
	std::cout << "\n-----Pop-----\n";
	std::cout << "Total Ops: " << stOp << "\nSuccess Ops: " << ssOp
			  << "\nContention Ops: " << scOp
			  << "\nInvalidOps: " << (stOp - ssOp - scOp) << '\n';
	std::cout << "SuccessOp/ValidOp ratio: " << ssOp / double(ssOp + scOp)
			  << "\nValidOp/TotalOp ratio: " << (ssOp + scOp) / double(stOp)
			  << '\n';
	std::cout << "\n-----Push+Pop-----\n";
	std::cout << "Total Ops: " << tOp << "\nSuccess Ops: " << sOp
			  << "\nContention Ops: " << cOp
			  << "\nInvalidOps: " << (tOp - sOp - cOp) << '\n';
	std::cout << "SuccessOp/ValidOp ratio: " << sOp / double(sOp + cOp)
			  << "\nValidOp/TotalOp ratio: " << (sOp+cOp) / double(tOp) << '\n';
	std::cout << "\n-----Cleanup-----\n";
	std::cout << "Now emptying buffer...\n";
	std::cout << "Detected remaining object count: " << rqb->size() << '\n';
	delete rqb;
	rqb = nullptr;
	std::cout << "Queue sucessfully destructed. Test sucessful.\n";
}


int main() {
	// mainLoop();
	// coro::mainJobLoop();
	atomicBuffTest();

	return 0;
}
