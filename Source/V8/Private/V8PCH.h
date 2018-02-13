#pragma once

#include "CoreMinimal.h"
#include "ObjectMacros.h"
#include "Object.h"
#include "UObjectGlobals.h"
#include "ScriptMacros.h"

#pragma warning( push )
#pragma warning( disable : 4946 )
#pragma warning( disable : 4191 )

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#ifndef THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_START
#	define THIRD_PARTY_INCLUDES_END
#endif

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_START
#include "ChakraCore.h"
#include "ChakraCommon.h"
#include "ChakraDebug.h"
#include "ChakraCoreVersion.h"
THIRD_PARTY_INCLUDES_END

#if PLATFORM_WINDOWS
#include "HideWindowsPlatformTypes.h"
#endif

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS

#pragma warning( pop )

DECLARE_LOG_CATEGORY_EXTERN(Javascript, Log, All);

typedef JsValueRef JsFunctionRef;

struct IJavascriptDebugger
{
	virtual ~IJavascriptDebugger() {}

	virtual void Destroy() = 0;

	static IJavascriptDebugger* Create(int32 InPort, JsContextRef InContext);
};

struct IJavascriptInspector
{
	virtual ~IJavascriptInspector() {}

	virtual void Destroy() = 0;

	static IJavascriptInspector* Create(int32 InPort, JsContextRef InContext);
};

// utility for holding value reference
template<typename T=JsValueRef>
class Persistent
{
public:
	Persistent() : value_(JS_INVALID_REFERENCE) {}
	Persistent(T Value) : value_(JS_INVALID_REFERENCE)
	{
		Reset(Value);
	}

	~Persistent()
	{
		Reset(JS_INVALID_REFERENCE);
	}

	Persistent(const Persistent& other) : value_(JS_INVALID_REFERENCE)
	{
		Reset(other.value_);
	}

	T Get() const
	{
		return value_;
	}

	bool IsEmpty() const
	{
		return value_ == JS_INVALID_REFERENCE;
	}

	void Reset(T newValue = JS_INVALID_REFERENCE)
	{
		T oldValue = value_;
		value_ = newValue;

		if (value_ != JS_INVALID_REFERENCE)
			JsAddRef(value_, nullptr);

		if (oldValue != JS_INVALID_REFERENCE)
			JsRelease(oldValue, nullptr);
	}

private:
	T value_;
};

class FContextScope
{
	JsContextRef prevContext = JS_INVALID_REFERENCE;
	JsContextRef currContext = JS_INVALID_REFERENCE;

public:
	FContextScope(JsContextRef context)
	{
		JsErrorCode getErr = JsGetCurrentContext(&prevContext);
		check(getErr == JsNoError);

		JsErrorCode setErr = JsSetCurrentContext(context);
		check(setErr == JsNoError);

		currContext = context;
	}

	~FContextScope()
	{
		JsContextRef nowContext = JS_INVALID_REFERENCE;
		check(JsGetCurrentContext(&nowContext) == JsNoError && nowContext == currContext);

		// recover context state
		JsSetCurrentContext(prevContext);
	}
};
