#pragma once

#include "V8PCH.h"

struct FStructMemoryInstance;
class FJavascriptIsolate;
struct IPropertyOwner;

struct FPendingClassConstruction
{
	FPendingClassConstruction() {}
	FPendingClassConstruction(JsValueRef InObject, UClass* InClass)
		: Object(InObject), Class(InClass)
	{
		JsAddRef(InObject, nullptr);
	}

	Persistent<JsValueRef> Object;
	UClass* Class;
	bool bCatched{ false };

	void Finalize(FJavascriptIsolate* Isolate, UObject* Object);
};
