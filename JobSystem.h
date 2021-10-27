#pragma once
#include <atomic>
#include <list>
#include <coroutine>
#include <iostream>
#include <assert.h>
#include <memory>





namespace coro {

	typedef long long lint;
	typedef unsigned long long ulint;
	typedef unsigned int uint;
	typedef ulint TickId;
	template<typename T> class Awaitable;
	template<typename T> class SingleCallJob;
	template<typename T> class MultiCallJob;
	class Worker;
	// TODO Maybe record the reason
	struct CoroutineTerminationException {};

	class CoControl
	{
		std::coroutine_handle<> handle;
		bool _terminationFlag = false;
		std::list<CoControl*> _waitingFor{};
		friend class Awaitable<void>;

	  protected:
		Worker* worker;
		CoControl(std::coroutine_handle<> h): handle(h){};
		void _notifyWaitingFor(std::initializer_list<CoControl*> c) {
			assert(_waitingFor.empty());
			_waitingFor.insert(_waitingFor.begin(), c);
		}

	  public:
		bool isTerminated() const { return _terminationFlag; }
		bool operator()(Worker& w) { resume(w); }
		bool resume(Worker& w) {
			_waitingFor.clear();
			worker = &w;
			handle.resume();
			worker = nullptr;
			return handle.done();
		}
		bool done() const { return handle.done(); }
		bool awaitingOthers() const { return !_waitingFor.empty(); }
		void destroy() { handle.destroy(); }
	};

	template<typename T>
	concept PromiseType = requires(T) {
		std::convertible_to<T, typename T::ControlType>;
		std::convertible_to<typename T::HandleType, std::coroutine_handle<>>;
	};
	template<typename T>
	concept TerminatingPromiseType = requires(T) {
		PromiseType<T>;
		std::convertible_to<T&, CoControl&>;
	};
	template<typename T>
	concept VoidPromiseType = requires(T) {
		PromiseType<T>;
		std::same_as<void, typename T::ValueType>;
		std::convertible_to<T&, CoControl>;
	};
	template<typename T>
	concept ValuePromiseType = requires(T t) {
		PromiseType<T>;
		!std::same_as<void, typename T::ValueType>;
		{ *t } -> std::convertible_to<typename T::ValueType>;
	};

	template<ValuePromiseType Promise> class Awaitable<Promise>
	{
		Promise& promise;

	  public:
		Awaitable(Promise& p): promise(p) {}
		constexpr void await_ready() {}
		Promise::ValueType await_resume() {
			if (promise.isTerminated()) throw CoroutineTerminationException{};
			auto r = *promise;
			if (promise.done()) promise.destroy();
			return r;
		}
		bool await_suspend(auto t) { return true; }
		CoControl& getControl() const { return promise; }
	};
	template<> class Awaitable<void>
	{
		CoControl& coControl;

	  public:
		Awaitable(VoidPromiseType auto& p): coControl(p) {}
		constexpr void await_ready() {}
		void await_resume() {
			if (coControl.isTerminated()) throw CoroutineTerminationException{};
			if (coControl.done()) coControl.destroy();
			return;
		}
		bool await_suspend(auto t) { return true; }
		CoControl& getControl() const { return coControl; }
	};


	class Worker
	{
		static std::list<CoControl*> jobToBeRun;  // init this
		static void addJobBusy(CoControl& jobControl) {
			jobToBeRun.push_back(&jobControl);	// Make atomic
		}
		std::list<CoControl*> localJobStack{};


	  public:
		void pushJobLocaly(CoControl& jobControl) {
			localJobStack.push_front(&jobControl);
		}
		template<typename Job> static void startJob(Job& job) {
			addJobBusy(*job);
		}
		Worker() {}
		// bool: All completed?
		bool tryDoJob(ulint nsTimeout) {
			// TODO add nsTimeout logic
			if (localJobStack.empty())
			{
				if (jobToBeRun.empty()) return true;
				CoControl& cc = *jobToBeRun.front();
				jobToBeRun.pop_front();
				localJobStack.push_front(&cc);
			}
			while (!localJobStack.empty())
			{
				CoControl& cc = *localJobStack.front();
				cc.resume(*this);
				if (!cc.awaitingOthers()) { localJobStack.pop_front(); }
			}
			return localJobStack.empty();
		}
		uint getRemainingJobCount();
		TickId getHighestActiveTick();
		TickId getLowestActiveTick();
		TickId getLowestReferencedTick();  // Maybe not needed
	};
	std::list<CoControl*> Worker::jobToBeRun{};

	template<typename T> class ResultContainer
	{
		T result;

	  public:
		template<typename... Args> void return_value(Args&&... v) {
			std::construct_at(&result, std::forward<Args>(v)...);
		}
		T operator*() { return result; }
	};
	template<> class ResultContainer<void>
	{
	  public:
		constexpr void return_void() {}
	};

	template<typename T>
	class Promise: public ResultContainer<T>, public CoControl
	{
	  public:
		typedef std::coroutine_handle<Promise<T>> HandleType;
		typedef CoControl ControlType;
		typedef T ValueType;

		static void* operator new(size_t n) {
			return (void*)std::allocator<std::byte>().allocate(n);
		}  // TODO

		Promise(): CoControl(HandleType::from_promise(*this)) {}
		Promise(const Promise&) = delete;
		Promise(Promise&&) = delete;

		operator CoControl&() & { return static_cast<CoControl&>(*this); }

		template<typename U> auto await_transform(SingleCallJob<U>&& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}
		template<typename U> auto await_transform(SingleCallJob<U>& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}
		template<typename U> auto await_transform(MultiCallJob<U>& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}

		// Single call job should not allow 'co_await promise' as that is
		// designed for co_await on same value in mutiple source
		Awaitable<Promise<T>> operator co_await() = delete;

		Promise& get_return_object() { return *this; }
		constexpr std::suspend_always initial_suspend() { return {}; }
		constexpr std::suspend_always final_suspend() noexcept { return {}; }
		void unhandled_exception() {}  // TODO
	};

	template<typename T> class YieldingPromise;
	template<typename T> class MultiResultContainer
	{
		std::list<T> yieldedResult{};  // TODO Change to atomic queue

	  public:
		template<typename... Args> void return_value(Args&&... v) {
			yieldedResult.emplace_back(std::forward<Args>(v)...);
		}
		template<typename... Args>
		std::suspend_always yield_value(Args&&... v) {
			yieldedResult.emplace_back(std::forward<Args>(v)...);
			return {};
		}
		T operator*() {
			// TODO Make atomic
			T r{yieldedResult.front()};
			yieldedResult.pop_front();
			return r;
		}
	};
	template<> class MultiResultContainer<void>
	{
	  public:
		constexpr void return_void() {}
		static std::suspend_always yield_value() {}
	};

	template<typename T>
	class YieldingPromise: public MultiResultContainer<T>, public CoControl
	{
	  public:
		typedef std::coroutine_handle<YieldingPromise<T>> HandleType;
		typedef CoControl ControlType;
		typedef T ValueType;

		static void* operator new(size_t n) {
			return (void*)std::allocator<std::byte>().allocate(n);
		}  // TODO
		YieldingPromise(): CoControl(HandleType::from_promise(*this)) {}
		YieldingPromise(const YieldingPromise&) = delete;
		YieldingPromise(YieldingPromise&&) = delete;

		operator CoControl&() & { return static_cast<CoControl&>(*this); }
		Awaitable<YieldingPromise<T>> operator co_await() { return *this; }

		template<typename U> auto await_transform(SingleCallJob<U>&& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}
		template<typename U> auto await_transform(SingleCallJob<U>& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}
		template<typename U> auto await_transform(MultiCallJob<U>& a) {
			auto awaitable = a.getAwaiter();
			_notifyWaitingFor({&awaitable.getControl()});
			worker->pushJobLocaly(awaitable.getControl());
			return awaitable;
		}

		YieldingPromise& get_return_object() { return *this; }
		constexpr std::suspend_always initial_suspend() { return {}; }
		constexpr std::suspend_always final_suspend() noexcept { return {}; }
		void unhandled_exception() {}  // TODO
	};




	template<typename T> class SingleCallJob
	{
		Promise<T>& promise;

	  public:
		typedef Promise<T> promise_type;
		SingleCallJob(Promise<T>& p): promise(p) {}
		Promise<T>& operator*() { return promise; }
		Awaitable<Promise<T>> getAwaiter() { return promise; }
	};

	template<typename T> class MultiCallJob
	{
		YieldingPromise<T>& promise;

	  public:
		typedef YieldingPromise<T> promise_type;
		MultiCallJob(YieldingPromise<T>& p): promise(p) {}
		YieldingPromise<T>& operator*() { return promise; }
		Awaitable<YieldingPromise<T>> getAwaiter() { return promise; }
	};

	class JobDesk
	{
	  protected:
		virtual SingleCallJob<void> makeJob();
	  public:
		JobDesk();
		Promise<void> getPromise();
	};





	class JobSite
	{
		struct Data {
			float a[100];
		};

		std::list<std::pair<TickId, Data>> dataset{};

	  public:
		Promise<float> requestPositionAtTick(TickId tick) {
			auto it = dataset.begin();
			if (it->first > tick) throw "Too Late!";
			while (it->first < tick) it++;
		}
		SingleCallJob<void> loadData() {
			auto& d = dataset.emplace_back(0, Data{});
			for (float& f : d.second.a)
			{
				do f += rand();
				while (f / RAND_MAX > 1.f);
			}
			std::cout << "First frame data loaded.\n";
		}
	};





	SingleCallJob<int> makeRng() { co_return rand(); }

	MultiCallJob<float> makeCount() {
		float f = 1.f;
		while (true)
		{
			f *= 1.234f;
			co_yield f;
		}
		co_return f;
	}
	SingleCallJob<double> doBoth(MultiCallJob<float>& coMakeCount) {
		std::cout << co_await makeRng() << ' ';
		std::cout << co_await coMakeCount << '\n';
		co_return co_await coMakeCount;
	}


	void mainJobLoop() {
		Worker worker{};
		auto genJob = makeCount();
		auto mainJob = doBoth(genJob);
		worker.startJob(mainJob);
		worker.tryDoJob(0);
		std::cout << **mainJob;
	}
}
