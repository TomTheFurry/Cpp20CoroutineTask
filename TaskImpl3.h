#pragma once
#include <coroutine>
#include <atomic>
#include <memory>
#include <assert.h>
#include "AtomicStructure.h"
#include <ranges>

// coroutine task implmentation 3
// Try to use tree structure to implement task relationship, and a iterator to
// traverse the tree.

// TODONEXT: Figure out weak_ptr shared_ptr and dtor relations!
template<typename T, void Dtor(T*)> class WeakPtrToSelf
	: std::enable_shared_from_this<T>
{
  protected:
	WeakPtrToSelf(): p(this, Dtor) {}
	const std::weak_ptr<T>& getWeakPtr() { return p; }
	std::shared_ptr<T> makeSharedPtr() { return p.lock(); }
};

template<class... Ts> struct overloadLambda: Ts... { using Ts::operator()...; };

// Basic tree node class
namespace tk {
	class AwaitOpNode;
	class TaskNode;
	class ResultContainer
	{};

	enum class TaskStatus {
		Locked,		  // Locked. Locker can call any func
		Awaiting,	  // Non Locked. Only atomic func
		ToBeResumed,  // Non Locked. Only atomic func OR tryClaim()
		End,		  // Non Locked. Only atomic func
	};

	class TaskNode: public WeakPtrToSelf<TaskNode, TaskNode::free>
	{
	  protected:
		std::atomic<TaskStatus> status;
		std::atomic<std::shared_ptr<AwaitOpNode>> awaitOp{};
		TaskNode() = default;
		TaskNode(const TaskNode&) = delete;
		TaskNode(TaskNode&&) = delete;

		// Try claim a result.
		// Return: nullptr == no result yet
		// Note: Termination counts as a immediate result
		virtual ResultContainer* onPopResult() = 0;
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
		virtual ResultContainer* onClaimedAndPopResult() = 0;

	  public:
		const auto& getStatus() { return status; }
		// Atomic func, the box should be reserved after that pop
		// Note: Termination counts as a immediate result
		ResultContainer* tryPopResult() {
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
					std::shared_ptr<AwaitOpNode> awaitOpRef = awaitOp.load();
					AwaitOpNode* op = awaitOpRef.get();
					if (op == nullptr) return false;
					if (op->helpAwaitee()) return true;
				}
				case TaskStatus::End: return true;
				}
				return false;
			}
		}
		virtual void onFree() = 0;
		static void free(TaskNode* t) { t->onFree(); }
	};
	class AwaitOpNode
	{
	  protected:
		AwaitOpNode() = default;

	  public:
		// Try help completing awaitees.
		// Note: It should not allow helping claimed awaitee
		// Return: std::tuple<bool,bool>: first bool is whether it did help,
		// second is whether the AwaitOp is done
		virtual bool helpAwaitee() = 0;
		virtual ~AwaitOpNode() = default;
	};
	template<std::derived_from<TaskNode> Task>
	class StartNode
	{
		// Atomic list of tasks that are active
		atomics::RingQueueBuffer<std::weak_ptr<TaskNode>> beginBuffer;

	  public:
		bool atomicPushActiveLink(std::weak_ptr<TaskNode> node) {
			atomics::Result result;
			do {
				result = beginBuffer.push(node);
			} while (result == atomics::Result::Contention);
			return result == atomics::Result::Success;
		}
		bool atomicPushActiveLink(const std::ranges::sized_range auto& node) {
			atomics::Result result;
			do {
				result = beginBuffer.push(node);
			} while (result == atomics::Result::Contention);
			return result == atomics::Result::Success;
		}
	};
}

// AwaitOp node class
namespace tk {

	class AwaitSingle: AwaitOpNode
	{

	  public:
		AwaitSingle(TaskNode f): future(f) {}
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
}

