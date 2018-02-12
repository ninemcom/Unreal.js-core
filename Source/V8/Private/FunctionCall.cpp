#include "V8PCH.h"
#include "JavascriptIsolate_Private.h"
#include "JavascriptContext_Private.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#include "Translator.h"
#include "Exception.h"
#include "Helpers.h"
#include "JavascriptStats.h"

namespace chakra
{
	void CallJavascriptFunction(JsContextRef context, JsValueRef This, UFunction* SignatureFunction, JsFunctionRef func, void* Parms)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptFunctionCallToJavascript);

		auto Buffer = reinterpret_cast<uint8*>(Parms);		

		enum { MaxArgs = 32 };

		JsValueRef argv[MaxArgs] = { This }; // first argument should be thisArg
		int argc = 1;

		TFieldIterator<UProperty> Iter(SignatureFunction);
		for (; Iter && argc < MaxArgs && (Iter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++Iter)
		{
			UProperty* Param = *Iter;
			argv[argc++] = ReadProperty(Param, Buffer, FNoPropertyOwner());
		}

		UProperty* ReturnParam = nullptr;
		for (; Iter; ++Iter)
		{
			UProperty* Param = *Iter;
			if (Param->GetPropertyFlags() & CPF_ReturnParm)
			{
				ReturnParam = Param;
				break;
			}
		}

		JsValueRef value = JS_INVALID_REFERENCE;
		JsCallFunction(func, argv, argc, &value);

		bool hasException = false;
		JsHasException(&hasException);

		if (hasException)
		{
			JsValueRef Exception = JS_INVALID_REFERENCE;
			JsGetAndClearException(&Exception);

			FJavascriptContext::FromChakra(context)->UncaughtException(FV8Exception::Report(Exception));
		}

		bool bHasAnyOutParams = false;
		if (SignatureFunction && SignatureFunction->HasAnyFunctionFlags(FUNC_HasOutParms))
		{
			// Iterate over input parameters
			for (TFieldIterator<UProperty> It(SignatureFunction); It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
			{
				// This is 'out ref'!
				if ((It->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					bHasAnyOutParams = true;
					break;
				}
			}
		}

		if (bHasAnyOutParams)
		{
			FContextScope scope(context);

			JsValueType valueType = JsValueType::JsUndefined;
			JsGetValueType(value, &valueType);
			if (valueType != JsValueType::JsObject)
			{
				chakra::Throw(TEXT("..."));
				return;
			}

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(SignatureFunction); It; ++It)
			{
				UProperty* Param = *It;

				auto PropertyFlags = Param->GetPropertyFlags();					

				// pass return parameter as '$'
				if (PropertyFlags & CPF_ReturnParm)
				{
					JsPropertyIdRef SubValueProp = JS_INVALID_REFERENCE;
					JsValueRef SubValue = JS_INVALID_REFERENCE;
					JsGetPropertyIdFromName(TEXT("$"), &SubValueProp);
					JsGetProperty(value, SubValueProp, &SubValue);

					WriteProperty(ReturnParam, Buffer, SubValue);						
				}
				// rejects 'const T&' and pass 'T&' as its name
				else if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					JsPropertyIdRef SubValueProp = JS_INVALID_REFERENCE;
					JsValueRef SubValue = JS_INVALID_REFERENCE;
					JsGetPropertyIdFromName(*Param->GetName(), &SubValueProp);
					JsGetProperty(value, SubValueProp, &SubValue);

					//if (!sub_value.IsEmpty())
					//{
						// value can be null if isolate is in trouble
						WriteProperty(Param, Buffer, SubValue);
					//}						
				}
			}			
		}
		else
		{
			if (ReturnParam)
			{
				WriteProperty(ReturnParam, Buffer, value);
			}
		}		
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS