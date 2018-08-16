#pragma once

#include "Translator.h"

namespace chakra
{
	static JsValueRef Undefined()
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Undefined"));
		JsValueRef undefined = JS_INVALID_REFERENCE;
		JsCheck(JsGetUndefinedValue(&undefined));

		return undefined;
	}

	static JsValueRef Null()
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Null"));
		JsValueRef null = JS_INVALID_REFERENCE;
		JsCheck(JsGetNullValue(&null));

		return null;
	}

	static JsValueRef Double(double Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Double"));
		JsValueRef dbl = JS_INVALID_REFERENCE;
		JsCheck(JsDoubleToNumber(Value, &dbl));

		return dbl;
	}

	static JsValueRef Int(int Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Int"));
		JsValueRef integer = JS_INVALID_REFERENCE;
		JsCheck(JsIntToNumber(Value, &integer));

		return integer;
	}

	static JsValueRef Boolean(bool Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Boolean"));
		JsValueRef boolean = JS_INVALID_REFERENCE;
		JsCheck(JsBoolToBoolean(Value, &boolean));

		return boolean;
	}

	static JsValueRef String(const FString& String)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::String"));
		JsValueRef stringValue;
		FTCHARToUTF8 converter(*String);
		JsCheck(JsCreateString(converter.Get(), converter.Length(), &stringValue));

		return stringValue;
	}

	static JsValueRef String(const char* String)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::String(char)"));
		JsValueRef stringValue;
		JsCheck(JsCreateString(String, FCStringAnsi::Strlen(String), &stringValue));

		return stringValue;
	}

	static void Throw(const FString& InString)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Throw"));
		JsValueRef error = JS_INVALID_REFERENCE;
		JsCreateError(String(InString), &error);
		JsCheck(JsSetException(error));
	}

	static JsValueRef External(void* Data, JsFinalizeCallback OnDestroy)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::External"));
		JsValueRef externalObj = JS_INVALID_REFERENCE;
		JsCheck(JsCreateExternalObject(Data, OnDestroy, &externalObj));

		return externalObj;
	}

	static JsFunctionRef FunctionTemplate()
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::FunctionTemplate"));
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction([](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			// do nothing
			return Undefined();
		}, nullptr, &Function));

		return Function;
	}

	static JsFunctionRef FunctionTemplate(JsNativeFunction Callback)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::FunctionTemplate(callback)"));
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, nullptr, &Function));

		return Function;
	}

	static JsFunctionRef FunctionTemplate(JsNativeFunction Callback, void* Data)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::FunctionTemplate(data)"));
		JsFunctionRef Function = JS_INVALID_REFERENCE;
		JsCheck(JsCreateFunction(Callback, Data, &Function));

		return Function;
	}

	static JsValueRef New(JsFunctionRef Template, JsValueRef* Argv = nullptr, int Argc = 0)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::New"));
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

	static bool IsObject(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsObject"));
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsObject;
	}

	static bool IsArrayBuffer(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsArrayBuffer %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArrayBuffer;
	}

	static bool IsArray(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsArray %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsArray;
	}

	static bool IsNumber(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsNumber %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNumber;
	}

	static bool IsString(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsString %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsString;
	}

	static bool IsEmpty(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsEmpty %p"), Value);
		return Value == JS_INVALID_REFERENCE;
	}

	static bool IsExternal(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsExternal %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		bool hasExternal = false;
		JsCheck(JsHasExternalData(Value, &hasExternal));
		
		return hasExternal;
	}

	static bool IsFunction(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsFunction %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsFunction;
	}

	static bool IsNull(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsNull %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));

		return Type == JsNull;
	}

	static bool IsUndefined(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IsUndefined %p"), Value);
		if (Value == JS_INVALID_REFERENCE)
			return false;

		JsValueType Type = JsUndefined;
		JsCheck(JsGetValueType(Value, &Type));
		
		return Type == JsUndefined;
	}

	static int IntFrom(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::IntFrom %p"), Value);
		int iValue = 0;
		JsNumberToInt(Value, &iValue);
		return iValue;
	}

	static double DoubleFrom(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::DoubleFrom %p"), Value);
		double dValue = 0;
		JsNumberToDouble(Value, &dValue);
		return dValue;
	}

	static bool BoolFrom(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::BoolFrom %p"), Value);
		bool bValue = false;
		JsBooleanToBool(Value, &bValue);
		return bValue;
	}

	static bool BoolEvaluate(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::BoolEvaluate %p"), Value);
		JsValueRef boolConverted = JS_INVALID_REFERENCE;
		JsCheck(JsConvertValueToBoolean(Value, &boolConverted));

		return BoolFrom(boolConverted);
	}

	static JsPropertyIdRef PropertyID(const char* ID)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::PropertyID(char)"));
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		JsCheck(JsCreatePropertyId(ID, FCStringAnsi::Strlen(ID), &PropID));

		return PropID;
	}

	static JsPropertyIdRef PropertyID(const FString& ID)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::PropertyID '%s'"), *ID);
		JsPropertyIdRef PropID = JS_INVALID_REFERENCE;
		FTCHARToUTF8 propIDStr(*ID);
		JsCheck(JsCreatePropertyId(propIDStr.Get(), propIDStr.Length(), &PropID));

		return PropID;
	}

	static JsValueType GetType(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::GetType %p"), Value);
		JsValueType type = JsUndefined;
		JsCheck(JsGetValueType(Value, &type));

		return type;
	}

	static JsValueRef GetProperty(JsValueRef Value, const char* Name)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::GetProperty(char) %p"), Value);
		JsValueRef Property = JS_INVALID_REFERENCE;
		JsCheck(JsGetProperty(Value, PropertyID(Name), &Property));

		return Property;
	}

	static JsValueRef GetProperty(JsValueRef Value, const FString& Name)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::GetProperty %p"), Value);
		JsValueRef Property = JS_INVALID_REFERENCE;
		JsCheck(JsGetProperty(Value, PropertyID(Name), &Property));

		return Property;
	}

	static void SetProperty(JsValueRef Object, const char* Name, JsValueRef Prop)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::SetProperty(char) %p"), Object);
		JsCheck(JsSetProperty(Object, PropertyID(Name), Prop, true));
	}

	static void SetProperty(JsValueRef Object, const FString& Name, JsValueRef Prop)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::SetProperty %p"), Object);
		JsCheck(JsSetProperty(Object, PropertyID(Name), Prop, true));
	}

	static JsValueRef GetPrototype(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::GetPrototype %p"), Value);
		JsValueRef prototype = JS_INVALID_REFERENCE;
		JsCheck(JsGetPrototype(Value, &prototype));

		return prototype;
	}

	static JsValueRef GetIndex(JsValueRef MaybeArray, int index)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::GetIndex %p"), MaybeArray);
		JsValueRef elem = JS_INVALID_REFERENCE;
		JsCheck(JsGetIndexedProperty(MaybeArray, Int(index), &elem));

		return elem;
	}

	static void SetIndex(JsValueRef MaybeArray, int Index, JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::SetIndex %p"), MaybeArray);
		JsCheck(JsSetIndexedProperty(MaybeArray, Int(Index), Value));
	}

	static int Length(JsValueRef MaybeArray)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Length %p"), MaybeArray);
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
		UE_LOG(Javascript, Verbose, TEXT("chakra::SetAccessor %p"), Value);
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

		JsValueType type = GetType(Value);

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

	static bool HasProperty(JsValueRef Value, const FString& PropertyName)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::HasProperty %p"), Value);
		if (IsEmpty(Value))
			return false;

		bool ret = false;
		JsCheck(JsHasProperty(Value, PropertyID(PropertyName), &ret));

		return ret;
	}

	static TArray<FString> PropertyNames(JsValueRef Value)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::PropertyNames %p"), Value);
		TArray<FString> ret;
		JsValueRef properties = JS_INVALID_REFERENCE;
		JsCheck(JsGetOwnPropertyNames(Value, &properties));

		int len = Length(properties);
		for (int i = 0; i < len; i++)
		{
			JsValueRef Name = GetIndex(properties, i);
			ret.Add(chakra::StringFromChakra(Name));
		}

		return ret;
	}

	static void Inherit(JsFunctionRef Template, JsFunctionRef ParentTemplate)
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::Inherit %p"), Template);
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
		// child.prototype.__proto__ = parent

		JsCheck(JsSetPrototype(GetProperty(Template, "prototype"), GetProperty(ParentTemplate, "prototype")));
		//JsFunctionRef prototypeClass = FunctionTemplate();
		//SetProperty(prototypeClass, "prototype", ParentTemplate);

		//JsValueRef prototype = New(prototypeClass);
		//JsValueRef oldPrototype = GetProperty(Template, "prototype");
		//JsValueRef propKeys = JS_INVALID_REFERENCE;
		//JsCheck(JsGetOwnPropertyNames(oldPrototype, &propKeys));

		//int len = Length(propKeys);
		//for (int i = 0; i < len; i++)
		//{
		//	FString key = chakra::StringFromChakra(GetIndex(propKeys, i));
		//	SetProperty(prototype, key, GetProperty(oldPrototype, key));
		//}

		//SetProperty(Template, "prototype", New(prototypeClass));
	}

	static JsContextRef CurrentContext()
	{
		UE_LOG(Javascript, Verbose, TEXT("chakra::CurrentContext %p"));
		JsContextRef ctx = JS_INVALID_REFERENCE;
		JsCheck(JsGetCurrentContext(&ctx));

		return ctx;
	}
}
