#include "Common.h"
#include "Event.h"
#include "Core.h"
#include "Serialization.h"
#include "Sampler.h"
#include <DbgHelp.h>
#include <unordered_set>
#include "HPTimer.h"

namespace Brofiler
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct CallStackTreeNode
{
	DWORD64 dwArddress;
	uint32 invokeCount;

	std::list<CallStackTreeNode> children;

	CallStackTreeNode()									: dwArddress(0), invokeCount(0) {}
	CallStackTreeNode(DWORD64 address)	: dwArddress(address), invokeCount(0) {} 

	bool Merge(const CallStack& callstack, size_t index)
	{
		++invokeCount;
		if (index == 0)
			return true;

		// I suppose, that usually sampling function has only several children.. so linear search will be fast enough
		DWORD64 address = callstack[index];
		for (auto it = children.begin(); it != children.end(); ++it)
			if (it->dwArddress == address)
				return it->Merge(callstack, index - 1);

		// Didn't find node => create one
		children.push_back(CallStackTreeNode(address));
		return children.back().Merge(callstack, index - 1); 
	}

	void CollectAddresses(std::unordered_set<DWORD64>& addresses) const
	{
		addresses.insert(dwArddress);
		for each (const auto& node in children)
			node.CollectAddresses(addresses);
	}

	OutputDataStream& Serialize(OutputDataStream& stream) const
	{
		stream << (uint64)dwArddress << invokeCount;

		stream << (uint32)children.size();
		for each (const CallStackTreeNode& node in children)
			node.Serialize(stream);

		return stream;
	}
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Sampler::StopSampling()
{
	if (!IsActive())
		return false;

	SetEvent(finishEvent);

	DWORD result = WaitForSingleObject(workerThread, INFINITE);
	BRO_UNUSED(result);
	BRO_ASSERT(result == WAIT_OBJECT_0, "Can't stop sampling thread!");

	CloseHandle(workerThread);
	workerThread = nullptr;

	CloseHandle(finishEvent);
	finishEvent = nullptr;

	targetThreads.clear();

	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Sampler::Sampler() : workerThread(nullptr), finishEvent(nullptr), intervalMicroSeconds(300)
{
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Sampler::~Sampler()
{
	StopSampling();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void Sampler::StartSampling(const std::vector<ThreadEntry*>& threads, uint samplingInterval)
{
	threads;
	samplingInterval;

#if USE_BROFILER_SAMPLING
	symEngine.Init();

	intervalMicroSeconds = samplingInterval;
	targetThreads = threads;

	if (IsActive())
		StopSampling();

	callstacks.clear();

	BRO_VERIFY(finishEvent == nullptr && workerThread == nullptr, "Can't start sampling!", return);

	finishEvent = CreateEvent(NULL, false, false, 0);
	workerThread = CreateThread(NULL, 0, &Sampler::AsyncUpdate, this, 0, NULL);

	BRO_ASSERT(finishEvent && workerThread, "Sampling was not started!")
#endif
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Sampler::IsActive() const
{
	return workerThread != nullptr || finishEvent != nullptr;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ClearStackContext(CONTEXT& context)
{
	memset(&context, 0, sizeof(context));
	context.ContextFlags = CONTEXT_FULL;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if USE_BROFILER_SAMPLING
DWORD WINAPI Sampler::AsyncUpdate(LPVOID lpParam)
{
	Sampler& sampler = *(Sampler*)(lpParam);

	std::vector<std::pair<HANDLE, ThreadEntry*>> openThreads;
	openThreads.reserve(sampler.targetThreads.size());

	for each (ThreadEntry* entry in sampler.targetThreads)
	{
		DWORD threadID = entry->description.threadID;
		BRO_VERIFY(threadID != GetCurrentThreadId(), "It's a bad idea to sample specified thread! Deadlock will occur!", continue);

		HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, threadID);
		if (hThread == NULL)
			continue;

		openThreads.push_back(std::make_pair(hThread, entry));
	}

	if (openThreads.empty())
		return 0;

	CallStackBuffer buffer;

	CONTEXT context;

	ClearStackContext(context);

	while (WaitForSingleObject(sampler.finishEvent, 0) == WAIT_TIMEOUT)
	{
		SpinSleep(sampler.intervalMicroSeconds);

		// Check whether we are inside sampling scope
		for each (const auto& entry in openThreads)
		{
			HANDLE handle = entry.first;
			const ThreadEntry* thread = entry.second;

			// Get storage from TLS slot (it can be replaced by Fibers)
			EventStorage* storage = *thread->threadTLS;
			if (!storage || !storage->isSampling)
				continue;

			uint count = 0;

			DWORD suspendedStatus = SuspendThread(handle);

			if (suspendedStatus != (DWORD)-1)
			{
				// Check scope again because it is possible to leave sampling scope while trying to suspend main thread
				if (storage->isSampling && GetThreadContext(handle, &context))
				{
					count = sampler.symEngine.GetCallstack(handle, context, buffer);
				}

				ClearStackContext(context);
				ResumeThread(handle);
			}

			if (count > 0)
			{
				sampler.callstacks.push_back(CallStack(buffer.begin(), buffer.begin() + count));
			}

			ClearStackContext(context);
		}
	}

	for each (const auto& entry in openThreads)
		CloseHandle(entry.first);

	return 0;
}
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
OutputDataStream& operator<<(OutputDataStream& os, const Symbol * const symbol)
{
	BRO_VERIFY(symbol, "Can't serialize NULL symbol!", return os);
	return os << (uint64)symbol->address << symbol->module << symbol->function << symbol->file << symbol->line;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
OutputDataStream& Sampler::Serialize(OutputDataStream& stream)
{
	BRO_VERIFY(!IsActive(), "Can't serialize active Sampler!", return stream);

	stream << (uint32)callstacks.size();

	CallStackTreeNode tree;

	Core::Get().DumpProgress("Merging CallStacks...");

	for each (const CallStack& callstack in callstacks)
		if (!callstack.empty())
			tree.Merge(callstack, callstack.size() - 1);

	std::unordered_set<DWORD64> addresses;
	tree.CollectAddresses(addresses);

	Core::Get().DumpProgress("Resolving Symbols...");

	std::vector<const Symbol*> symbols;
	for each (DWORD64 address in addresses)
		if (const Symbol* symbol = symEngine.GetSymbol(address))
			symbols.push_back(symbol);

	stream << symbols;

	tree.Serialize(stream);

	// Clear temporary data for dbghelp.dll (http://microsoft.public.windbg.narkive.com/G2WkSt2k/stackwalk64-performance-problems)
	//symEngine.Close();

	return stream;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool Sampler::IsSamplingScope() const
{
	for each (const ThreadEntry* entry in targetThreads)
		if (const EventStorage* storage = *entry->threadTLS)
			if (storage->isSampling)
				return true;

	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t Sampler::GetCollectedCount() const
{
	return callstacks.size();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
}