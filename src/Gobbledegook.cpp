// Copyright 2017 Paul Nettle.
//
// This file is part of Gobbledegook.
//
// Gobbledegook is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Gobbledegook is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Gobbledegook.  If not, see <http://www.gnu.org/licenses/>.

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// >>
// >>>  INSIDE THIS FILE
// >>
//
// The methods in this file represent the complete external C interface for a Gobbledegook server.
//
// >>
// >>>  DISCUSSION
// >>
//
// Although Gobbledegook requires customization (see Server.cpp), it still maintains a public interface (this file.) This keeps the
// interface to the server simple and compact, while also allowing the server to be built as a library if the developer so chooses.
//
// As an alternative, it is also possible to publish customized Gobbledegook servers, allowing others to use the server through the
// public interface defined in this file.
//
// In addition, this interface is compatible with the C language, allowing non-C++ programs to interface with Gobbledegook, even
// though Gobbledgook is a C++ codebase. This should also simplify the use of this interface with other languages, such as Swift.
//
// The interface below has the following categories:
//
//     Log registration - used to register methods that accept all Gobbledegook logs
//     Update queue management - used for notifying the server that data has been updated
//     Server state - used to track the server's current running state and health
//     Server control - running and stopping the server
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

#include <string.h>
#include <string>
#include <thread>
#include <deque>
#include <mutex>

#include "Init.h"
#include "Logger.h"
#include "Server.h"

// During initialization, we'll check for complation at this interval
static const int kMaxAsyncInitCheckIntervalMS = 10;

// Our server thread
static std::thread serverThread;

// The current server state
volatile static GGKServerRunState serverRunState = EUninitialized;

// The current server health
volatile static GGKServerHealth serverHealth = EOk;

// Our update queue
typedef std::tuple<std::string, std::string> QueueEntry;
std::deque<QueueEntry> updateQueue;
std::mutex updateQueueMutex;

// ---------------------------------------------------------------------------------------------------------------------------------
//  _                                  _     _             _   _
// | |    ___   __ _    _ __ ___  __ _(_)___| |_ _ __ __ _| |_(_) ___  _ ___
// | |   / _ \ / _` |  | '__/ _ \/ _` | / __| __| '__/ _` | __| |/ _ \| '_  |
// | |__| (_) | (_| |  | | |  __/ (_| | \__ \ |_| | | (_| | |_| | (_) | | | |
// |_____\___/ \__, |  |_|  \___|\__, |_|___/\__|_|  \__,_|\__|_|\___/|_| |_|
//             |___/             |___/
//
// ---------------------------------------------------------------------------------------------------------------------------------

void ggkLogRegisterDebug(GGKLogReceiver receiver) { Logger::registerDebugReceiver(receiver); }
void ggkLogRegisterInfo(GGKLogReceiver receiver) { Logger::registerInfoReceiver(receiver); }
void ggkLogRegisterStatus(GGKLogReceiver receiver) { Logger::registerStatusReceiver(receiver); }
void ggkLogRegisterWarn(GGKLogReceiver receiver) { Logger::registerWarnReceiver(receiver); }
void ggkLogRegisterError(GGKLogReceiver receiver) { Logger::registerErrorReceiver(receiver); }
void ggkLogRegisterFatal(GGKLogReceiver receiver) { Logger::registerFatalReceiver(receiver); }
void ggkLogRegisterTrace(GGKLogReceiver receiver) { Logger::registerTraceReceiver(receiver); }
void ggkLogRegisterAlways(GGKLogReceiver receiver) { Logger::registerAlwaysReceiver(receiver); }

// ---------------------------------------------------------------------------------------------------------------------------------
//  _   _           _       _                                                                                                     _
// | | | |_ __   __| | __ _| |_ ___     __ _ _   _  ___ _   _  ___    _ __ ___   __ _ _ __   __ _  __ _  ___ _ __ ___   ___ _ __ | |_
// | | | | '_ \ / _` |/ _` | __/ _ \   / _` | | | |/ _ \ | | |/ _ \  | '_ ` _ \ / _` | '_ \ / _` |/ _` |/ _ \ '_ ` _ \ / _ \ '_ \| __|
// | |_| | |_) | (_| | (_| | ||  __/  | (_| | |_| |  __/ |_| |  __/  | | | | | | (_| | | | | (_| | (_| |  __/ | | | | |  __/ | | | |_
//  \___/| .__/ \__,_|\__,_|\__\___|   \__, |\__,_|\___|\__,_|\___|  |_| |_| |_|\__,_|_| |_|\__,_|\__, |\___|_| |_| |_|\___|_| |_|\__|
//       |_|                              |_|                                                     |___/
//
// Push/pop update notifications onto a queue. As these methods are where threads collide (i.e., this is how they communicate),
// these methods are thread-safe.
// ---------------------------------------------------------------------------------------------------------------------------------

// Adds an update to the front of the queue for a characteristic at the given object path
//
// Returns non-zero value on success or 0 on failure.
int ggkNofifyUpdatedCharacteristic(const char *pObjectPath)
{
	return ggkPushUpdateQueue(pObjectPath, "org.bluez.GattCharacteristic1") != 0;
}

// Adds an update to the front of the queue for a descriptor at the given object path
//
// Returns non-zero value on success or 0 on failure.
int ggkNofifyUpdatedDescriptor(const char *pObjectPath)
{
	return ggkPushUpdateQueue(pObjectPath, "org.bluez.GattDescriptor1") != 0;
}

// Adds a named update to the front of the queue. Generally, this routine should not be used directly. Instead, use the
// `ggkNofifyUpdatedCharacteristic()` instead.
//
// Returns non-zero value on success or 0 on failure.
int ggkPushUpdateQueue(const char *pObjectPath, const char *pInterfaceName)
{
	QueueEntry t(pObjectPath, pInterfaceName);

	std::lock_guard<std::mutex> guard(updateQueueMutex);
	updateQueue.push_front(t);
	return 1;
}

// Get the next update from the back of the queue and returns the element in `element` as a string in the format:
//
//     "com/object/path|com.interface.name"
//
// If the queue is empty, this method returns `0` and does nothing.
//
// `elementLen` is the size of the `element` buffer in bytes. If the resulting string (including the null terminator) will not
// fit within `elementLen` bytes, the method returns `-1` and does nothing.
//
// If `keep` is set to non-zero, the entry is not removed and will be retrieved again on the next call. Otherwise, the element
// is removed.
//
// Returns 1 on success, 0 if the queue is empty, -1 on error (such as the length too small to store the element)
int ggkPopUpdateQueue(char *pElementBuffer, int elementLen, int keep)
{
	std::string result;

	{
		std::lock_guard<std::mutex> guard(updateQueueMutex);

		// Check for an empty queue
		if (updateQueue.empty()) { return 0; }

		// Get the last element
		QueueEntry t = updateQueue.back();

		// Get the result string
		result = std::get<0>(t) + "|" + std::get<1>(t);

		// Ensure there's enough room for it
		if (result.length() + 1 > static_cast<size_t>(elementLen)) { return -1; }

		if (keep == 0)
		{
			updateQueue.pop_back();
		}
	}

	// Copy the element string
	memcpy(pElementBuffer, result.c_str(), result.length() + 1);

	return 1;
}

// Returns 1 if the queue is empty, otherwise 0
int ggkUpdateQueueIsEmpty()
{
	std::lock_guard<std::mutex> guard(updateQueueMutex);
	return updateQueue.empty() ? 1 : 0;
}

// Returns the number of entries waiting in the queue
int ggkUpdateQueueSize()
{
	std::lock_guard<std::mutex> guard(updateQueueMutex);
	return updateQueue.size();
}

// Removes all entries from the queue
void ggkUpdateQueueClear()
{
	std::lock_guard<std::mutex> guard(updateQueueMutex);
	updateQueue.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  ____                     _        _
// |  _ \ _   _ _ __     ___| |_ __ _| |_ ___
// | |_) | | | | '_ \   / __| __/ _` | __/ _ )
// |  _ <| |_| | | | |  \__ \ || (_| | ||  __/
// |_| \_\\__,_|_| |_|  |___/\__\__,_|\__\___|
//
// Methods for maintaining and reporting the state of the server
// ---------------------------------------------------------------------------------------------------------------------------------

// Retrieve the current running state of the server
//
// See `GGKServerRunState` (enumeration) for more information.
GGKServerRunState ggkGetServerRunState()
{
	return serverRunState;
}

// Convert a `GGKServerRunState` into a human-readable string
const char *ggkGetServerRunStateString(GGKServerRunState state)
{
	switch(state)
	{
		case EUninitialized: return "Uninitialized";
		case EInitializing: return "Initializing";
		case ERunning: return "Running";
		case EStopping: return "Stopping";
		case EStopped: return "Stopped";
		default: return "Unknown";
	}
}

// Internal method to set the run state of the server
void setServerRunState(GGKServerRunState newState)
{
	Logger::status(SSTR << "** SERVER RUN STATE CHANGED: " << ggkGetServerRunStateString(serverRunState) << " -> " << ggkGetServerRunStateString(newState));
	serverRunState = newState;
}

// Convenience method to check ServerRunState for a running server
int ggkIsServerRunning()
{
	return serverRunState == ERunning ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  ____                              _                _ _   _
// / ___|  ___ _ ____   _____ _ __   | |__   ___  __ _| | |_| |___
// \___ \ / _ \ '__\ \ / / _ \ '__|  | '_ \ / _ \/ _` | | __| '_  |
//  ___) |  __/ |   \ V /  __/ |     | | | |  __/ (_| | | |_| | | |
// |____/ \___|_|    \_/ \___|_|     |_| |_|\___|\__,_|_|\__|_| |_|
//
// Methods for maintaining and reporting the health of the server
// ---------------------------------------------------------------------------------------------------------------------------------

// Retrieve the current health of the server
//
// See `GGKServerHealth` (enumeration) for more information.
GGKServerHealth ggkGetServerHealth()
{
	return serverHealth;
}

// Convert a `GGKServerHealth` into a human-readable string
const char *ggkGetServerHealthString(GGKServerHealth state)
{
	switch(state)
	{
		case EOk: return "Ok";
		case EFailedInit: return "Failed initialization";
		case EFailedRun: return "Failed run";
		default: return "Unknown";
	}
}

// Internal method to set the health of the server
void setServerHealth(GGKServerHealth newState)
{
	serverHealth = newState;
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  ____  _                 _   _
// / ___|| |_ ___  _ __    | |_| |__   ___    ___  ___ _ ____   _____ _ __
// \___ \| __/ _ \| '_ \   | __| '_ \ / _ \  / __|/ _ \ '__\ \ / / _ \ '__|
//  ___) | || (_) | |_) |  | |_| | | |  __/  \__ \  __/ |   \ V /  __/ |
// |____/ \__\___/| .__/    \__|_| |_|\___|  |___/\___|_|    \_/ \___|_|
//                |_|
//
// ---------------------------------------------------------------------------------------------------------------------------------

// Tells the server to begin the shutdown process
//
// The shutdown process will interrupt any currently running asynchronous operation and prevent new operations from starting.
// Once the server has stabilized, its event processing loop is terminated and the server is cleaned up.
//
// `ggkGetServerRunState` will return EStopped when shutdown is complete. To block until the shutdown is complete, see
// `ggkWait()`.
//
// Alternatively, you can use `ggkShutdownAndWait()` to request the shutdown and block until the shutdown is complete.
void ggkTriggerShutdown()
{
	shutdown();
}

// Convenience method to trigger a shutdown (via `ggkTriggerShutdown()`) and also waits for shutdown to complete (via
// `ggkWait()`)
int ggkShutdownAndWait()
{
	// Tell the server to shut down
	ggkTriggerShutdown();

	// Block until it has shut down completely
	return ggkWait();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// __        __    _ _
// \ \      / /_ _(_) |_     ___  _ __     ___  ___ _ ____   _____ _ __
//  \ \ /\ / / _` | | __|   / _ \| '_ \   / __|/ _ \ '__\ \ / / _ \ '__|
//   \ V  V / (_| | | |_   | (_) | | | |  \__ \  __/ |   \ V /  __/ |
//    \_/\_/ \__,_|_|\__|   \___/|_| |_|  |___/\___|_|    \_/ \___|_|
//
// ---------------------------------------------------------------------------------------------------------------------------------

// Blocks for up to maxAsyncInitTimeoutMS milliseconds until the server shuts down.
//
// If shutdown is successful, this method will return a non-zero value. Otherwise, it will return 0.
//
// If the server fails to stop for some reason, the thread will be killed.
//
// Typically, a call to this method would follow `ggkTriggerShutdown()`.
int ggkWait()
{
	try
	{
		serverThread.join();
	}
	catch(std::system_error &ex)
	{
		if (ex.code() == std::errc::invalid_argument)
		{
			Logger::warn(SSTR << "Server thread was not joinable during stop(): " << ex.what());
		}
		else if (ex.code() == std::errc::no_such_process)
		{
			Logger::warn(SSTR << "Server thread was not valid during stop(): " << ex.what());
		}
		else if (ex.code() == std::errc::resource_deadlock_would_occur)
		{
			Logger::warn(SSTR << "Deadlock avoided in call to stop() (did the server thread try to stop itself?): " << ex.what());
		}
		else
		{
			Logger::warn(SSTR << "Unknown system_error code (" << ex.code() << "): " << ex.what());
		}
	}

	// Return true if we're stopped, otherwise false
	return ggkGetServerRunState() == EStopped ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
//  ____  _             _      _   _
// / ___|| |_ __ _ _ __| |_   | |_| |__   ___    ___  ___ _ ____   _____ _ __
// \___ \| __/ _` | '__| __|  | __| '_ \ / _ \  / __|/ _ \ '__\ \ / / _ \ '__|
//  ___) | || (_| | |  | |_   | |_| | | |  __/  \__ \  __/ |   \ V /  __/ |
// |____/ \__\__,_|_|   \__|   \__|_| |_|\___|  |___/\___|_|    \_/ \___|_|
//
// ---------------------------------------------------------------------------------------------------------------------------------

// Set the server state to 'EInitializing' and then immediately create a server thread and initiate the server's async
// processing on the server thread.
//
// At that point the current thread will block for maxAsyncInitTimeoutMS milliseconds or until initialization completes.
//
// If initialization was successful, the method will return a non-zero value with the server running on its own thread in
// 'runServerThread'.
//
// If initialization was unsuccessful, this method will continue to block until the server has stopped. This method will then
// return 0.
//
// IMPORTANT:
//
// The data setter uses void* types to allow receipt of unknown data types from the server. Ensure that you do not store these
// pointers. Copy the data before returning from your getter delegate.
//
// Similarly, the pointer to data returned to the data getter should point to non-volatile memory so that the server can use it
// safely for an indefinite period of time.
int ggkStart(GGKServerDataGetter getter, GGKServerDataSetter setter, int maxAsyncInitTimeoutMS)
{
	// Allocate our server
	TheServer = std::make_shared<Server>(getter, setter);

	// Start our server thread
	try
	{
		serverThread = std::thread(runServerThread);
	}
	catch(std::system_error &ex)
	{
		if (ex.code() == std::errc::resource_unavailable_try_again)
		{
			Logger::error(SSTR << "Server thread was unable to start during start(): " << ex.what());

			setServerRunState(EStopped);
			return 0;
		}
	}

	// Waits for the server to pass the EInitializing state
	int retryTimeMS = 0;
	while (retryTimeMS < maxAsyncInitTimeoutMS && ggkGetServerRunState() <= EInitializing)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(kMaxAsyncInitCheckIntervalMS));
		retryTimeMS += kMaxAsyncInitCheckIntervalMS;
	}

	// If something went wrong, shut down if we've not already done so
	if (ggkGetServerRunState() < ERunning || ggkGetServerHealth() != EOk)
	{
		setServerHealth(EFailedInit);

		// Stop the server (this is a blocking call)
		if (!ggkShutdownAndWait())
		{
			Logger::warn(SSTR << "Unable to stop the server from error in start()");
		}

		return 0;
	}

	// Everything looks good
	return 1;
}