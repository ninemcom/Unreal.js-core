#pragma once

#include "JavascriptIsolate_Private.h"

struct FStructMemoryInstance;
struct FJavascriptContext;
struct IPropertyOwner;

struct FPendingClassConstruction
{
	FPendingClassConstruction() {}
	FPendingClassConstruction(JsValueRef InObject, UClass* InClass)
		: Object(InObject), Class(InClass)
	{
	}

	FPendingClassConstruction(const FPendingClassConstruction& Other)
		: Object(Other.Object.Get()), Class(Other.Class), bCatched(Other.bCatched)
	{
	}

	Persistent<JsValueRef> Object;
	UClass* Class;
	bool bCatched{ false };

	void Finalize(FJavascriptContext* Context, UObject* Object);
};

struct FJavascriptContext : TSharedFromThis<FJavascriptContext>
{
	/** A map from Unreal UObject to V8 Object */
	TMap< UObject*, Persistent<JsValueRef> > ObjectToObjectMap;

	/** A map from Struct buffer to V8 Object */
	TMap< TSharedPtr<FStructMemoryInstance>, Persistent<JsValueRef> > MemoryToObjectMap;

	virtual ~FJavascriptContext() {}
	virtual void Expose(FString RootName, UObject* Object) = 0;
	virtual FString GetScriptFileFullPath(const FString& Filename) = 0;
	virtual FString ReadScriptFile(const FString& Filename) = 0;
	virtual FString Public_RunScript(const FString& Script, bool bOutput = true) = 0;
	virtual void RequestV8GarbageCollection() = 0;
	virtual void Public_RunFile(const FString& Filename) = 0;
    virtual void FindPathFile(const FString TargetRootPath, const FString TargetFileName, TArray<FString>& OutFiles) = 0;
	virtual void SetAsDebugContext(int32 InPort) = 0;
	virtual void ResetAsDebugContext() = 0;
	virtual bool IsDebugContext() const = 0;
	virtual void CreateInspector(int32 Port) = 0;
	virtual void DestroyInspector() = 0;
	virtual bool WriteAliases(const FString& Filename) = 0;
	virtual bool WriteDTS(const FString& Filename, bool bIncludingTooltip) = 0;
	virtual bool HasProxyFunction(UObject* Holder, UFunction* Function) = 0;
	virtual bool CallProxyFunction(UObject* Holder, UObject* This, UFunction* FunctionToCall, void* Parms) = 0;	

	virtual void UncaughtException(const FString& Exception) = 0;

	//virtual v8::Isolate* isolate() = 0;
	virtual JsContextRef context() = 0;
	virtual JsValueRef ExportObject(UObject* Object, bool bForce = false) = 0;
	virtual JsValueRef GetProxyFunction(UObject* Object, const TCHAR* Name) = 0;
	virtual JsValueRef ReadProperty(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner) = 0;
	virtual void WriteProperty(UProperty* Property, uint8* Buffer, JsValueRef Value) = 0;

	static FJavascriptContext* FromChakra(JsContextRef Context);

	static FJavascriptContext* Create(JsRuntimeHandle InRuntime, TArray<FString>& InPaths);

	virtual void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) = 0;

	virtual const FObjectInitializer* GetObjectInitializer() = 0;

	virtual JsFunctionRef ExportStruct(UScriptStruct* ScriptStruct) = 0;
	virtual JsFunctionRef ExportClass(UClass* Class, bool bAutoRegister = true) = 0;
	virtual void RegisterClass(UClass* Class, JsFunctionRef Template) = 0;
	virtual JsValueRef GetGlobalTemplate() = 0;
	virtual JsValueRef ExportStructInstance(UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner) = 0;

	/** A map from Unreal UClass to V8 Function template */
	TMap< UClass*, Persistent<JsValueRef> > ClassToFunctionTemplateMap;

	/** A map from Unreal UScriptStruct to V8 Function template */
	TMap< UScriptStruct*, Persistent<JsValueRef> > ScriptStructToFunctionTemplateMap;	

	/** BlueprintFunctionLibrary function mapping */
	TMultiMap< const UStruct*, UFunction*> BlueprintFunctionLibraryMapping;

	TMultiMap< const UStruct*, UFunction*> BlueprintFunctionLibraryFactoryMapping;

	TArray<FPendingClassConstruction> ObjectUnderConstructionStack;

	JsRuntimeHandle runtime_;
};
