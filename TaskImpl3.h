#pragma once
#include <coroutine>
#include <atomic>
#include <memory>
#include <assert.h>

// coroutine task implmentation 3
// Try to use tree structure to implement task relationship, and a iterator to
// traverse the tree.


template<typename T, void Dtor(T*)> class WeakPtrToSelf
{
	std::weak_ptr<T> p;

  protected:
	WeakPtrToSelf(): p(this, Dtor) {}
	const std::weak_ptr<T>& getWeakPtr() { return p; }
	std::shared_ptr<T> makeSharedPtr() { return p.lock(); }
};

template<class... Ts> struct overloadLambda: Ts... { using Ts::operator()...; };

// Basic tree node class
namespace tk {
	class StartNode;
	class AwaitOpNode;
	class TaskNode;

	class StartNode
	{
		// Atomic list of tasks that are active
		// TODO: Atomic ring buffer

	  public:
		void atomicAddActiveLink(TaskNode* node) {
			activeLink.fetch_add(1, std::memory_order_relaxed);
		}

	}


	// Await Ops
	namespace tk {
		// Abstract class for await ops
		class AwaitOpNode
		{
		  protected:
			AwaitOpNode() {}

		  public:
			virtual void registerNode()
		}


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

			// Can modify any state as long as it didn't break other write-safe
			// func It should resume the coroutine unitl it co_awaits on others
			// or it has gotten one or more result (either by co_return or
			// co_yield) Note: Termination counts as a immediate result Note:
			// The amount of result it generates is not limited but it should
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
				if (status.load(std::memory_order::relaxed)
					== TaskStatus::Locked)
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
			// Note: The result may be immediately claimed by other task. So it
			// is more suitable to use tryCompleteAndPopResult() instead to get
			// the result Return: true if a result is generated
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
