#include "V8PCH.h"
#include "JavascriptContext_Private.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#include "Translator.h"
#include "Exception.h"
#include "Helpers.h"
#include "JavascriptStats.h"

namespace chakra
{
	void CallJavascriptFunction(JsContextRef context, JsValueRef This, UFunction* SignatureFunction, JsValueRef func, void* Parms)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptFunctionCallToJavascript);

		auto Buffer = reinterpret_cast<uint8*>(Parms);		

		enum { MaxArgs = 32 };

		JsValueRef argv[MaxArgs] = { This };
		int argc = 1;

		auto jsContext = FJavascriptContext::FromChakra(context);

		TFieldIterator<UProperty> Iter(SignatureFunction);
		for (; Iter && argc < MaxArgs && (Iter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++Iter)
		{
			UProperty* Param = *Iter;
			argv[argc++] = jsContext->ReadProperty(Param, Buffer, FNoPropertyOwner());
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
		JsErrorCode invokeErr = JsCallFunction(func, argv, argc, &value);
		if (invokeErr == JsErrorScriptException)
		{
			JsValueRef exception = JS_INVALID_REFERENCE;
			JsCheck(JsGetAndClearException(&exception));

			jsContext->UncaughtException(FV8Exception::Report(exception));
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
			if (chakra::IsEmpty(value) || !chakra::IsObject(value))
			{
				chakra::Throw(TEXT("..."));
				return;
			}

			JsValueRef Object = value;

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(SignatureFunction); It; ++It)
			{
				UProperty* Param = *It;

				auto PropertyFlags = Param->GetPropertyFlags();					

				// pass return parameter as '$'
				if (PropertyFlags & CPF_ReturnParm)
				{
					JsValueRef sub_value = chakra::GetProperty(Object, "$");

					jsContext->WriteProperty(ReturnParam, Buffer, sub_value);						
				}
				// rejects 'const T&' and pass 'T&' as its name
				else if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					JsValueRef sub_value = chakra::GetProperty(Object, Param->GetName());

					if (!chakra::IsEmpty(sub_value))
					{
						// value can be null if isolate is in trouble
						jsContext->WriteProperty(Param, Buffer, sub_value);
					}						
				}
			}			
		}
		else
		{
			if (ReturnParam)
			{
				jsContext->WriteProperty(ReturnParam, Buffer, value);
			}
		}		
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS