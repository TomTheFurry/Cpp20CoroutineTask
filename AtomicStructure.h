#pragma once
#include <atomic>
#include <concepts>
#include <limits>
#include <variant>




namespace atomics {
	struct DEBUG_INIT {};

	// Mutex Locks without block function
	class BusyMutex
	{
		std::atomic_flag flag{};
		inline void free() { flag.clear(std::memory_order::release); }

	  public:
		class UniqueLock
		{
			friend class BusyMutex;
			BusyMutex* m;
			UniqueLock(BusyMutex* mtx): m(mtx) {}
			UniqueLock(const UniqueLock&) = delete;
			UniqueLock(UniqueLock&& o) noexcept: m(o.m) { o.m = nullptr; }

		  public:
			operator bool() const { return bool(m); }
			~UniqueLock() {
				if (m != nullptr) m->free();
			}
		};
		BusyMutex() = default;
		BusyMutex(const BusyMutex&) = delete;
		// REMEMBER to check if you get the lock or not!
		[[nodiscard]] inline UniqueLock tryGetLock() {
			bool v = flag.test_and_set(std::memory_order::acquire);
			if (v)
			{
				// flag is already set. Someone has the lock already
				return nullptr;
			} else
			{
				// Flag was not set, and it now is. We have gotten the lock
				return this;
			}
		}
	};

	// Class: RingQueueBuffer
	// It is a spin-lock based queue (fifo) with a fixed size implemented with a
	// ring buffer. There can be multiple comsumers and multiple producers. Note
	// that all memory usage is inlined, which means that there are no
	// additional dynamic allocations
	enum class Result {
		Contention,
		FullOrEmpty,
		Success,
	};
	template<typename T, std::unsigned_integral I = size_t, I N = 256>
	class RingQueueBuffer
	{
		BusyMutex mtxPush{};
		BusyMutex mtxPop{};
		T buffer[N];
		std::atomic<I> head{0};	 // May wrap around
		std::atomic<I> tail{0};	 // May wrap around

	  public:
		RingQueueBuffer() = default;
		RingQueueBuffer(const RingQueueBuffer&) = delete;
		RingQueueBuffer(RingQueueBuffer&&) = delete;
		// Pushes an element to the queue
		// If fails, it will not consume any arguments. Example: move semantics
		// will not be called and the object will not be consumed and is still
		// valid to use
		template<typename... Args> Result push(Args&&... args) {
			auto l = mtxPush.tryGetLock();
			if (!l) return Result::Contention;

			// No need for any read/write to previous head location
			auto h = head.load(std::memory_order::relaxed);
			// Aquire tail destruction when queue is near full and wraps around
			auto t = tail.load(std::memory_order::relaxed);
			// This should be fine even when the value wraps around
			if (h - t >= N) return Result::FullOrEmpty;

			std::construct_at(&buffer[h % N], std::forward<Args>(args)...);
			// Release the just constructed object
			head.store(h + 1, std::memory_order::release);

			return Result::Success;
		}
		// Pops an element from the queue
		std::variant<Result, T> pop() {
			auto l = mtxPop.tryGetLock();
			if (!l) return Result::Contention;

			// No need for any read/write to the previous pop location
			auto t = tail.load(std::memory_order::relaxed);
			// Acquire the value just pushed to queue when it is nearly empty
			auto h = head.load(std::memory_order::acquire);
			// This should be fine even when the value wraps around
			// h-t should never be negative as we have a lock on the pop and
			// other threads should only be pushing new elements
			if (h == t) return Result::FullOrEmpty;

			auto r = std::variant<Result, T>(std::move(buffer[t % N]));
			buffer[t % N].~T();
			// Release the value changed after destruction
			tail.store(t + 1, std::memory_order::relaxed);

			return r;
		}
		// Get the number of elements (upper bound)
		[[nodiscard]] I size() const {
			auto t = tail.load(std::memory_order::acquire);
			auto h = head.load(std::memory_order::relaxed);
			return h - t;
		}
		// Check if the queue is empty
		[[nodiscard]] bool empty() const {
			auto t = tail.load(std::memory_order::acquire);
			auto h = head.load(std::memory_order::relaxed);
			return h == t;
		}
		// Destructor (spin lock should be free or exception will be thrown)
		~RingQueueBuffer() noexcept(false) {
			auto lockA = mtxPush.tryGetLock();
			auto lockB = mtxPop.tryGetLock();
			if (!lockA || !lockB) throw "RingQueueBuffer is still in use";
			// Now no one can push or pop, and we have full control
			auto t = tail.load(std::memory_order::relaxed);
			auto h = head.load(std::memory_order::relaxed);
			for (auto i = t; i < h; ++i) { buffer[i % N].~T(); }
		}
	};

}
