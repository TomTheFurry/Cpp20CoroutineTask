#pragma once
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

namespace task {
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
		virtual void notifyAwaitorTermination(Task* t) = 0;
		virtual void notifyAwaiteeTermination(Task* t) = 0;
		virtual void notifyForcedTermination() = 0;
		virtual std::variant<TaskStrongHandle, Task*> notifyNewAwaitor() = 0;
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
		// Return whether to cascade the termination to awaitor
		virtual bool processTermination(Task*) = 0;
		virtual void processAwaitorTermination() = 0;
	};

	class AwaitSingle: public AwaitOperation
	{
		Task* task;

	  public:
		AwaitSingle(Task* t): task(t) {}
		virtual bool processTermination(Task* t) {
			assert(task == t);
			return true;
		}
	};

	class AwaitEither: public AwaitOperation
	{
		std::vector<Task*> tasks;

	  public:
		// TODO: Add more type??
		AwaitEither(std::vector<Task*>&& ts): tasks(std::move(ts)) {}
		virtual bool processTermination(Task* t) final override {
			bool allNull = true;
			bool foundTask = false;
			for (auto& task : tasks)
			{
				if (task == t)
				{
					task = nullptr;
					foundTask = true;
				}
				if (task != nullptr) allNull = false;
				if (foundTask && !allNull) break;
			}
			assert(foundTask);
			return allNull;
		}
		virtual void processAwaitorTermination() final override;
	};

	class AwaitAll: public AwaitOperation
	{
		std::vector<Task*> tasks;

	  public:
		// TODO: Add more type??
		AwaitAll(std::vector<Task*>&& ts): tasks(std::move(ts)) {}
		virtual bool processTermination(Task* t) final override {
			for (auto& task : tasks)
			{
				if (task == t)
				{
					task = nullptr;
					return true;
				}
			}
			assert(false);
			return false;
		}
		virtual void processAwaitorTermination() final override {
			for (auto& task : tasks)
			{
				if (task == nullptr) continue;
			}
		}
	};
}
// NEXT TO FIX: Race condition on prcessTermination() & ~Task()...
//
// Fix 1: Use std::shared_ptr && std::weak_ptr to safe-gaurd ownership. Assure
// task destruction only when all awaitees are destructed or are relying on
// other tasks
//
// Fix 2: Lazy cleanup using atomic counter. Only flush task after certain time.
//
// Fix 3: Use a Task Pool impl. Any task not owned should be in that pool. When
// Worker picks up a task, check if task can be destructed
//
// Fix 4: Add atomicPopTask() to AwaitOperation, making all access to tasks in
// AwaitOperation thread-safe. Not sure how this work with getting results
// though...
//
// Fix 5: Mandate the use of locks on processing awaitOperation updates, and
// only allow updating the awaitOperation from Task Owner.
//
// Fix 6: Mandate the use of double request buffers on any changes to
// awaitOperation, and only allow updating the awaitOperation from Task Owner.
//
// Fix 7: Use a Java-like lazy collector, where tasks not referenced to base
// starting task is automatically cleaned up.
//
// Fix 8:

namespace task {

	template<typename T> class OneToOneTask;
	template<typename T> class OneToOneTaskStrongHandle;



	template<typename T> class OneToOneTaskStrongHandle: public TaskStrongHandle
	{
		template<typename U> friend class OneToOneTask;
		OneToOneTask<T>& promise;

	  private:
		OneToOneTaskStrongHandle(OneToOneTask<T>& p): promise(p) {}

	  public:
		using promise_type = OneToOneTask<T>;
		// virtual Task& getTask();
		virtual std::optional<TaskStrongHandle> resume() final override {
			promise.resume();
			if (promise.awaitees.empty()) {}
		}
		virtual std::optional<TaskStrongHandle> terminate() final override {}
		virtual ~OneToOneTaskStrongHandle() final override {}
	};
	template<typename T> class OneToOneTask: public BasicResultBox<T>, Task
	{
		template<typename U> friend class OneToOneTaskStrongHandle;
		std::atomic_flag terminationFlag{};
		std::atomic_bool claimed = false;
		std::vector<Task*> awaitees{};
		std::vector<Task*> terminatedAwaitees{};
		Task* awaitor = nullptr;
		OneToOneTaskStrongHandle<T> getHandle() { return *this; }
		void resume() { HandleType::from_promise(*this).resume(); }

	  public:
		typedef std::coroutine_handle<OneToOneTask<T>> HandleType;
		typedef T ValueType;

		virtual std::optional<TaskStrongHandle> tryClaimTask() final override {
			bool e = false;
			if (claimed.compare_exchange_strong(e, true))
				return std::make_optional<TaskStrongHandle>(*this);
			else
				return {};
		}
		virtual void notifyAwaitorTermination(Task* t) final override {
			assert(t == awaitor);
			bool doneAlready =
			  terminationFlag.test_and_set();  // TODO Mem order
			if (doneAlready) return;
			for (auto& tPtr : awaitees) tPtr->notifyAwaitorTermination(this);
		}
		virtual void notifyAwaiteeTermination(Task* t) final override {
			terminatedAwaitees.push_back(t);
		}
		virtual void notifyForcedTermination() = 0;
		virtual std::variant<TaskStrongHandle, Task*> notifyNewAwaitor() = 0;

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
