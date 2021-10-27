#pragma once
#include <coroutine>
#include <vector>
#include <thread>

typedef unsigned int uint;

namespace task {
	class TaskManager;
	class TaskWorker;

	class TaskManager
	{
		std::vector<TaskWorker> workers;

	  public:
		TaskManager(size_t numOfThreads) { workers.resize(numOfThreads); }
		void addAsyncTask() {}
	};

	class TaskWorker
	{
		friend class TaskManager;
		std::thread thread;
		std::atomic<uint> state;
		TaskWorker(): state(0), thread(&_main, *this) {}





		static void _main(TaskWorker& w) { w.main(); }
		void main() {
			while (true)
			{
				uint cState = state.load();
				state.wait(cState);
				// State changed. Do something???
			}
		}
	};
}
