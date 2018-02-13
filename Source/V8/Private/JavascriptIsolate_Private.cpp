
#ifndef THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_END
#endif

#include "JavascriptIsolate_Private.h"
#include "Templates/Tuple.h"
#include "Config.h"
#include "MallocArrayBufferAllocator.h"
#include "Translator.h"
#include "ScopedArguments.h"
#include "Exception.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext_Private.h"
#include "JavascriptContext.h"
#include "Helpers.h"
#include "JavascriptGeneratedClass.h"
#include "JavascriptGeneratedClass_Native.h"
#include "StructMemoryInstance.h"
#include "JavascriptMemoryObject.h"
#include "Engine/UserDefinedStruct.h"
#include "V8PCH.h"
#include "UObjectIterator.h"
#include "TextProperty.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif
#include "JavascriptStats.h"
#include "IV8.h"

THIRD_PARTY_INCLUDES_START
//#include <libplatform/libplatform.h>
THIRD_PARTY_INCLUDES_END

//using namespace v8;
PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

FString PropertyNameToString(UProperty* Property)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name);
		}
	}
	return name.ToString();
}

bool MatchPropertyName(UProperty* Property, FName NameToMatch)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name) == NameToMatch.ToString();
		}
	}
	return name == NameToMatch;
}

class FJavascriptIsolateImplementation : public FJavascriptIsolate
{
public:
	void RegisterSelf(Isolate* isolate)
	{
		isolate_ = isolate;
		isolate->SetData(0, this);

		Delegates = IDelegateManager::Create(isolate);

#if STATS
		SetupCallbacks();
#endif
	}

#if STATS
	FCycleCounter Counter[4];	

	void OnGCEvent(bool bStart, GCType type, GCCallbackFlags flags)
	{
		auto GetStatId = [](int Index) -> TStatId {
			switch (Index)
			{
			case 0: return GET_STATID(STAT_Scavenge);
			case 1: return GET_STATID(STAT_MarkSweepCompact);
			case 2: return GET_STATID(STAT_IncrementalMarking);
			default: return GET_STATID(STAT_ProcessWeakCallbacks);
			}
		};

		auto GCEvent = [&](int Index) {
			if (bStart)
			{
				Counter[Index].Start(GetStatId(Index)); 
			}
			else
			{
				Counter[Index].Stop();
			}
		};

		for (int32 Index = 0; Index < ARRAY_COUNT(Counter); ++Index)
		{
			if (type & (1 << Index))
			{
				GCEvent(Index);
			}
		}		
	}

	static void OnMemoryAllocationEvent(ObjectSpace space, AllocationAction action, int size)
	{
		FName StatId;
		switch (space)
		{
		case kObjectSpaceNewSpace: StatId = GET_STATFNAME(STAT_NewSpace); break;
		case kObjectSpaceOldSpace: StatId = GET_STATFNAME(STAT_OldSpace); break;
		case kObjectSpaceCodeSpace: StatId = GET_STATFNAME(STAT_CodeSpace); break;
		case kObjectSpaceMapSpace: StatId = GET_STATFNAME(STAT_MapSpace); break;
		case kObjectSpaceLoSpace: StatId = GET_STATFNAME(STAT_LoSpace); break;
		default: return;
		}

		if (action == kAllocationActionAllocate)
		{
			INC_DWORD_STAT_FNAME_BY(StatId, size);
		}
		else
		{
			DEC_DWORD_STAT_FNAME_BY(StatId, size);
		}		
	}

	void SetupCallbacks()
	{
		isolate_->AddGCEpilogueCallback([](Isolate* isolate, GCType type, GCCallbackFlags flags) {
			GetSelf(isolate)->OnGCEvent(false, type, flags);			
		});		

		isolate_->AddGCPrologueCallback([](Isolate* isolate, GCType type, GCCallbackFlags flags) {
			GetSelf(isolate)->OnGCEvent(true, type, flags);
		});

#if V8_MAJOR_VERSION == 5 && V8_MINOR_VERSION < 3
		isolate_->AddMemoryAllocationCallback([](ObjectSpace space, AllocationAction action,int size) {
			OnMemoryAllocationEvent(space, action, size);
		}, kObjectSpaceAll, kAllocationActionAll);
#endif
	}
#endif

};

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
