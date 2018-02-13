#pragma once

#include "Translator.h"

#define JsCheck(expression) \
do { \
	JsErrorCode err = expression; \
	check(err == JsNoError); \
} while(false)

namespace chakra
{
	JsValueRef Undefined()
	{
		JsValueRef undefined = JS_INVALID_REFERENCE;
		JsCheck(JsGetUndefinedValue(&undefined));

		return undefined;
	}

	JsValueRef Null()
	{
		JsValueRef null = JS_INVALID_REFERENCE;
		JsCheck(JsGetNullValue(&null));

		return null;
	}

	JsValueRef Double(double Value)
	{
		JsValueRef dbl = JS_INVALID_REFERENCE;
		JsCheck(JsDoubleToNumber(Value, &dbl));

		return dbl;
	}

	JsValueRef Int(int Value)
	{
		JsValueRef integer = JS_INVALID_REFERENCE;
		JsCheck(JsIntToNumber(Value, &integer));

		return integer;
	}

	JsValueRef Boolean(bool Value)
	{
		JsValueRef boolean = JS_INVALID_REFERENCE;
		JsCheck(JsBoolToBoolean(Value, &boolean));

		return boolean;
	}

	FORCEINLINE JsValueRef String(const FString& String)
	{
		JsValueRef stringValue;
		JsCheck(JsCreateString(TCHAR_TO_UTF8(*String), String.Len(), &stringValue));

		return stringValue;
	}

	FORCEINLINE JsValueRef String(const char* String)
	{
		JsValueRef stringValue;
		JsCheck(JsCreateString(String, FCStringAnsi::Strlen(String), &stringValue));

		return stringValue;
	}

	void Throw(const FString& InString)
	{
		JsValueRef stringValue = String(InString);
		JsCheck(JsSetException(stringValue));
	}

	FORCEINLINE JsValueRef External(void* Data)
	{
		JsValueRef externalObj = JS_INVALID_REFERENCE;
		JsCheck(JsCreateExternalObject(Data, nullptr, &externalObj));

		return externalObj;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate()
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction([](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			// do nothing
			return Undefined();
		}, nullptr, &Function));

		return Function;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate(JsNativeFunction Callback)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, nullptr, &Function));

		return Function;
	}

	FORCEINLINE JsFunctionRef FunctionTemplate(JsNativeFunction Callback, void* Data)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, Data, &Function));

		return Function;
	}

	JsValueRef New(JsFunctionRef Template, JsValueRef* Argv = nullptr, int Argc = 0)
	{
		JsValueRef instance = JS_INVALID_REFERENCE;
		JsCheck(JsConstructObject(Template, Argv, Argc, &instance));

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

		JsFunctionRef prototypeClass = FunctionTemplate();
		JsCheck(JsSetPrototype(prototypeClass, ParentTemplate));
		JsCheck(JsSetPrototype(Template, New(prototypeClass)));
	}

	bool IsObject(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsObject;
	}

	bool IsArrayBuffer(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArrayBuffer;
	}

	bool IsArray(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArray;
	}

	bool IsNumber(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNumber;
	}

	bool IsString(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsString;
	}

	bool IsEmpty(JsValueRef Value)
	{
		return Value == JS_INVALID_REFERENCE;
	}

	bool IsExternal(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		bool hasExternal = false;
		JsCheck(JsHasExternalData(Value, &hasExternal));
		
		return hasExternal;
	}

	bool IsFunction(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsFunction;
	}

	bool IsNull(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNull;
	}

	bool IsUndefined(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));
		
		return Type == JsUndefined;
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
		JsValueRef boolConverted = JS_INVALID_REFERENCE;
		JsCheck(JsConvertValueToBoolean(Value, &boolConverted));

		return BoolFrom(boolConverted);
	}

	JsValueRef GetProperty(JsValueRef Value, const FString& Name)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		JsValueRef Property = JS_INVALID_REFERENCE;
		JsErrorCode err = JsGetPropertyIdFromName(*Name, &PropID);
		if (err != JsNoError)
			return JS_INVALID_REFERENCE;

		JsCheck(JsGetProperty(Value, PropID, &Property));

		return Property;
	}

	void SetProperty(JsValueRef Object, const FString Name, JsValueRef Prop)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propIDStr(*Name);
		JsCheck(JsCreatePropertyId(propIDStr.Get(), propIDStr.Length(), &PropID));
		JsCheck(JsSetProperty(Object, PropID, Prop, true));
	}

	JsValueRef GetPrototype(JsValueRef Value)
	{
		JsValueRef prototype = JS_INVALID_REFERENCE;
		JsCheck(JsGetPrototype(Value, &prototype));

		return prototype;
	}

	JsValueRef GetIndex(JsValueRef MaybeArray, int index)
	{
		JsValueRef elem = JS_INVALID_REFERENCE;
		JsCheck(JsGetIndexedProperty(MaybeArray, Int(index), &elem));

		return elem;
	}

	void SetIndex(JsValueRef MaybeArray, int Index, JsValueRef Value)
	{
		JsCheck(JsSetIndexedProperty(MaybeArray, Int(Index), Value));
	}

	int Length(JsValueRef MaybeArray)
	{
		if (!IsArray(MaybeArray))
			return 0;

		int length = 0;
		JsCheck(JsNumberToInt(GetProperty(MaybeArray, TEXT("length")), &length));

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

	void SetAccessor(JsFunctionRef Template, FString PropertyName, FPropertyDescriptor PropertyDescriptor)
	{
		JsValueRef AccessorDesc = JS_INVALID_REFERENCE;
		JsCheck(JsCreateObject(&AccessorDesc));
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
		JsCheck(JsCreatePropertyId(propertyID.Get(), propertyID.Length(), &accessibleProp));

		bool ret = false;
		JsCheck(JsDefineProperty(Prototype, accessibleProp, AccessorDesc, &ret));
	}

	JsContextRef CurrentContext()
	{
		JsContextRef ctx = JS_INVALID_REFERENCE;
		JsCheck(JsGetCurrentContext(&ctx));

		return ctx;
	}
}
