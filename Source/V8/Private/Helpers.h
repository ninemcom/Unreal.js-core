#pragma once

#include "Translator.h"

using namespace chakra;

namespace chakra
{
	JsValueRef Undefined()
	{
		JsValueRef undefined = JS_INVALID_REFERENCE;
		JsErrorCode err = JsGetUndefinedValue(&undefined);
		check(err == JsNoError);

		return undefined;
	}

	JsValueRef Null()
	{
		JsValueRef null = JS_INVALID_REFERENCE;
		JsErrorCode err = JsGetNullValue(&null);
		check(err == JsNoError);

		return null;
	}

	JsValueRef Double(double Value)
	{
		JsValueRef dbl = JS_INVALID_REFERENCE;
		JsErrorCode err = JsDoubleToNumber(Value, &dbl);
		check(err == JsNoError);

		return dbl;
	}

	JsValueRef Int(int Value)
	{
		JsValueRef integer = JS_INVALID_REFERENCE;
		JsErrorCode err = JsIntToNumber(Value, &integer);
		check(err == JsNoError);

		return integer;
	}

	JsValueRef Boolean(bool Value)
	{
		JsValueRef boolean = JS_INVALID_REFERENCE;
		JsErrorCode err = JsBoolToBoolean(Value, &boolean);
		check(err == JsNoError);

		return boolean;
	}

	FORCEINLINE JsValueRef Keyword(const FString& String)
	{
		return Chakra_KeywordString(String);
	}

	FORCEINLINE JsValueRef Keyword(const char* String)
	{
		return Chakra_KeywordString(String);
	}

	FORCEINLINE JsValueRef String(const FString& InString)
	{
		return Chakra_String(InString);
	}

	FORCEINLINE JsValueRef String(const char* InString)
	{
		return Chakra_String(InString);
	}

	void Throw(const FString& InString)
	{
		JsSetException(Chakra_String(InString));
	}

	FORCEINLINE JsValueRef External(void* Data)
	{
		JsValueRef externalObj = JS_INVALID_REFERENCE;
		JsErrorCode err = JsCreateExternalObject(Data, nullptr, &externalObj);
		return externalObj;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate()
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsErrorCode err = JsCreateFunction([](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			// do nothing
			return Undefined();
		}, nullptr, &Function);
		check(err == JsNoError);

		return Function;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate(JsNativeFunction Callback)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsErrorCode err = JsCreateFunction(Callback, nullptr, &Function);
		check(err == JsNoError);

		return Function;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate(JsNativeFunction Callback, void* Data)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsErrorCode err = JsCreateFunction(Callback, Data, &Function);
		check(err == JsNoError);

		return Function;
	}

	JsValueRef New(JsFunctionRef Template, JsValueRef* Argv = nullptr, int Argc = 0)
	{
		JsValueRef instance = JS_INVALID_REFERENCE;
		JsConstructObject(Template, Argv, Argc, &instance);

		return instance;
	}

	void Inherit(JsFunctionRef Template, JsFunctionRef ParentTemplate)
	{
		// simple inheritance using prototype
		// function parent() {}
		// function child() {}
		// child.prototype = Object.create(parent.prototype);

		// NOTE:
		// Object.create = function (o) {
		//     function F() {}
		//     F.prototype = o;
		//     return new F();
		// };

		JsFunctionRef prototypeClass = FunctionTemplate(nullptr);
		JsSetPrototype(prototypeClass, ParentTemplate);
		JsSetPrototype(Template, New(prototypeClass));
	}

	bool IsObject(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsObject;
	}

	bool IsArrayBuffer(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsArrayBuffer;
	}

	bool IsArray(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsArray;
	}

	bool IsNumber(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsNumber;
	}

	bool IsString(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsString;
	}

	bool IsEmpty(JsValueRef Value)
	{
		return Value == JS_INVALID_REFERENCE;
	}

	bool IsExternal(JsValueRef Value)
	{
		bool hasExternal = false;
		return JsHasExternalData(Value, &hasExternal) == JsNoError && hasExternal;
	}

	bool IsFunction(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsFunction;
	}

	bool IsNull(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsNull;
	}

	bool IsUndefined(JsValueRef Value)
	{
		JsValueType Type = JsUndefined;
		return JsGetValueType(Value, &Type) == JsNoError && Type == JsUndefined;
	}

	int IntFrom(JsValueRef Value)
	{
		int iValue = 0;
		JsNumberToInt(Value, &iValue);
		return iValue;
	}

	double DoubleFrom(JsValueRef Value)
	{
		double dValue = 0;
		JsNumberToDouble(Value, &dValue);
		return dValue;
	}

	bool BoolFrom(JsValueRef Value)
	{
		bool bValue = false;
		JsBooleanToBool(Value, &bValue);
		return bValue;
	}

	bool BoolEvaluate(JsValueRef Value)
	{
		bool bValue = false;
		JsConvertValueToBoolean(Value, &bValue);
		return bValue;
	}

	JsValueRef GetProperty(JsValueRef Value, const FString& Name)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		JsValueRef Property = JS_INVALID_REFERENCE;
		JsGetPropertyIdFromName(*Name, &PropID);
		JsGetProperty(Value, PropID, &Property);

		return Property;
	}

	bool SetProperty(JsValueRef Object, const FString Name, JsValueRef Prop)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propIDStr(*Name);
		JsErrorCode err = JsCreatePropertyId(propIDStr.Get(), propIDStr.Length(), &PropID);
		if (err != JsNoError)
			return false;

		err = JsSetProperty(Object, PropID, Prop, true);
		return err == JsNoError;
	}

	JsValueRef GetPrototype(JsValueRef Value)
	{
		JsValueRef prototype = JS_INVALID_REFERENCE;
		JsGetPrototype(Value, &prototype);

		return prototype;
	}

	JsValueRef GetIndex(JsValueRef MaybeArray, int index)
	{
		if (!IsArray(MaybeArray))
			return JS_INVALID_REFERENCE;

		JsValueRef elem = JS_INVALID_REFERENCE;
		JsGetIndexedProperty(MaybeArray, Int(index), &elem);

		return elem;
	}

	bool SetIndex(JsValueRef MaybeArray, int Index, JsValueRef Value)
	{
		if (!IsArray(MaybeArray))
			return false;

		JsSetIndexedProperty(MaybeArray, Int(Index), Value);
		return true;
	}

	int Length(JsValueRef MaybeArray)
	{
		if (!IsArray(MaybeArray))
			return 0;

		int length = 0;
		JsNumberToInt(GetProperty(MaybeArray, TEXT("length")), &length);

		return length;
	}

	struct FPropertyDescriptor
	{
		JsFunctionRef Getter = JS_INVALID_REFERENCE;
		JsFunctionRef Setter = JS_INVALID_REFERENCE;
		bool Configurable = false;
		bool Enumerable = false;
		bool Writable = false;
	};

	bool SetAccessor(JsFunctionRef Template, FString PropertyName, FPropertyDescriptor PropertyDescriptor)
	{
		JsValueRef AccessorDesc = JS_INVALID_REFERENCE;
		JsCreateObject(&AccessorDesc);
		if (!IsEmpty(PropertyDescriptor.Getter))
			SetProperty(AccessorDesc, "get", PropertyDescriptor.Getter);
		if (!IsEmpty(PropertyDescriptor.Setter) && PropertyDescriptor.Writable)
			SetProperty(AccessorDesc, "set", PropertyDescriptor.Setter);

		SetProperty(AccessorDesc, "configurable", Boolean(PropertyDescriptor.Configurable));
		SetProperty(AccessorDesc, "enumerable", Boolean(PropertyDescriptor.Enumerable));
		SetProperty(AccessorDesc, "writable", Boolean(PropertyDescriptor.Writable));

		JsValueRef Prototype = GetProperty(Template, "prototype");
		JsPropertyIdRef accessibleProp = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propertyID(*PropertyName);
		JsCreatePropertyId(propertyID.Get(), propertyID.Length(), &accessibleProp);

		bool ret = false;
		return JsDefineProperty(Prototype, accessibleProp, AccessorDesc, &ret) == JsNoError && ret;
	}

	JsContextRef CurrentContext()
	{
		JsContextRef ctx = JS_INVALID_REFERENCE;
		JsGetCurrentContext(&ctx);
		return ctx;
	}
}
