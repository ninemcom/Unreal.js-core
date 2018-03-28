#include "V8PCH.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptComponent.h"
#include "Config.h"
#include "IV8.h"
#include "Helpers.h"
#include "Translator.h"
#include "Exception.h"

#include "JavascriptIsolate_Private.h"
#include "JavascriptContext_Private.h"

DEFINE_LOG_CATEGORY(Javascript);

uint32 FJavascriptBackgroundWork::Run()
{
	// context setted in Execute function
	Work.Execute();
	Done = true;
	return 0;
}

void UJavascriptContext::GetHeapStatistics(FJavascriptHeapStatistics& Statistics)
{
	//v8::HeapStatistics stats;

	if (JavascriptContext.IsValid())
	{
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsCheck(JsGetRuntime(JavascriptContext->context(), &runtime));

		size_t limit = 0, usage = 0;
		JsGetRuntimeMemoryLimit(runtime, &limit);
		JsGetRuntimeMemoryUsage(runtime, &usage);

		Statistics.HeapSizeLimit = limit;
		Statistics.UsedHeapSize = usage;
		//JavascriptIsolate->isolate_->GetHeapStatistics(&stats);

		//Statistics.TotalHeapSize = stats.total_heap_size();
		//Statistics.TotalHeapSizeExecutable = stats.total_heap_size_executable();
		//Statistics.TotalPhysicalSize = stats.total_physical_size();
		//Statistics.TotalAvailableSize = stats.total_available_size();
		//Statistics.UsedHeapSize = stats.used_heap_size();
		//Statistics.HeapSizeLimit = stats.heap_size_limit();
		//Statistics.MallocedMemory = stats.malloced_memory();
		//Statistics.bDoesZapGarbage = !!stats.does_zap_garbage();
	}
}

UJavascriptContext::UJavascriptContext(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
	const bool bIsClassDefaultObject = IsTemplate(RF_ClassDefaultObject);
	if (!bIsClassDefaultObject)
	{
		Paths = IV8::Get().GetGlobalScriptSearchPaths();
		JavascriptContext = TSharedPtr<FJavascriptContext>(FJavascriptContext::Create(reinterpret_cast<JsRuntimeHandle>(IV8::Get().GetRuntime()), Paths));

		Expose("Context", this);

		SetContextId(GetName());

		ExecStatusChangeHandle = IV8::Get().GetExecStatusChangedDelegate().AddLambda([=](bool Status) {
			if (Status) {
				ResumeTick();
			} else {
				PauseTick();
			}
		});
	}
}

void UJavascriptContext::SetContextId(FString Name)
{
	ContextId = MakeShareable(new FString(Name));
}

void UJavascriptContext::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UJavascriptContext* This = CastChecked<UJavascriptContext>(InThis);

	if (This->JavascriptContext.IsValid())
	{
		Collector.AllowEliminatingReferences(false);

		This->JavascriptContext->AddReferencedObjects(This, Collector);

		Collector.AllowEliminatingReferences(true);
	}

	Super::AddReferencedObjects(This, Collector);
}

void UJavascriptContext::Expose(FString Name, UObject* Object)
{
	JavascriptContext->Expose(Name, Object);
}

FString UJavascriptContext::GetScriptFileFullPath(FString Filename)
{
	return JavascriptContext->GetScriptFileFullPath(Filename);
}

FString UJavascriptContext::ReadScriptFile(FString Filename)
{
	return JavascriptContext->ReadScriptFile(Filename);
}

void UJavascriptContext::RunFile(FString Filename)
{
	JavascriptContext->Public_RunFile(Filename);
}

FString UJavascriptContext::RunScript(FString Script, bool bOutput)
{
	return JavascriptContext->Public_RunScript(Script, bOutput);	
}

void UJavascriptContext::RequestV8GarbageCollection()
{
	JavascriptContext->RequestV8GarbageCollection();
}

void UJavascriptContext::FindPathFile(FString TargetRootPath, FString TargetFileName, TArray<FString>& OutFiles)
{
    JavascriptContext->FindPathFile(TargetRootPath, TargetFileName, OutFiles);
}

void UJavascriptContext::SetAsDebugContext(int32 InPort)
{
	JavascriptContext->SetAsDebugContext(InPort);
}

void UJavascriptContext::ResetAsDebugContext()
{
	JavascriptContext->ResetAsDebugContext();
}

void UJavascriptContext::CreateInspector(int32 Port)
{
	JavascriptContext->CreateInspector(Port);
}

void UJavascriptContext::DestroyInspector()
{
	JavascriptContext->DestroyInspector();
}

bool UJavascriptContext::IsDebugContext() const
{
	return JavascriptContext->IsDebugContext();
}

bool UJavascriptContext::WriteAliases(FString Filename)
{
	return JavascriptContext->WriteAliases(Filename);
}

bool UJavascriptContext::WriteDTS(FString Filename, bool bIncludingTooltip)
{
	return JavascriptContext->WriteDTS(Filename, bIncludingTooltip);
}

bool UJavascriptContext::HasProxyFunction(UObject* Holder, UFunction* Function)
{
	return JavascriptContext->HasProxyFunction(Holder, Function);
}

bool UJavascriptContext::CallProxyFunction(UObject* Holder, UObject* This, UFunction* FunctionToCall, void* Parms)
{
	return JavascriptContext->CallProxyFunction(Holder, This, FunctionToCall, Parms);
}

void UJavascriptContext::PauseTick()
{
	return JavascriptContext->PauseTick();
}

void UJavascriptContext::ResumeTick()
{
	return JavascriptContext->ResumeTick();
}

void UJavascriptContext::BeginDestroy()
{
	Super::BeginDestroy();

	IV8::Get().GetExecStatusChangedDelegate().Remove(ExecStatusChangeHandle);
	JavascriptContext.Reset();
	ContextId.Reset();
}
