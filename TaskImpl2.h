#pragma once
// NEXT TO FIX: Race condition on prcessTermination() & ~Task()...
//
// Fix 1: Use std::shared_ptr && std::weak_ptr to safe-gaurd ownership. Assure
// task destruction only when all awaitees are destructed or are relying on
// other tasks

// TODO: Redesign so that helping a coroutine doesn't require claiming it first.
// TODO: Redesign so that you claim a task only when you want to really run its
// coroutine handle.
// TODO: Allow multithreaded meta data updating, (Corespond to above redesign)
#include <coroutine>
#include <optional>
#include <variant>
#include <vector>
#include <atomic>
#include <utility>
#include <memory>
#include <initializer_list>
#include <span>
#include <assert.h>
#include <concepts>

template<typename T, void Dtor(T*)> class WeakPtrToSelf
{
	std::weak_ptr<T> p;

  protected:
	WeakPtrToSelf(): p(this, Dtor) {}
	const std::weak_ptr<T>& getWeakPtr() { return p; }
	std::shared_ptr<T> makeSharedPtr() { return p.lock(); }
};

template<class... Ts> struct overloadLambda: Ts... { using Ts::operator()...; };

// Await Ops
namespace tk {


	class AwaitOp
	{
	  protected:
		AwaitOp() = default;

	  public:
		// Try help completing awaitees.
		// Note: It should not allow helping claimed awaitee
		// Return: std::tuple<bool,bool>: first bool is whether it did help,
		// second is whether the AwaitOp is done
		virtual bool helpAwaitee() = 0;
		virtual ~AwaitOp() = default;
	};
	using AwaitOpAtomicRef = std::atomic<std::shared_ptr<AwaitOp>>;
	using AwaitOpAtomicLink = std::atomic<std::weak_ptr<AwaitOp>>;
}

// Task
namespace tk {

	class Task;
	enum class TaskStatus {
		Locked,		  // Locked. Locker can call any func
		Awaiting,	  // Non Locked. Only atomic func
		ToBeResumed,  // Non Locked. Only atomic func OR tryClaim()
		End,		  // Non Locked. Only atomic func
	};
	class TaskHandle;
	using TaskRef = std::shared_ptr<Task>;
	using TaskLink = std::weak_ptr<Task>;
	class ResultBox
	{
	  protected:
		ResultBox() = default;
		ResultBox(const ResultBox&) = delete;
		ResultBox(ResultBox&&) {}

	  public:
		virtual bool isTerminated() = 0;
	};

	class Task
	{
		friend class TaskHandle;

	  protected:
		std::atomic<TaskStatus> status;
		AwaitOpAtomicRef awaitOp{};
		Task() = default;
		Task(const Task&) = delete;
		Task(Task&&) = delete;

		// Try claim a result.
		// Return: nullptr == no result yet
		// Note: Termination counts as a immediate result
		virtual ResultBox* onPopResult() = 0;

		// Can modify any state as long as it didn't break other write-safe func
		// It should resume the coroutine unitl it co_awaits on others or
		// it has gotten one or more result (either by co_return or co_yield)
		// Note: Termination counts as a immediate result
		// Note: The amount of result it generates is not limited but it should
		// not exceed the amount of awaitor it has
		// Return: true only if a result is generated
		// WARINING: Remember that this function also unclaims the task
		virtual bool onClaimed() = 0;

		// Same as onClaimed() but it also claims a result if it has one
		virtual ResultBox* onClaimedAndPopResult() = 0;

	  public:
		const auto& getStatus() { return status; }

		// Atomic func, the box should be reserved after that pop
		// Note: Termination counts as a immediate result
		ResultBox* tryPopResult() {
			if (status.load(std::memory_order::relaxed) == TaskStatus::Locked)
				return nullptr;
			return onPopResult();
		}

		/*
		 * Idea: Change this to NON stack based... Perhaps?? Use
		 * OneToOneTask<void> with yield for running tasks.
		 *
		 * Result: Not seems to be a good idea. It has a lot of overhead and
		 * it's not really necessary. See InternalRunStack.h
		 */

		// Try and complete the task until there are one or more results
		// Note: Termination counts as a immediate result
		// Note: The result may be immediately claimed by other task. So it is
		// more suitable to use tryCompleteAndPopResult() instead to get the
		// result
		// Return: true if a result is generated
		bool tryComplete() {
			TaskStatus preStatus = TaskStatus::ToBeResumed;
			bool locked =
			  status.compare_exchange_strong(preStatus, TaskStatus::Locked,
				std::memory_order::relaxed, std::memory_order::relaxed);
			while (true)
			{
				switch (preStatus)
				{
				case TaskStatus::ToBeResumed:
					assert(locked);
					if (onClaimed()) return true;
					continue;
				case TaskStatus::Locked: return false;
				case TaskStatus::Awaiting: {
					// Take copy of shared_ptr to prevent dtor call of obj
					// before leaving scope
					std::shared_ptr<AwaitOp> awaitOpRef = awaitOp.load();
					AwaitOp* op = awaitOpRef.get();
					if (op == nullptr) return false;
					if (op->helpAwaitee()) return true;
				}
				case TaskStatus::End: return true;
				}
				return false;
			}
		}
	};

}





// Task base class
namespace task {

	class Task;
	enum class TaskStatus {
		Terminated,
		InProgress,
		Completed,
	};
	class TaskHandle;
	using TaskRef = std::shared_ptr<Task>;
	using TaskLink = std::weak_ptr<Task>;
	struct UseItself {};
	class Task
	{
		friend class TaskHandle;

	  protected:
		Task() = default;
		Task(const Task&) = delete;
		Task(Task&&) = delete;

		/// External Owning control management
		// For itself to make a task handle without being friends with
		// TaskHandle class
		TaskHandle makeHandle(const TaskLink& ptr);
		// Resume / run the task coroutine. Optionally return the next task to
		// run
		virtual std::variant<TaskHandle, UseItself> onResume(
		  bool tryClaimTask) = 0;
		// Terminate / halt the task, by throwing an error to the func. All
		// awaitors will also be terminated.
		virtual TaskHandle onTerminate() = 0;
		// Release the handle for others to take over, or signal that control is
		// no longer needed
		virtual void onRelease() = 0;

	  public:
		/// External Owning control management
		virtual TaskHandle tryClaimTask() = 0;

		/// External Status checking & Awaitee-Awaitor syncing
		virtual TaskStatus getStatus() = 0;
		virtual void dropAwaitor(const TaskLink& t) = 0;
		virtual void signalTermination() = 0;
		virtual TaskHandle notifyNewAwaitor(
		  const TaskLink& t, bool tryClaimTask = true) = 0;

		/// External Lifetime management
		virtual void destroy() = 0;
		static void destruct(Task* p) noexcept { p->destroy(); }
	};
	class TaskHandle
	{
		friend class Task;

	  private:
		TaskRef task;
		TaskHandle(TaskRef&& t): task(std::move(t)) {}

	  public:
		TaskHandle(): task() {}
		TaskHandle(const TaskHandle&) = delete;
		TaskHandle(TaskHandle&& o) noexcept: task(std::move(o.task)) {}
		TaskHandle& operator=(TaskHandle&& o) noexcept {
			task->onRelease();
			task = std::move(o.task);
			return *this;
		}
		operator bool() const { return bool(task); }
		bool hasValue() const { return bool(task); }
		bool hasResult() const { return bool(task.get()); }
		// Resume the task. Note that resuming completed task is invalid usage
		std::variant<TaskHandle, UseItself> resume(bool tryClaimTask = true);
		// Terminate / halt the task, by throwing an error to the func. All
		// awaitors will also be terminated.
		TaskHandle terminate() { task->onTerminate(); }

		// Destructor
		~TaskHandle() { task ? task->onRelease() : void(); }
	};
	std::variant<TaskHandle, UseItself> TaskHandle::resume(bool tryClaimTask) {
		return task->onResume(tryClaimTask);
	}

	TaskHandle Task::makeHandle(const TaskLink& ptr) {
		return TaskHandle(ptr.lock());
	}

	template<typename T> class TaskFuture
	{
	  public:
		virtual bool await_ready() = 0;
		virtual void await_suspend() = 0;
		virtual T await_resume() = 0;
	};
}

// Await Ops
namespace task {
	struct AwaitResultResume {};
	struct AwaitResultWait {
		TaskHandle nextTask;
	};
	struct AwaitResultTerminate {
		// TODO: Add reason in here
	};
	using AwaitResult =
	  std::variant<AwaitResultResume, AwaitResultWait, AwaitResultTerminate>;


	template<typename T> class BasicResultBox
	{
		T result;
		// TODO: Prob should be atomic?
		bool isDone = false;

	  public:
		template<typename... Args> void return_value(Args&&... v) {
			std::construct_at(&result, std::forward<Args>(v)...);
			isDone = true;
		}
		T operator*() { return result; }
		operator bool() const { return isDone; }
	};
	template<> class BasicResultBox<void>
	{
		bool isDone = false;

	  public:
		void return_void() { isDone = true; }
		constexpr void operator*() {}
		operator bool() const { return isDone; }
	};

	class AwaitOperation
	{
	  protected:
		AwaitOperation() = default;

	  public:
		// Return next task status operation
		virtual AwaitResult updateStatus(
		  std::atomic<TaskStatus>& currentStatus) = 0;
		virtual ~AwaitOperation() = default;
	};

	template<typename Future> class AwaitSingle: public AwaitOperation
	{
		Future future;

	  public:
		AwaitSingle(Future&& f): future(f) {}
		virtual AwaitResult updateStatus(
		  std::atomic<TaskStatus>& currentStatus) final override {
			assert(currentStatus.load() != TaskStatus::Completed);

			TaskStatus targetStatus = future->getStatus();
			// Check for termination first.
			if (targetStatus == TaskStatus::Terminated)
			{
				currentStatus.store(TaskStatus::Terminated);
				future.release();
				return AwaitResultTerminate{};
			}
			TaskStatus cStatus = currentStatus.load();
			if (cStatus == TaskStatus::Terminated)
			{
				future.release();
				return AwaitResultTerminate{};
			}
			assert(cStatus == TaskStatus::InProgress);
			if (targetStatus == TaskStatus::Completed)
				return AwaitResultResume{};
			assert(targetStatus == TaskStatus::InProgress);
			return AwaitResultWait{future->tryClaimTask()};
		}
		virtual ~AwaitSingle() final override = default;

		bool await_ready() { return future.hasResult(); }
		void await_suspend() { future.onSuspend(); }
		auto await_resume() { return future.getResult(); }
	};

	class AwaitAny: public AwaitOperation
	{
		std::vector<std::shared_ptr<Task>> tasks;

	  public:
		// TODO: Add more type??
		AwaitAny(std::vector<std::shared_ptr<Task>>&& ts):
		  tasks(std::move(ts)) {}
		virtual AwaitResult updateStatus(
		  std::atomic<TaskStatus>& currentStatus) final override {
			assert(currentStatus.load() != TaskStatus::Completed);

			TaskStatus cStatus = currentStatus.load();
			if (cStatus == TaskStatus::Terminated)
			{
				tasks.clear();
				return AwaitResultTerminate{};
			}

			TaskStatus targetStatus = TaskStatus::Terminated;
			TaskHandle nextTaskHandle{};
			for (auto& t : tasks)
			{
				if (!t) continue;
				auto s = t->getStatus();

				if (s == TaskStatus::Terminated)
				{
					t.reset();
					continue;
				} else if (s == TaskStatus::InProgress)
				{
					targetStatus = TaskStatus::InProgress;
					if (!nextTaskHandle) nextTaskHandle = t->tryClaimTask();
					continue;
				} else
				{
					assert(s == TaskStatus::Completed);
					targetStatus = TaskStatus::Completed;
					if (&t != &tasks.front()) { tasks.front() = std::move(t); }
					tasks.resize(1);
					break;
				}
			}

			// Check for termination
			if (targetStatus == TaskStatus::Terminated)
			{
				currentStatus.store(TaskStatus::Terminated);
				tasks.clear();
				return AwaitResultTerminate{};
			}

			assert(cStatus == TaskStatus::InProgress);
			if (targetStatus == TaskStatus::Completed)
				return AwaitResultResume{};
			assert(targetStatus == TaskStatus::InProgress);
			return AwaitResultWait{std::move(nextTaskHandle)};
		}
		virtual ~AwaitAny() final override = default;
	};

	class AwaitAll: public AwaitOperation
	{
		std::vector<std::shared_ptr<Task>> tasks;

	  public:
		// TODO: Add more type??
		AwaitAll(std::vector<std::shared_ptr<Task>>&& ts):
		  tasks(std::move(ts)) {}
		virtual AwaitResult updateStatus(
		  std::atomic<TaskStatus>& currentStatus) final override {
			assert(currentStatus.load() != TaskStatus::Completed);

			TaskStatus cStatus = currentStatus.load();
			if (cStatus == TaskStatus::Terminated)
			{
				tasks.clear();
				return AwaitResultTerminate{};
			}

			TaskStatus targetStatus = TaskStatus::Completed;
			TaskHandle nextTaskHandle{};
			for (auto& t : tasks)
			{
				assert(t);
				auto s = t->getStatus();

				if (s == TaskStatus::Terminated)
				{
					targetStatus = TaskStatus::Terminated;
					break;
				} else if (s == TaskStatus::InProgress)
				{
					targetStatus = TaskStatus::InProgress;
					if (!nextTaskHandle) nextTaskHandle = t->tryClaimTask();
					continue;
				} else
				{
					assert(s == TaskStatus::Completed);
					continue;
				}
			}

			// Check for termination
			if (targetStatus == TaskStatus::Terminated)
			{
				currentStatus.store(TaskStatus::Terminated);
				tasks.clear();
				return AwaitResultTerminate{};
			}

			assert(cStatus == TaskStatus::InProgress);
			if (targetStatus == TaskStatus::Completed)
				return AwaitResultResume{};
			assert(targetStatus == TaskStatus::InProgress);
			return AwaitResultWait{std::move(nextTaskHandle)};
		}
		virtual ~AwaitAll() final override = default;
	};
}

// One result to One awaitor task
namespace task {
	template<typename T> class OneToOneTask;
	template<typename T> using OneToOneCoroutine =
	  std::shared_ptr<OneToOneTask<T>>;
	template<typename T, typename... Args>
	struct std::coroutine_traits<std::shared_ptr<OneToOneTask<T>>, Args...> {
		using promise_type = task::OneToOneTask<T>;
	};


	template<typename T> class OneToOneTask:
	  public WeakPtrToSelf<OneToOneTask<T>, Task::destruct>,
	  BasicResultBox<T>,
	  Task
	{
		std::atomic_bool claimed = false;
		std::atomic<TaskStatus> status{TaskStatus::InProgress};
		TaskLink awaitor;
		std::unique_ptr<AwaitOperation> awaitee;

		TaskHandle _getAwaitorHandle() {
			auto awaitorPtr = awaitor.lock();
			if (!awaitorPtr) return TaskHandle{};
			return awaitorPtr->tryClaimTask();
		}

		TaskHandle _tryClaimNextTask() { assert(awaitee); }

	  protected:
		virtual std::variant<TaskHandle, UseItself> onResume(
		  bool tryClaimTask) final override {
			if (!awaitee)
			{
				HandleType::from_promise(*this).resume();
				if (awaitee && awaitee->get().) TaskStatus s = status.load();
				if (s == TaskStatus::Completed || s == TaskStatus::Terminated)
				{
					if (!tryClaimTask) return TaskHandle{};
					auto l = awaitor.lock();
					return l ? l->tryClaimTask() : TaskHandle{};
				}
				assert(s == TaskStatus::InProgress);
				if (!awaitee) return UseItself{};
			}
			AwaitResult op = awaitee->updateStatus(
			  status);	// TODO: Add settings to decide claim new task or not

			if (auto* v = std::get_if<AwaitResultWait>(&op))
				return tryClaimTask ? TaskHandle{std::move(v->nextTask)} : {};
			if (auto* v = std::get_if<AwaitResultTerminate>(&op))
			{
				awaitee.reset();
				return tryClaimTask ? _getAwaitorHandle() : {};
			}
			assert(std::get_if<AwaitResultResume>(&op) != nullptr);
			HandleType::from_promise(*this).resume();

			TaskStatus s = status.load();
			if (s == task::TaskStatus::InProgress) return UseItself{};
			return tryClaimTask ? _getAwaitorHandle() : {};
		}
		// Terminate / halt the task. Awaitors and awaitees of this function may
		// terminate.
		virtual TaskHandle onTerminate() {
			TaskStatus expected = TaskStatus::InProgress;
			status.compare_exchange_strong(expected, TaskStatus::Terminated);
		}
		// Release the handle for others to take over, or signal that control is
		// no longer needed
		virtual void onRelease() {
			bool expected = true;
			if (claimed.compare_exchange_strong(expected, false))
				throw "TaskHandleClaimedTwiceError";
		}

	  public:
		typedef std::coroutine_handle<OneToOneTask<T>> HandleType;
		typedef T ValueType;

		OneToOneTask(const TaskLink& caller): awaitor(caller) {}
		template<typename... Args> OneToOneTask(Args&&... args): awaitor() {}
		template<bool, typename... Args>
		OneToOneTask(bool startNow, Args&&... args): awaitor() {}

		template<typename U>
		auto await_transform(OneToOneCoroutine<U>&& handle) {
			// status.store(TaskStatus::InProgress);
			awaitee = new AwaitSingle(std::move(handle)->toFuture());
			return *awaitee;
		}


		virtual TaskHandle tryClaimTask() final override {
			bool e = false;
			if (claimed.compare_exchange_strong(e, true))
				return makeHandle(this->getWeakPtr());
			else
				return TaskHandle();
		}
		virtual TaskStatus getStatus() final override { return status.load(); }
		virtual void dropAwaitor(const TaskLink& t) final override {
			assert(!awaitor.owner_before(t) && !t.owner_before(awaitor));
			signalTermination();
		}
		virtual void signalTermination() final override {
			onTerminate();	// This has the same handling as owner terminating
		}

		virtual TaskHandle notifyNewAwaitor(
		  const TaskLink& t, bool tryClaimTask) final override {
			assert(!awaitor);
			awaitor = t;
			throw "InvalidUsage";
		}
		virtual void destroy() final override {
			HandleType::from_promise(*this).destroy();
		}

		// C++20 Coroutine func
		OneToOneCoroutine<T> get_return_object() {
			return this->makeSharedPtr();
		}
		std::suspend_always initial_suspend() {
			awaitee.reset();
			return {};
		}
		std::suspend_always yield_void() { awaitee.reset(); }

		constexpr std::suspend_always final_suspend() noexcept {
			awaitee.reset();
			status.store(TaskStatus::Completed);
			return {};
		}

		// For OneAwaiterOneResult, no await transform is needed
		// For list of OneAwaiterOneResult:
		// TODO

		void unhandled_exception() {}  // TODO
	};
	template<typename T> class OneToOneCoroutine:
	  public std::shared_ptr<OneToOneTask<T>>
	{
		using std::shared_ptr<OneToOneTask<T>>::shared_ptr;

	  public:
		OneToOneTaskFuture<T> toFuture() && {
			return OneToOneFuture<T>(std::move(*this));
		}
	};


	template<typename T> class OneToOneTaskFuture
	{
		friend class OneToOneTask<T>;
		OneToOneCoroutine task;

		OneToOneTaskFuture(OneToOneCoroutine&& task): task(std::move(task)) {
			// Immidiate try run it;
			task->notifyNewAwaitor();
			TaskHandle t = task->tryClaimTask();
			t.resume(false);
		}

	  public:
		bool hasResult() const { return bool(*task); }
		TaskStatus getStatus() const { return task->getStatus(); }
		void onSuspend() {}
		T getResult() { return **task; }
		void release() { task.reset(); }
	};
}
