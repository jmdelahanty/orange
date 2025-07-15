#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "offthreadmachine.h"

COffThreadMachine::COffThreadMachine(const char* tName)
:threadOn(0), threadHandle(0), cpuToUse(-1)
{
	if(tName) { 
		strncpy(threadName, tName, 63);
		threadName[63] = 0; // Ensure null termination
	}
	else threadName[0] = 0;
}

COffThreadMachine::~COffThreadMachine()
{
	StopThread();
}

/*
   Return CAPNAV_ERROR_SUCCESS if thread started succeessfully; CAPNAV_ERROR_ALEADY_EXISTS if thread is already started,
   CAPNAV_ERROR_CREATE if fail to creat thread object.
*/
int COffThreadMachine::StartThread(const char* tName)
{
    if (threadHandle) return EEXIST;

    // If a new name is provided when starting, update the member variable.
    if (tName) {
        strncpy(threadName, tName, sizeof(threadName) - 1);
        threadName[sizeof(threadName) - 1] = 0; // Ensure null termination
    }

#if defined(__GNUC__)
    // Create the thread first.
    if (pthread_create(&threadHandle, NULL, MachineThread, (void*)this) != 0) {
        return ECHILD; // Failed to create thread
    }

    // --- Start of Naming Logic ---
    if (threadName[0] != '\0') // Only set name if one exists
    {
        // Create a temporary buffer that is the exact size required by the OS.
        char truncated_name[16];
        strncpy(truncated_name, threadName, 15);
        truncated_name[15] = 0; // Manually ensure null termination

        // Set the thread name using the safe, truncated version.
        pthread_setname_np(threadHandle, truncated_name);
    }
    // --- End of Naming Logic ---

#else
    // Windows version remains the same
    if (!(threadHandle = CreateThread(NULL, 0, MachineThread, (void*)this, 0, NULL))) {
        return ECHILD;
    }
    // SetThreadDescription(threadHandle, ...); // Windows naming call would go here
#endif

    return 0; // Success
}

void COffThreadMachine::StopThread()
{
	if(threadHandle && threadOn)
	{
		threadOn = 0;
		DoStopThread();
#if defined(__GNUC__)
		pthread_join(threadHandle, NULL); 
#else			
		WaitForSingleObject(threadHandle,INFINITE);
		CloseHandle(threadHandle);
#endif			
	}
    threadOn = 0;
    threadHandle = 0;
}

void COffThreadMachine::DoStopThread()
{

}

THREAD_FUNCTION COffThreadMachine::MachineThread(void* arg)
{
	COffThreadMachine* self = (COffThreadMachine*)arg;

	if(self->cpuToUse >= 0) //cpuToUse is 0-based.
    {
#if defined(__GNUC__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(self->cpuToUse, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
        SetThreadAffinityMask(GetCurrentThread(), ((unsigned long long) 1) << self->cpuToUse);
#endif

#ifdef CAPTURE
        LOG("Thread: %s, Setting CPU to %d.\n", self->threadName, self->cpuToUse);
#else
		
#endif

    }
	self->threadOn = 1;
	self->ThreadRunning();
	self->threadOn = 0;
	return NULL;
}

