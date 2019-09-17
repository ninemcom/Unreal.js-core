#include "Translator.h"
#include "Engine/UserDefinedStruct.h"

namespace v8
{
	UObject* UObjectFromV8(Local<Context> context, Local<Value> Value)
	{
		uint8* Memory = RawMemoryFromV8(context, Value);
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

	uint8* RawMemoryFromV8(Local<Context> context, Local<Value> Value)
	{
		if (Value.IsEmpty() || !Value->IsObject() || Value->IsUndefined() || Value->IsNull())
		{
			return nullptr;
		}

		auto v8_obj = Value->ToObject(context).ToLocalChecked();
		if (v8_obj->InternalFieldCount() == 0)
		{
			return nullptr;
		}
		return reinterpret_cast<uint8*>(v8_obj->GetAlignedPointerFromInternalField(0));		
	}

	UClass* UClassFromV8(Isolate* isolate_, Local<Value> Value)
	{
		if (Value.IsEmpty() || !Value->IsObject())
		{
			return nullptr;
		}

		auto maybe_v8_obj = Value->ToObject(isolate_->GetCurrentContext());
		if (maybe_v8_obj.IsEmpty())
		{
			return nullptr;
		}

		auto v8_obj = maybe_v8_obj.ToLocalChecked();

		if (v8_obj->IsFunction())
		{
			auto maybe_vv = v8_obj->Get(isolate_->GetCurrentContext(), V8_KeywordString(isolate_, "StaticClass"));
			if (!maybe_vv.IsEmpty())
			{
				v8_obj = maybe_vv.ToLocalChecked()->ToObject(isolate_->GetCurrentContext()).ToLocalChecked();
			}
		}

		if (v8_obj->IsExternal())
		{
			auto v8_class = Local<External>::Cast(v8_obj);

			UClass* Class{ nullptr };
			if (!v8_class.IsEmpty())
			{
				Class = reinterpret_cast<UClass*>(v8_class->Value());
			}

			return Class;
		}

		return nullptr;
	}

	Local<String> V8_String(Isolate* isolate, const FString& String)
	{
#if V8_MINOR_VERSION >= 6
		return String::NewFromUtf8(isolate, TCHAR_TO_UTF8(*String)).ToLocalChecked();
#else
		return String::NewFromUtf8(isolate, TCHAR_TO_UTF8(*String));
#endif
	}

	Local<String> V8_String(Isolate* isolate, const char* String)
	{
#if V8_MINOR_VERSION >= 6
		return String::NewFromUtf8(isolate, String).ToLocalChecked();
#else
		return String::NewFromUtf8(isolate, String);
#endif
	}

	Local<String> V8_KeywordString(Isolate* isolate, const FString& String)
	{
#if V8_MINOR_VERSION >= 6
		return String::NewFromUtf8(isolate, TCHAR_TO_UTF8(*String), NewStringType::kInternalized).ToLocalChecked();
#else
		return String::NewFromUtf8(isolate, TCHAR_TO_UTF8(*String), String::kInternalizedString);
#endif
	}

	Local<String> V8_KeywordString(Isolate* isolate, const char* String)
	{
#if V8_MINOR_VERSION >= 6
		return String::NewFromUtf8(isolate, String, NewStringType::kInternalized).ToLocalChecked();
#else
		return String::NewFromUtf8(isolate, String, String::kInternalizedString);
#endif
	}

	FString StringFromV8(Isolate* isolate, Local<Value> Value)
	{
		return UTF8_TO_TCHAR(*String::Utf8Value(isolate, Value));
	}

	FString StringFromArgs(const FunctionCallbackInfo<v8::Value>& args, int StartIndex)
	{
		auto isolate = args.GetIsolate();
		HandleScope handle_scope(isolate);

		TArray<FString> ArgStrings;

		for (int Index = StartIndex; Index < args.Length(); Index++)
		{
			ArgStrings.Add(StringFromV8(isolate, args[Index]));
		}

		return FString::Join(ArgStrings, TEXT(" "));
	}
}
