#include <thread>
#include <coroutine>
#include <vector>
#include <variant>
#include <atomic>
#include <list>
#include <assert.h>
#include <iostream>

#pragma once

// FIXME: call delete() for std::coroutine_handle
// TODO: add custom new() for promise for allocing the coro-frame


template<typename T> class AtomicQueue
{};





enum class FunctionalJobExecutionResult {
	FinalCompleted,
	AwaitNewJob,
};

enum class WorkplaceJobExecutionResult {
	FinalCompleted,	 // Cleanup job, Run job awaiting the result, or new job.
	YieldCompleted,	 // Yielded, Run job awaiting the result, or new job.
	AwaitNewJob,	 // Return job to run, add job to await list. Run next job.
};

struct JobCompleted {};
struct JobYielded {};

class Executor
{
  public:
	std::list<std::coroutine_handle<>> coList{};
	template<typename T> void resumeJob(T& t);
};

int debugCounter = 0;
template<typename T> class WorkplaceCoroutineJob
{
	int _debugID = debugCounter++;
	friend class Executor;

  public:
	struct JobCoroutine {
		static std::suspend_always initial_suspend() { return {}; }
		static std::suspend_always final_suspend() noexcept { return {}; }
		static void unhandled_exception(){};
		Executor* upper = nullptr;

		void return_value(T i) { returnValue = i; }

		// Get a return object for the creator of a job
		WorkplaceCoroutineJob<T> get_return_object() {
			return WorkplaceCoroutineJob{*this};
		}
		bool hasValue() const { return returnValue.has_value(); }
		T getValue() { return returnValue.value(); }
		std::optional<T> returnValue{};
		template<typename U>
		auto await_transform(WorkplaceCoroutineJob<U> job) {
			upper->coList.push_front(job.getCoroutineHandle());
			job.getPromise().upper = upper;
			return AwaitedJobToDo<U>(job.getPromise());
		}
	};

  private:
	JobCoroutine& coroutine;

  public:
	using promise_type = JobCoroutine;
	using OneToOneTaskStrongHandle = std::coroutine_handle<JobCoroutine>;
	WorkplaceCoroutineJob(JobCoroutine& h): coroutine(h) {}

	template<typename U> struct AwaitedJobToDo {
		WorkplaceCoroutineJob<U>::JobCoroutine& c;
		AwaitedJobToDo(WorkplaceCoroutineJob<U>::JobCoroutine& newJob):
		  c(newJob) {}

		void await_ready() {}
		U await_resume() { return c.getValue(); }
		bool await_suspend(auto t) { return true; }
	};
	std::unique_ptr<Executor> executor;

	JobCoroutine& getPromise() { return coroutine; }
	bool isDone() const { return coroutine.hasValue(); }

	OneToOneTaskStrongHandle getCoroutineHandle() const {
		return OneToOneTaskStrongHandle::from_promise(coroutine);
	}

	T getValue() { return coroutine.getValue(); }
};

template<typename T> inline void Executor::resumeJob(T& t) {
	if (!t.executor)
	{
		t.executor = std::unique_ptr<Executor>(new Executor{});
		t.coroutine.upper = this;
	}
	if (!coList.empty())
	{
		auto c = coList.front();
		c.resume();
		if (c.done()) coList.pop_front();
	} else
		t.getCoroutineHandle().resume();
}



WorkplaceCoroutineJob<int> makeOne();
WorkplaceCoroutineJob<float> makeOneF() { co_return 1.f; }

WorkplaceCoroutineJob<int> makeOne() { co_return(int) co_await makeOneF(); }

WorkplaceCoroutineJob<double> doStuff() {
	co_return(double) co_await makeOne();
}

void mainLoop() {
	auto start = doStuff();
	auto executor = Executor();
	int i = 0;
	while (!start.isDone())
	{
		executor.resumeJob(start);
		i++;
	}
	double r = start.getValue();
	i++;
}


class Worker: std::thread
{
  public:
};

class Scheduler
{
  public:
	Scheduler();
	std::vector<Worker> workers;
};
