#include "V8PCH.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

THIRD_PARTY_INCLUDES_START
//#include <libplatform/libplatform.h>
//#include "node_platform.h"
THIRD_PARTY_INCLUDES_END

#include "JavascriptContext.h"
#include "IV8.h"
#include "JavascriptStats.h"
#include "JavascriptSettings.h"
#include "Containers/Ticker.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

DEFINE_STAT(STAT_V8IdleTask);
DEFINE_STAT(STAT_JavascriptDelegate);
DEFINE_STAT(STAT_JavascriptProxy);
DEFINE_STAT(STAT_Scavenge);
DEFINE_STAT(STAT_MarkSweepCompact);
DEFINE_STAT(STAT_IncrementalMarking);
DEFINE_STAT(STAT_ProcessWeakCallbacks);

DEFINE_STAT(STAT_JavascriptPropertyGet);
DEFINE_STAT(STAT_JavascriptPropertySet);
DEFINE_STAT(STAT_JavascriptFunctionCallToEngine);
DEFINE_STAT(STAT_JavascriptFunctionCallToJavascript);
DEFINE_STAT(STAT_JavascriptReadOffStruct);

DEFINE_STAT(STAT_NewSpace);
DEFINE_STAT(STAT_OldSpace);
DEFINE_STAT(STAT_CodeSpace);
DEFINE_STAT(STAT_MapSpace);
DEFINE_STAT(STAT_LoSpace);

static float GV8IdleTaskBudget = 1 / 60.0f;

UJavascriptSettings::UJavascriptSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// --harmony for ES6 in V8, ChakraCore supports ES6 natively
	V8Flags = TEXT("--harmony --harmony-shipping --es-staging --expose-gc");
}

void UJavascriptSettings::Apply() const
{
	//IV8::Get().SetFlagsFromString(V8Flags);
}

class V8Module : public IV8
{
public:
	TArray<FString> Paths;
	JsRuntimeHandle ChakraRuntime = JS_INVALID_RUNTIME_HANDLE;

	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		Paths.Add(GetGameScriptsDirectory());
		//@HACK : Dirty hacks
		Paths.Add(GetPluginScriptsDirectory());
		Paths.Add(GetPluginScriptsDirectory2());
		Paths.Add(GetPluginScriptsDirectory3());
		Paths.Add(GetPluginScriptsDirectory4());
		Paths.Add(GetPakPluginScriptsDirectory());

		const UJavascriptSettings& Settings = *GetDefault<UJavascriptSettings>();
		Settings.Apply();

		//V8::InitializeICU(nullptr);
		JsErrorCode createErr = JsCreateRuntime(JsRuntimeAttributeDisableNativeCodeGeneration, nullptr, &ChakraRuntime);
		checkf(createErr == JsNoError, TEXT("Failed to create javascript runtime! %d"), (int)createErr);

		FName NAME_JavascriptCmd("JavascriptCmd");
		GLog->Log(NAME_JavascriptCmd, ELogVerbosity::Log, *FString::Printf(TEXT("Unreal.js started. ChakraCore %d.%d.%d"), CHAKRA_CORE_MAJOR_VERSION, CHAKRA_CORE_MINOR_VERSION, CHAKRA_CORE_PATCH_VERSION));
	}

	virtual void ShutdownModule() override
	{		
		JsDisposeRuntime(ChakraRuntime);
	}

	//@HACK
	static FString GetPluginScriptsDirectory()
	{
		return FPaths::EnginePluginsDir() / "Backend/UnrealJS/Content/Scripts/";
	}

	static FString GetPluginScriptsDirectory2()
	{
		return FPaths::EnginePluginsDir() / "UnrealJS/Content/Scripts/";
	}

	static FString GetPluginScriptsDirectory3()
	{
		return FPaths::EnginePluginsDir() / "Marketplace/UnrealJS/Content/Scripts/";
	}

	static FString GetPluginScriptsDirectory4()
	{
		return FPaths::ProjectPluginsDir() / "UnrealJS/Content/Scripts/";
	}

	static FString GetPakPluginScriptsDirectory()
	{
		return FPaths::EngineDir() / "Contents/Scripts/";
	}

	static FString GetGameScriptsDirectory()
	{
		return FPaths::ProjectContentDir() / "Scripts/";
	}

	virtual void AddGlobalScriptSearchPath(const FString& Path) override
	{
		Paths.Add(Path);
	}

	virtual void RemoveGlobalScriptSearchPath(const FString& Path) override
	{
		Paths.Remove(Path);
	}

	virtual TArray<FString> GetGlobalScriptSearchPaths() override
	{
		return Paths;
	}

	virtual void FillAutoCompletion(TSharedPtr<FString> TargetContext, TArray<FString>& OutArray, const TCHAR* Input) override
	{
		static auto SourceCode = LR"doc(
(function () {
    var pattern = '%s'; var head = '';
    pattern.replace(/\\W*([\\w\\.]+)$/, function (a, b, c) { head = pattern.substr(0, c + a.length - b.length); pattern = b });
    var index = pattern.lastIndexOf('.');
    var scope = this;
    var left = '';
    if (index >= 0) {
        left = pattern.substr(0, index + 1);
        try { scope = eval(pattern.substr(0, index)); }
        catch (e) { scope = null; }
        pattern = pattern.substr(index + 1);
    }
    var result = [];
    for (var k in scope) {
        if (k.indexOf(pattern) == 0) {
            result.push(head + left + k);
        }
    }
    return result.join(',');
})()
)doc";

		for (TObjectIterator<UJavascriptContext> It; It; ++It)
		{
			UJavascriptContext* Context = *It;

			if (Context->ContextId == TargetContext || (!TargetContext.IsValid() && Context->IsDebugContext()))
			{
				FString Result = Context->RunScript(FString::Printf(SourceCode, *FString(Input).ReplaceCharWithEscapedChar()), false);
				Result.ParseIntoArray(OutArray, TEXT(","));
			}
		}
	}

	virtual void Exec(TSharedPtr<FString> TargetContext, const TCHAR* Command) override
	{
		for (TObjectIterator<UJavascriptContext> It; It; ++It)
		{
			UJavascriptContext* Context = *It;

			if (Context->ContextId == TargetContext || (!TargetContext.IsValid() && Context->IsDebugContext()))
			{
				static FName NAME_JavascriptCmd("JavascriptCmd");
				GLog->Log(NAME_JavascriptCmd, ELogVerbosity::Log, Command);
				Context->RunScript(Command);
			}
		}
	}

	virtual bool HasDebugContext() const
	{
		for (TObjectIterator<UJavascriptContext> It; It; ++It)
		{
			UJavascriptContext* Context = *It;

			if (Context->IsDebugContext())
			{
				return true;
			}
		}

		return false;
	}

	virtual void GetContextIds(TArray<TSharedPtr<FString>>& OutContexts) override
	{
		for (TObjectIterator<UJavascriptContext> It; It; ++It)
		{
			UJavascriptContext* Context = *It;

			if (!Context->IsTemplate(RF_ClassDefaultObject))
			{
				OutContexts.Add(Context->ContextId);
			}
		}
	}

	// not support in chakracore
	//virtual void SetFlagsFromString(const FString& V8Flags) override
	//{
	//	V8::SetFlagsFromString(TCHAR_TO_ANSI(*V8Flags), strlen(TCHAR_TO_ANSI(*V8Flags)));
	//}

	virtual void SetIdleTaskBudget(float BudgetInSeconds) override
	{
		GV8IdleTaskBudget = BudgetInSeconds;
	}

	virtual void* GetRuntime() override
	{
		return ChakraRuntime;
	}

	//virtual void* GetV8Platform() override
	//{
	//	return platform_.platform();
	//}
};

IMPLEMENT_MODULE(V8Module, V8)

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
