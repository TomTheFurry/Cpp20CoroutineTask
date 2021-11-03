#pragma once
// NEXT TO FIX: Race condition on prcessTermination() & ~Task()...
//
// Fix 1: Use std::shared_ptr && std::weak_ptr to safe-gaurd ownership. Assure
// task destruction only when all awaitees are destructed or are relying on
// other tasks

//TODO: Redesign so that helping a coroutine doesn't require claiming it first.
//TODO: Redesign so that you claim a task only when you want to really run its coroutine handle.
//TODO: Allow multithreaded meta data updating, (Corespond to above redesign)
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
		virtual std::variant<TaskHandle, UseItself> onResume(bool tryClaimTask) = 0;
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
		bool hasResult() const { return bool(*task); }
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
		//TODO: Prob should be atomic?
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

	template<typename Future>
	class AwaitSingle: public AwaitOperation
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
		
		
		
		bool await_ready() {
			return future.hasResult();
		}
		std::coroutine_handle<> await_suspend() {
			return future.onSuspend();
		}
		auto await_resume() {
			return future.getResult();
		}
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
			awaitee.reset();
			auto awaitorPtr = awaitor.lock();
			if (!awaitorPtr) return TaskHandle{};
			return awaitorPtr->tryClaimTask();
		}

	  protected:
		virtual std::variant<TaskHandle, UseItself> onResume(bool tryClaimTask) final override {
			assert(awaitee);
			AwaitResult op = awaitee->updateStatus(status);

			if (auto* v = std::get_if<AwaitResultWait>(&op))
				return tryClaimTask ? TaskHandle{std::move(v->nextTask)} : {};
			if (auto* v = std::get_if<AwaitResultTerminate>(&op))
				return tryClaimTask ? _getAwaitorHandle() : {};
			assert(std::get_if<AwaitResultResume>(&op) != nullptr);
			HandleType::from_promise(*this).resume();
			TaskStatus s = status.load();
			if (s == task::TaskStatus::InProgress) return tryClaimTask ? UseItself{} : {};
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
		template<typename... Args> OneToOneTask(bool startNow, Args&& args):
		  awaitor() {}

		template<typename U>
		auto await_transform(OneToOneCoroutine<U>&& handle) {
			//status.store(TaskStatus::InProgress);
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
		OneToOneCoroutine get_return_object() { return this->makeSharedPtr(); }
		constexpr std::suspend_always initial_suspend() { return {}; }
		constexpr std::suspend_always final_suspend() noexcept {
			status.store(TaskStatus::Completed);
			return {};
		}

		// For OneAwaiterOneResult, no await transform is needed
		// For list of OneAwaiterOneResult:
		// TODO

		void unhandled_exception() {}  // TODO
	};
	template<typename T> class OneToOneCoroutine : public std::shared_ptr<OneToOneTask<T>> {
		using std::shared_ptr<OneToOneTask<T>>::shared_ptr;
		public:
		OneToOneTaskFuture<T> toFuture() && {
			return OneToOneFuture<T>(std::move(*this));
		}
	}
	
	
	template<typename T> class OneToOneTaskFuture
	{
		friend class OneToOneTask<T>;
		OneToOneCoroutine task;
		
		OneToOneTaskFuture(OneToOneCoroutine&& task) : task(std::move(task)) {
			//Immidiate try run it;
			task->notifyNewAwaitor();
			TaskHandle t = task->tryClaimTask();
			t.resume(false);
		}

	  public:
		bool hasResult() const { return bool(*task); }
		TaskStatus getStatus() const { return task->getStatus(); }
		
		
		std::coroutine_handle<> onSuspend() {
			TaskHandle t = task->tryClaimTask();
			return t ? HandleType::from_promise(*task) : {};
		}
		T getResult() { return **task; }
		void release() { task.reset(); }
	};
}
