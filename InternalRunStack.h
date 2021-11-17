#pragma once

#include <coroutine>

namespace tk {

	namespace internal {
		// A void c++20 coroutine that executes a task synchronously until it is
		// finished or its not possible to continue execution. Then it co_yield
		// the next task to run, or return if there is no next task.

		class ExecuteSessionHandle
		{
		  public:
			class promise_type
			{
				struct SuspendAndRun {
					std::coroutine_handle<> handle;
					constexpr void await_ready() noexcept {}
					constexpr std::coroutine_handle<> await_suspend(
					  std::coroutine_handle<> h) noexcept {
						auto toBeRun = handle;
						handle = h;
						return toBeRun;
					}
					constexpr void await_resume() noexcept { auto }
				};

			  public:
				promise_type() = default;
				promise_type(const promise_type&) = delete;
				promise_type& operator=(const promise_type&) = delete;
				~promise_type() = default;

				constexpr std::coroutine_handle<promise_type>
				  getCoroutine() noexcept {
					return std::coroutine_handle<promise_type>::from_promise(
					  *this);
				}

				auto get_return_object() { return ExecuteSessionHandle{}; }
				constexpr auto initial_suspend() {
					return std::suspend_never{};
				}
				constexpr auto final_suspend() { return std::suspend_always{}; }
				constexpr void return_void() {}

				static_assert(Promise<promise_type>,
				  "Something broke and this is no longer a Promise...");

				SuspendAndRun<promise_type> yield_value(
				  ExecuteSessionHandle::promise_type& next) {
					return {&next};
				}
			};
		};



	}


}