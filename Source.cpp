//#include "Scheduler.h"
//#include "JobSystem.h"
#include "TaskImpl1.h"

task::OneToOneCoroutine<int> getInc(int begin) { co_return begin + 1; }



int main() {
	// mainLoop();
	//coro::mainJobLoop();


	return 0;
}
