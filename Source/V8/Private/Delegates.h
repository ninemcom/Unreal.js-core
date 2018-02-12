#pragma once

#include "V8PCH.h"

namespace chakra
{
	struct IDelegateManager
	{
		static IDelegateManager* Create();
		virtual void Destroy() = 0;
		virtual JsValueRef GetProxy(JsValueRef This, UObject* Object, UProperty* Property) = 0;
	};
}