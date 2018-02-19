#pragma once
#include "V8PCH.h"

#define JsCheck(expression) chakra::Check(expression)

enum class EPropertyOwner
{
	None,
	Object,
	Memory
};

struct IPropertyOwner
{
	EPropertyOwner Owner;
};

struct FNoPropertyOwner : IPropertyOwner
{
	FNoPropertyOwner()
	{
		Owner = EPropertyOwner::None;
	}
};

namespace chakra
{
	//void ReportException(Isolate* isolate, TryCatch& try_catch);
	JsValueRef Chakra_KeywordString(const FString& String);
	JsValueRef Chakra_KeywordString(const char* String);
	FString StringFromChakra(JsValueRef Value);
	void CallJavascriptFunction(JsContextRef context, JsValueRef This, UFunction* SignatureFunction, JsFunctionRef func, void* Parms);
	UClass* UClassFromChakra(JsValueRef Value);
	UObject* UObjectFromChakra(JsValueRef Value);
	uint8* RawMemoryFromChakra(JsValueRef Value);
	FString StringFromArgs(const JsValueRef* args, unsigned short nargs, int StartIndex = 0);
	void Check(JsErrorCode Error);
}
