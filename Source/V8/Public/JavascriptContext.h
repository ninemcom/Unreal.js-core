#pragma once

#include "CoreMinimal.h"
#include "ObjectMacros.h"
#include "Object.h"
#include "UObjectGlobals.h"
#include "ScriptMacros.h"
#include "HAL/Runnable.h"
#include "JavascriptContext.generated.h"

struct FPrivateJavascriptFunction;
struct FPrivateJavascriptRef;

USTRUCT(BlueprintType)
struct V8_API FJavascriptFunction
{
	GENERATED_BODY()

public:
	void Execute();
	void Execute(UScriptStruct* Struct, void* Buffer);

	TSharedPtr<FPrivateJavascriptFunction> Handle;
};

class FJavascriptBackgroundWork : public FRunnable
{
	FJavascriptFunction Callback;
	FJavascriptFunction Work;
	bool Done;

public:
	FJavascriptBackgroundWork(const FJavascriptFunction& InWork, const FJavascriptFunction& InCallback)
		: Callback(InCallback)
		, Work(InWork)
		, Done(false)
	{}

	virtual uint32 Run() override;

	bool IsDone() const { return Done; }
	FJavascriptFunction& GetCallback() { return Callback; }
};

USTRUCT(BlueprintType)
struct V8_API FJavascriptRef
{
	GENERATED_BODY()

public:
	TSharedPtr<FPrivateJavascriptRef> Handle;
};

struct FJavascriptContext;

struct V8_API FArrayBufferAccessor
{	
	static int32 GetSize();
	static void* GetData();
	static void Discard();
};

USTRUCT()
struct FJavascriptRawAccess_Data
{
	GENERATED_BODY()
};

USTRUCT()
struct FJavascriptRawAccess
{
	GENERATED_BODY()
public:
    virtual ~FJavascriptRawAccess() {}
	virtual UScriptStruct* GetScriptStruct(int32 Index) { return nullptr; }
	virtual void* GetData(int32 Index) { return nullptr; }
	virtual int32 GetNumData() { return 0; }
	virtual FName GetDataName(int32 Index) { return FName(); }
};

USTRUCT()
struct FJavascriptMemoryStruct
{
	GENERATED_BODY()

public:
    virtual ~FJavascriptMemoryStruct() {}
	virtual int32 GetDimension() { return 1;  }
	virtual void* GetMemory(const int32* Dim) { return nullptr; }
	virtual int32 GetSize(int32 Dim) { return 0; }
};

USTRUCT(BlueprintType)
struct V8_API FJavascriptHeapStatistics
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 TotalHeapSize;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 TotalHeapSizeExecutable;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 TotalPhysicalSize;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 TotalAvailableSize;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 UsedHeapSize;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 HeapSizeLimit;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	int32 MallocedMemory;

	UPROPERTY(BlueprintReadOnly, Category = "Scripting | Javascript")
	bool bDoesZapGarbage;
};

UCLASS()
class V8_API UJavascriptContext : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	// Begin UObject interface.
	virtual void BeginDestroy() override;
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	// End UObject interface.

	TSharedPtr<FJavascriptContext> JavascriptContext;

	TSharedPtr<FString> ContextId;
	FDelegateHandle ExecStatusChangeHandle;

	UPROPERTY(BlueprintReadWrite, Category = "Scripting|Javascript")
	TArray<FString> Paths;

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void SetContextId(FString Name);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void Expose(FString Name, UObject* Object);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	FString GetScriptFileFullPath(FString Filename);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	FString ReadScriptFile(FString Filename);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void RunFile(FString Filename);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	FString RunScript(FString Script, bool bOutput = true);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void RequestV8GarbageCollection();

    UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
    void FindPathFile(FString TargetRootPath, FString TargetFileName, TArray<FString>& OutFiles);
	
    UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	bool WriteAliases(FString Target);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	bool WriteDTS(FString Target, bool bIncludingTooltip);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void SetAsDebugContext(int32 InPort = 5858);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void ResetAsDebugContext();

	UFUNCTION(BlueprintPure, Category = "Scripting|Javascript")
	bool IsDebugContext() const;

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void CreateInspector(int32 Port = 9229);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void DestroyInspector();

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void GetHeapStatistics(FJavascriptHeapStatistics& Statistics);

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void PauseTick();

	UFUNCTION(BlueprintCallable, Category = "Scripting|Javascript")
	void ResumeTick();

	bool HasProxyFunction(UObject* Holder, UFunction* Function);
	bool CallProxyFunction(UObject* Holder, UObject* This, UFunction* Function, void* Parms);
};