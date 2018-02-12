#pragma once

#include "V8PCH.h"
#include "Translator.h"

using namespace chakra;

struct FV8Exception
{
	static FString Report(JsValueRef Exception)
	{
		JsValueRef ExceptionString = JS_INVALID_REFERENCE;
		JsErrorCode convertErr = JsConvertValueToString(Exception, &ExceptionString);
		check(convertErr == JsNoError);

		FString exception = StringFromChakra(ExceptionString);
		UE_LOG(Javascript, Error, TEXT("%s"), *exception);

		return exception;

		//if (!exception.IsEmpty())
		//{
		//	auto filename = StringFromV8(message->GetScriptResourceName());
		//	auto linenum = message->GetLineNumber();
		//	auto line = StringFromV8(message->GetSourceLine());

		//	UE_LOG(Javascript, Error, TEXT("%s:%d: %s"), *filename, linenum, *exception);

		//	auto stack_trace = StringFromV8(try_catch.StackTrace());
		//	if (stack_trace.Len() > 0)
		//	{
		//		TArray<FString> Lines;
		//		stack_trace.ParseIntoArrayLines(Lines);
		//		for (const auto& line : Lines)
		//		{
		//			UE_LOG(Javascript, Error, TEXT("%s"), *line);
		//		}

		//		return stack_trace;
		//	} 
		//	else
		//	{
		//		return *exception;
		//	}
		//}

		return FString();
	}
};