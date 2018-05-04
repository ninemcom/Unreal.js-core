#pragma once

#include "V8PCH.h"

namespace chakra
{
	class Isolate;
	template <class T> class Local;
	class Object;
	class Value;

	struct IDelegateManager
	{
		static IDelegateManager* Create();
		virtual void Destroy() = 0;
		virtual JsValueRef GetProxy(JsValueRef This, UObject* Object, UProperty* Property) = 0;
	};
}