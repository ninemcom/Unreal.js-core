#include "Translator.h"
#include "Helpers.h"

namespace chakra
{
	void Check(JsErrorCode error)
	{
#if DO_CHECK
		if (error == JsNoError)
			return;

		if (error == JsErrorScriptException || error == JsErrorScriptCompile)
		{
			JsValueRef exception = JS_INVALID_REFERENCE;
			JsCheck(JsGetAndClearException(&exception));
			JsCheck(JsConvertValueToString(exception, &exception));

			char buffer[0x1000]; size_t strlen = 0;
			JsCheck(JsCopyString(exception, buffer, sizeof(buffer), &strlen));
			buffer[strlen] = '\0';

			FString errStr = UTF8_TO_TCHAR(buffer);
			checkf(false, *errStr);
			return;
		}

		check(false);
#endif
	}


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
		if (chakra::IsEmpty(Value) || !chakra::IsObject(Value))
			return nullptr;

		void* dataPtr = nullptr;
		JsCheck(JsGetExternalData(chakra::GetProperty(Value, "__self"), &dataPtr));

		return reinterpret_cast<uint8*>(dataPtr);		
	}

	UClass* UClassFromChakra(JsValueRef Value)
	{
		if (chakra::IsEmpty(Value))
			return nullptr;

		JsValueType type;
		JsCheck(JsGetValueType(Value, &type));

		if (type == JsFunction)
		{
			JsPropertyIdRef staticClass = JS_INVALID_REFERENCE;
			JsCheck(JsCreatePropertyId("StaticClass", 11, &staticClass));
			JsCheck(JsGetProperty(Value, staticClass, &Value));
		}

		bool isExternal = false;
		if (chakra::IsEmpty(Value) || !chakra::IsExternal(Value))
			return nullptr;

		UClass* Class = nullptr;
		JsCheck(JsGetExternalData(Value, reinterpret_cast<void**>(&Class)));

		return Class;
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
			buffer = reinterpret_cast<char*>(FMemory::Malloc(Strlen+1));
		}

		err = JsCopyString(Value, buffer, Strlen, &Strlen);
		buffer[Strlen] = '\0';
		if (err != JsNoError)
		{
			check(false);
			if (useExtraBuffer)
				FMemory::Free(buffer);

			return FString();
		}

		FString ret = UTF8_TO_TCHAR(buffer);
		if (useExtraBuffer)
			FMemory::Free(buffer);

		return ret;
	}

	FString StringFromArgs(const JsValueRef* args, unsigned short nargs, int StartIndex)
	{
		TArray<FString> ArgStrings;

		for (unsigned int Index = StartIndex; Index < nargs; Index++)
		{
			JsValueRef stringified = JS_INVALID_REFERENCE;
			JsCheck(JsConvertValueToString(args[Index], &stringified));

			ArgStrings.Add(StringFromChakra(stringified));
		}

		return FString::Join(ArgStrings, TEXT(" "));
	}
}