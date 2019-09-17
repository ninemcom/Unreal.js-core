#include "V8PCH.h"
#include "JavascriptIsolate_Private.h"
#include "JavascriptContext_Private.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#include "Translator.h"
#include "Exception.h"
#include "Helpers.h"
#include "JavascriptStats.h"

namespace v8
{
	void CallJavascriptFunction(Local<Context> context, Local<Value> This, UFunction* SignatureFunction, Local<Function> func, void* Parms)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptFunctionCallToJavascript);

		auto isolate = context->GetIsolate();

		HandleScope handle_scope(isolate);

		auto Buffer = reinterpret_cast<uint8*>(Parms);		

		enum { MaxArgs = 32 };

		Local<Value> argv[MaxArgs];
		int argc = 0;

		TFieldIterator<UProperty> Iter(SignatureFunction);
		for (; Iter && argc < MaxArgs && (Iter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++Iter)
		{
			UProperty* Param = *Iter;
			argv[argc++] = ReadProperty(isolate, Param, Buffer, FNoPropertyOwner());
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

		TryCatch try_catch(isolate);

		Isolate::AllowJavascriptExecutionScope allow_script(isolate);

		auto maybeValue = func->Call(context, This, argc, argv);

		if (try_catch.HasCaught())
		{
			FJavascriptContext::FromV8(context)->UncaughtException(FV8Exception::Report(isolate, try_catch));
		}

		FIsolateHelper I(isolate);

		if (maybeValue.IsEmpty())
		{
			I.Throw(TEXT("..."));
			return;
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

		auto value = maybeValue.ToLocalChecked();

		if (bHasAnyOutParams)
		{
			if (!value->IsObject())
			{
				I.Throw(TEXT("..."));
				return;
			}

			auto Object = value->ToObject(context).ToLocalChecked();

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(SignatureFunction); It; ++It)
			{
				UProperty* Param = *It;

				auto PropertyFlags = Param->GetPropertyFlags();					

				// pass return parameter as '$'
				if (PropertyFlags & CPF_ReturnParm)
				{
					auto sub_value = Object->Get(I.Keyword("$"));

					WriteProperty(isolate, ReturnParam, Buffer, sub_value, FNoPropertyOwner());						
				}
				// rejects 'const T&' and pass 'T&' as its name
				else if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					auto sub_value = Object->Get(I.Keyword(Param->GetName()));

					if (!sub_value.IsEmpty())
					{
						// value can be null if isolate is in trouble
						WriteProperty(isolate, Param, Buffer, sub_value, FNoPropertyOwner());
					}						
				}
			}			
		}
		else
		{
			if (ReturnParam)
			{
				WriteProperty(isolate, ReturnParam, Buffer, value, FNoPropertyOwner());
			}
		}		
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS