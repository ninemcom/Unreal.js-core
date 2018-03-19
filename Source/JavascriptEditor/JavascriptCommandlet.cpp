#include "JavascriptCommandlet.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"

UJavascriptCommandlet::UJavascriptCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{}

int32 UJavascriptCommandlet::Main(const FString& Params)
{
	bool bSuccess = false;

#if !UE_BUILD_SHIPPING
	const TCHAR* ParamStr = *Params;
	ParseCommandLine(ParamStr, CmdLineTokens, CmdLineSwitches);	

	{
		auto JavascriptContext = NewObject<UJavascriptContext>();

		JavascriptContext->Expose(TEXT("Root"), this);
				
		JavascriptContext->AddToRoot();

		JavascriptContext->SetContextId(TEXT("Commandlet"));
		
		{
			FEditorScriptExecutionGuard ScriptGuard;

			if (CmdLineTokens.Num())
			{
				JavascriptContext->RunFile(CmdLineTokens[0]);
				bSuccess = true;
			}
		}

		JavascriptContext->JavascriptContext.Reset();

		JavascriptContext->RemoveFromRoot();
	}
#else	
	bSuccess = false;
#endif

	return bSuccess ? 0 : 1;
}