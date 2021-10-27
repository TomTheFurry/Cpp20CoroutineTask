#pragma once
// NEXT TO FIX: Race condition on prcessTermination() & ~Task()...
//
// Fix 1: Use std::shared_ptr && std::weak_ptr to safe-gaurd ownership. Assure
// task destruction only when all awaitees are destructed or are relying on
// other tasks
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

template <typename T, typename D>
class WeakPtrToSelf {
	std::weak_ptr<T> p;
protected:
	WeakPtrToSelf() : p(this,D()) {}
	std::weak_ptr<T> makeWeakPtr() {return p;}
	std::shared_ptr<T> makeSharedPtr() {return p.lock();}
};




namespace task {

	template<typename Task>
	struct Destructor {
		operator() (Task* p) noexcept {p->destroy();}
	};
	
	enum class TaskStatus {
	Terminated,
	InProgress,
	Completed,
	};
	class Task;

	class TaskStrongHandle
	{
	  protected:
		TaskStrongHandle() = default;
		TaskStrongHandle(TaskStrongHandle&&) = default;
		TaskStrongHandle(const TaskStrongHandle&) = delete;

	  public:
		// virtual Task& getTask() = 0;
		//  Resume / run the task coroutine. Optionally return the next task to
		//  run
		virtual std::optional<TaskStrongHandle> resume() = 0;
		// Terminate / halt the task, by throwing an error to the func. All
		// awaitors will also be terminated.
		virtual std::optional<TaskStrongHandle> terminate() = 0;
		// Release the handle for others to take over, or signal that control is
		// no longer needed
		virtual ~TaskStrongHandle() = default;
	};

	class Task
	{
	  protected:
		Task() = default;
		Task(const Task&) = delete;
		Task(Task&&) = delete;

	  public:
		virtual std::optional<TaskStrongHandle> tryClaimTask() = 0;
		virtual TaskStatus getStatus() = 0;
		virtual void dropAwaitor(std::weak_ptr<Task> t) = 0;
		virtual void signalTermination() = 0;
		virtual std::variant<TaskStrongHandle, Task*> notifyNewAwaitor() = 0;
		virtual void destroy() = 0;
	};


	template<typename T> class BasicResultBox
	{
		T result;

	  public:
		template<typename... Args> void return_value(Args&&... v) {
			std::construct_at(&result, std::forward<Args>(v)...);
		}
		T operator*() { return result; }
	};
	template<> class BasicResultBox<void>
	{
	  public:
		constexpr void return_void() {}
		constexpr void operator*() {}
	};

	class AwaitOperation
	{
	  protected:
		AwaitOperation() = default;

	  public:

		// Return whether to start termination of the awaitor
		virtual TaskStatus updateStatus() = 0;
		virtual ~AwaitOperation() = default;
	};

	class AwaitSingle: public AwaitOperation
	{
		std::shared_ptr<Task> task;
	  public:
		AwaitSingle(std::shared_ptr<Task>&& t): task(t) {}
		virtual TaskStatus updateStatus() final override {
			return task->getStatus();
		}
		virtual ~AwaitSingle() final override = default;
	};

	class AwaitEither: public AwaitOperation
	{
		std::vector<std::shared_ptr<Task>> tasks;

	  public:
		// TODO: Add more type??
		AwaitEither(std::vector<std::shared_ptr<Task>>&& ts): tasks(std::move(ts)) {}
		virtual TaskStatus updateStatus() final override {
			bool anyInProgress = false;
			for (auto& t : tasks) {
				TaskStatus s = t->getStatus();
				if (s == TaskStatus::Completed) return TaskStatus::Completed;
				if (s == TaskStatus::InProgress) anyInProgress = true;
			}
			return anyInProgress ? TaskStatus::InProgress : TaskStatus::Terminated;
		}
		virtual ~AwaitEither() final override = default;
	};

	class AwaitAll: public AwaitOperation
	{
		std::vector<std::shared_ptr<Task>> tasks;

	  public:
		// TODO: Add more type??
		AwaitAll(std::vector<std::shared_ptr<Task>>&& ts): tasks(std::move(ts)) {}
		virtual TaskStatus updateStatus() final override {
			bool anyInProgress = false;
			for (auto& t : tasks) {
				TaskStatus s = t->getStatus();
				if (s == TaskStatus::Terminated) return TaskStatus::Terminated;
				if (s == TaskStatus::InProgress) anyInProgress = true;
			}
			return anyInProgress ? TaskStatus::InProgress : TaskStatus::Completed;
		}
		virtual ~AwaitAll() final override = default;
	};
}
namespace task {

	template<typename T> class OneToOneTask;
	template<typename T> class OneToOneTaskStrongHandle;

	template<typename T> class OneToOneTaskStrongHandle: public TaskStrongHandle
	{
		template<typename U> friend class OneToOneTask;
		std::shared_ptr<OneToOneTask<T>> promise;

	  private:
		OneToOneTaskStrongHandle(std::shared_ptr<OneToOneTask<T>>&& p): promise(p) {}

	  public:
		using promise_type = OneToOneTask<T>;
		// virtual Task& getTask();
		virtual std::optional<TaskStrongHandle> resume() final override {
			promise.resume();
			if (promise.awaitees.empty()) {}
		}
		virtual std::optional<TaskStrongHandle> terminate() final override {

		}
		virtual ~OneToOneTaskStrongHandle() final override {}
	};
	template<typename T> class OneToOneTask: public WeakPtrToSelf<OneToOneTask<T>, Destructor<OneToOneTask<T>>>, BasicResultBox<T>, Task
	{
		template<typename U> friend class OneToOneTaskStrongHandle;
		std::atomic_bool claimed = false;
		std::atomic<TaskStatus> status {TaskStatus::InProgress};
		std::weak_ptr<Task> awaitor;
		std::unique_ptr<AwaitOperation> awaitee;
		OneToOneTaskStrongHandle<T> getHandle() { return OneToOneTaskStrongHandle<T>(this->makeSharedPtr()); }
		void resume() { HandleType::from_promise(*this).resume(); }

	  public:
		typedef std::coroutine_handle<OneToOneTask<T>> HandleType;
		typedef T ValueType;

		OneToOneTask(std::weak_ptr<Task>&& caller) : awaitor(std::move(caller)) {}

		virtual std::optional<TaskStrongHandle> tryClaimTask() final override {
			bool e = false;
			if (claimed.compare_exchange_strong(e, true))
				return std::make_optional<TaskStrongHandle>(getHandle());
			else
				return {};
		}
		virtual TaskStatus getStatus() final override {
			return status.load();
		}
		virtual void dropAwaitor(std::weak_ptr<Task> t) final override {
			assert(!awaitor.owner_before(t) && !t.owner_before(awaitor));
			signalTermination();
		}
		virtual void signalTermination() final override {
			status.store(TaskStatus::Terminated);
		}

		virtual std::variant<TaskStrongHandle, Task*> notifyNewAwaitor() { throw "InvalidUsage";}
		virtual void destroy() final override {HandleType::from_promise(*this).destroy();}

		HandleType get_return_object() {
			return HandleType::from_promise(*this);
		}
		constexpr std::suspend_always initial_suspend() { return {}; }
		constexpr std::suspend_always final_suspend() noexcept { return {}; }

		// For OneAwaiterOneResult, no await transform is needed
		// For list of OneAwaiterOneResult:
		// TODO

		void unhandled_exception() {}  // TODO
	};
	template<typename T> using OneToOneCoroutine = OneToOneTaskStrongHandle<T>;

	template<typename T> class OneToOneAwaitable
	{
		OneToOneTaskRef<T> newTask;

	  public:
		OneToOneAwaitable(OneToOneTaskRef<T>&& t): newTask(std::move(t)) {}
		std::true_type await_ready() {
			newTask.resume();  // Direct call as no suspend needed
			return {};
		}
		T await_resume() { return newTask.result(); }
	};


	template<typename T> class OneToOneTaskRef: public Task
	{
		friend class OneToOneAwaitable<T>;
		typedef OneToOneTask<T> promise_type;
		promise_type::HandleType handle;


	  public:
		OneToOneTaskRef(promise_type::HandleType h): handle(h) {}
		OneToOneTaskRef(OneToOneTaskRef&& o) noexcept: handle(o.handle) {
			o.handle = {};
		}
		OneToOneTaskRef(const OneToOneTaskRef&) = delete;
		~OneToOneTaskRef() { handle.destroy(); }
		// Always successful
		virtual std::optional<TaskResumeHandle> tryClaimTask() final override {
			return {TaskResumeHandle(this)};
		}
		T result() { return *handle.promise(); }

		OneToOneAwaitable<T> operator co_await() && {
			return {std::move(*this)};
		}
	};
}

namespace task {

	template<typename T> class MultiAwaiterOneResultTask;
	template<typename T> class MultiOwnerPromise: public ResultContainer<T>
	{
	  public:
		typedef std::coroutine_handle<MultiOwnerPromise<T>> HandleType;
		typedef T ValueType;
		bool done() { return HandleType::from_promise(*this).done(); }
		// NOT thread safe!
		void resume() { HandleType::from_promise(*this).resume(); }
		HandleType get_return_object() {
			return HandleType::from_promise(*this);
		}
		constexpr std::suspend_always initial_suspend() { return {}; }
		constexpr std::suspend_always final_suspend() noexcept { return {}; }

		// For OneAwaiterOneResult, no await transform is needed
		// For list of OneAwaiterOneResult:
		// TODO

		void unhandled_exception() {}  // TODO

		static void destroy(MultiOwnerPromise* p) {
			HandleType::from_promise(p).destroy();
		}
	};
	template<typename T> class MultiOwnerOneResultAwaitable
	{
		std::shared_ptr<MultiOwnerPromise<T>> p;

	  public:
		MultiOwnerOneResultAwaitable(MultiAwaiterOneResultTask<T>& t):
		  p(t.getPromise()) {}
		MultiOwnerOneResultAwaitable(
		  std::shared_ptr<MultiOwnerPromise<T>>&& prom):
		  p(std::move(prom)) {}
		MultiOwnerOneResultAwaitable(
		  const MultiOwnerOneResultAwaitable&) = delete;
		MultiOwnerOneResultAwaitable(MultiOwnerOneResultAwaitable&& o):
		  p(std::move(o.p)) {}
		bool await_ready() { return p->done(); }
		void await_suspend() {}	 // TODO
		T await_resume() { return *p; }
	};

	template<typename T> class MultiAwaiterOneResultTask: public Task
	{
		friend class MultiOwnerOneResultAwaitable<T>;
		typedef MultiOwnerPromise<T> promise_type;
		std::shared_ptr<MultiOwnerPromise<T>> p;

	  public:
		std::shared_ptr<MultiOwnerPromise<T>> getPromise() { return p; }
		MultiAwaiterOneResultTask(promise_type::HandleType h):
		  p(&h.promise(), promise_type::destroy) {}
		MultiAwaiterOneResultTask(const MultiAwaiterOneResultTask&) = delete;
		MultiAwaiterOneResultTask(MultiAwaiterOneResultTask&& o):
		  p(std::move(o.p)) {}
		virtual std::optional<TaskResumeHandle> tryGetControl() final override {
			return {TaskResumeHandle(this)};
		}
		virtual std::optional<TaskResumeHandle> resume() final override {
			p->resume();
		}
	};
	/// NEXT FIX: TaskHandle needs to be virtual class!
	/// resume(), ~(), terminate(), ???
	///

}
