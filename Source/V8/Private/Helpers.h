#pragma once

#include "Translator.h"

namespace chakra
{
	static JsValueRef Undefined()
	{
		JsValueRef undefined = JS_INVALID_REFERENCE;
		JsCheck(JsGetUndefinedValue(&undefined));

		return undefined;
	}

	static JsValueRef Null()
	{
		JsValueRef null = JS_INVALID_REFERENCE;
		JsCheck(JsGetNullValue(&null));

		return null;
	}

	static JsValueRef Double(double Value)
	{
		JsValueRef dbl = JS_INVALID_REFERENCE;
		JsCheck(JsDoubleToNumber(Value, &dbl));

		return dbl;
	}

	static JsValueRef Int(int Value)
	{
		JsValueRef integer = JS_INVALID_REFERENCE;
		JsCheck(JsIntToNumber(Value, &integer));

		return integer;
	}

	static JsValueRef Boolean(bool Value)
	{
		JsValueRef boolean = JS_INVALID_REFERENCE;
		JsCheck(JsBoolToBoolean(Value, &boolean));

		return boolean;
	}

	static JsValueRef String(const FString& String)
	{
		JsValueRef stringValue;
		FTCHARToUTF8 converter(*String);
		JsCheck(JsCreateString(converter.Get(), converter.Length(), &stringValue));

		return stringValue;
	}

	static JsValueRef String(const char* String)
	{
		JsValueRef stringValue;
		JsCheck(JsCreateString(String, FCStringAnsi::Strlen(String), &stringValue));

		return stringValue;
	}

	static void Throw(const FString& InString)
	{
		JsValueRef stringValue = String(InString);
		JsCheck(JsSetException(stringValue));
	}

	static JsValueRef External(void* Data, JsFinalizeCallback OnDestroy)
	{
		JsValueRef externalObj = JS_INVALID_REFERENCE;
		JsCheck(JsCreateExternalObject(Data, OnDestroy, &externalObj));

		return externalObj;
	}

	static JsFunctionRef FunctionTemplate()
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction([](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			// do nothing
			return Undefined();
		}, nullptr, &Function));

		return Function;
	}

	static JsFunctionRef FunctionTemplate(JsNativeFunction Callback)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, nullptr, &Function));

		return Function;
	}

	static JsFunctionRef FunctionTemplate(JsNativeFunction Callback, void* Data)
	{
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, Data, &Function));

		return Function;
	}

	static JsValueRef New(JsFunctionRef Template, JsValueRef* Argv = nullptr, int Argc = 0)
	{
		JsValueRef instance = JS_INVALID_REFERENCE;
		if (Argv == nullptr)
		{
			JsValueRef args[] = { Undefined() };
			JsCheck(JsConstructObject(Template, args, 1, &instance));
		}
		else
		{
			JsCheck(JsConstructObject(Template, Argv, Argc, &instance));
		}

		return instance;
	}

	static void Inherit(JsFunctionRef Template, JsFunctionRef ParentTemplate)
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

	static bool IsObject(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsObject;
	}

	static bool IsArrayBuffer(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArrayBuffer;
	}

	static bool IsArray(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArray;
	}

	static bool IsNumber(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNumber;
	}

	static bool IsString(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsString;
	}

	static bool IsEmpty(JsValueRef Value)
	{
		return Value == JS_INVALID_REFERENCE;
	}

	static bool IsExternal(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		bool hasExternal = false;
		JsCheck(JsHasExternalData(Value, &hasExternal));
		
		return hasExternal;
	}

	static bool IsFunction(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsFunction;
	}

	static bool IsNull(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNull;
	}

	static bool IsUndefined(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));
		
		return Type == JsUndefined;
	}

	static int IntFrom(JsValueRef Value)
	{
		int iValue = 0;
		JsNumberToInt(Value, &iValue);
		return iValue;
	}

	static double DoubleFrom(JsValueRef Value)
	{
		double dValue = 0;
		JsNumberToDouble(Value, &dValue);
		return dValue;
	}

	static bool BoolFrom(JsValueRef Value)
	{
		bool bValue = false;
		JsBooleanToBool(Value, &bValue);
		return bValue;
	}

	static bool BoolEvaluate(JsValueRef Value)
	{
		JsValueRef boolConverted = JS_INVALID_REFERENCE;
		JsCheck(JsConvertValueToBoolean(Value, &boolConverted));

		return BoolFrom(boolConverted);
	}

	static JsPropertyIdRef PropertyID(const char* ID)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		JsCheck(JsCreatePropertyId(ID, FCStringAnsi::Strlen(ID), &PropID));

		return PropID;
	}

	static JsPropertyIdRef PropertyID(const FString& ID)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propIDStr(*ID);
		JsCheck(JsCreatePropertyId(propIDStr.Get(), propIDStr.Length(), &PropID));

		return PropID;
	}

	static JsValueRef GetProperty(JsValueRef Value, const FString& Name)
	{
		JsValueRef Property = JS_INVALID_REFERENCE;
		JsCheck(JsGetProperty(Value, PropertyID(Name), &Property));

		return Property;
	}

	static void SetProperty(JsValueRef Object, const FString& Name, JsValueRef Prop)
	{
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propIDStr(*Name);
		JsCheck(JsCreatePropertyId(propIDStr.Get(), propIDStr.Length(), &PropID));
		JsCheck(JsSetProperty(Object, PropID, Prop, true));
	}

	static JsValueRef GetPrototype(JsValueRef Value)
	{
		JsValueRef prototype = JS_INVALID_REFERENCE;
		JsCheck(JsGetPrototype(Value, &prototype));

		return prototype;
	}

	static JsValueRef GetIndex(JsValueRef MaybeArray, int index)
	{
		JsValueRef elem = JS_INVALID_REFERENCE;
		JsCheck(JsGetIndexedProperty(MaybeArray, Int(index), &elem));

		return elem;
	}

	static void SetIndex(JsValueRef MaybeArray, int Index, JsValueRef Value)
	{
		JsCheck(JsSetIndexedProperty(MaybeArray, Int(Index), Value));
	}

	static int Length(JsValueRef MaybeArray)
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

	static void SetAccessor(JsValueRef Value, FString PropertyName, FPropertyDescriptor PropertyDescriptor)
	{
		JsValueRef AccessorDesc = JS_INVALID_REFERENCE;
		JsCheck(JsCreateObject(&AccessorDesc));
		if (!IsEmpty(PropertyDescriptor.Getter))
			SetProperty(AccessorDesc, "get", PropertyDescriptor.Getter);
		if (!IsEmpty(PropertyDescriptor.Setter))
			SetProperty(AccessorDesc, "set", PropertyDescriptor.Setter);
		if (PropertyDescriptor.Configurable)
			SetProperty(AccessorDesc, "configurable", Boolean(true));
		if (PropertyDescriptor.Enumerable)
			SetProperty(AccessorDesc, "enumerable", Boolean(true));
		if (PropertyDescriptor.Writable && IsEmpty(PropertyDescriptor.Setter))
			SetProperty(AccessorDesc, "writable", Boolean(true));

		JsValueType type;
		JsCheck(JsGetValueType(Value, &type));

		if (type == JsFunction)
		{
			// set to prototype
			JsValueRef Prototype = GetProperty(Value, "prototype");
			check(Prototype != JS_INVALID_REFERENCE);

			bool ret = false;
			JsCheck(JsDefineProperty(Prototype, PropertyID(PropertyName), AccessorDesc, &ret));
		}
		else
		{
			bool ret = false;
			JsCheck(JsDefineProperty(Value, PropertyID(PropertyName), AccessorDesc, &ret));
		}
	}

	static JsContextRef CurrentContext()
	{
		JsContextRef ctx = JS_INVALID_REFERENCE;
		JsCheck(JsGetCurrentContext(&ctx));

		return ctx;
	}
}
