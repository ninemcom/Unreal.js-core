#include "Translator.h"

namespace chakra
{
	UObject* UObjectFromChakra(JsValueRef Value)
	{
		uint8* Memory = RawMemoryFromChakra(Value);
		if (Memory)
		{
			auto uobj = reinterpret_cast<UObject*>(Memory);
			if (uobj->IsValidLowLevelFast() && !uobj->IsPendingKill())
			{
				return uobj;
			}
		}	

		return nullptr;
	}

	uint8* RawMemoryFromChakra(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return nullptr;

		JsValueType type;
		JsErrorCode err = JsGetValueType(Value, &type);
		check(err == JsNoError);

		if (type != JsObject)
			return nullptr;

		JsPropertyIdRef selfProp;
		err = JsCreatePropertyId("__self", 6, &selfProp);
		check(err == JsNoError);

		JsValueRef self = JS_INVALID_REFERENCE;
		err = JsGetProperty(Value, selfProp, &self);
		check(err == JsNoError);

		bool hasExternalData;
		err = JsHasExternalData(self, &hasExternalData);
		check(err == JsNoError);
		if (!hasExternalData)
			return nullptr;

		void* dataPtr = nullptr;
		JsGetExternalData(self, &dataPtr);

		return reinterpret_cast<uint8*>(dataPtr);		
	}

	UClass* UClassFromChakra(JsValueRef Value)
	{
		if (Value == JS_INVALID_REFERENCE)
			return nullptr;

		JsValueType type;
		if (JsGetValueType(Value, &type) != JsNoError)
			return nullptr;

		if (type == JsUndefined || type == JsNull)
			return nullptr;

		if (type == JsFunction)
		{
			JsPropertyIdRef staticClass = JS_INVALID_REFERENCE;
			JsCreatePropertyId("StaticClass", 11, &staticClass);
			JsGetProperty(Value, staticClass, &Value);
		}

		bool isExternal = false;
		if (Value != JS_INVALID_REFERENCE && JsHasExternalData(Value, &isExternal) == JsNoError && isExternal)
		{
			UClass* Class = nullptr;
			JsGetExternalData(Value, reinterpret_cast<void**>(&Class));

			return Class;
		}

		return nullptr;
	}

	FString StringFromChakra(JsValueRef Value)
	{
		const int STACK_BUFFER_SIZE = 0x10000;
		char Buffer[STACK_BUFFER_SIZE];
		const TCHAR* StringPtr = nullptr;
		size_t Strlen = 0;
		JsErrorCode err = JsCopyString(Value, nullptr, 0, &Strlen);
		if (err != JsNoError)
			return FString();

		char* buffer = Buffer;
		bool useExtraBuffer = Strlen >= STACK_BUFFER_SIZE;
		if (useExtraBuffer)
		{
			buffer = reinterpret_cast<char*>(FMemory_Alloca(Strlen+1));
		}

		err = JsCopyString(Value, buffer, Strlen, &Strlen);
		buffer[Strlen] = '\0';
		if (err != JsNoError)
		{
			check(false);
			if (useExtraBuffer)
				delete buffer;

			return FString();
		}

		FString ret = UTF8_TO_TCHAR(buffer);
		if (useExtraBuffer)
			delete buffer;

		return ret;
	}

	FString StringFromArgs(const JsValueRef* args, unsigned short nargs, int StartIndex)
	{
		TArray<FString> ArgStrings;

		for (unsigned int Index = StartIndex; Index < nargs; Index++)
		{
			JsValueRef stringified = JS_INVALID_REFERENCE;
			JsConvertValueToString(args[Index], &stringified);

			ArgStrings.Add(StringFromChakra(stringified));
		}

		return FString::Join(ArgStrings, TEXT(" "));
	}
}