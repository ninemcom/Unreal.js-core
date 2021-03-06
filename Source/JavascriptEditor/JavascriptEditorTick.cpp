#include "JavascriptEditorTick.h"
#include "IV8.h"
#include "ScopedTransaction.h"
#include "Engine/Engine.h"

#if WITH_EDITOR
#include "Editor/EditorEngine.h"
#endif

#if WITH_EDITOR
class FJavascriptEditorTick : public FTickableEditorObject
{
	bool Tickable;
	FDelegateHandle ExecStatusChangedHandle;

	UJavascriptEditorTick* Object;
public:
	FJavascriptEditorTick(UJavascriptEditorTick* InObject)
		: Tickable(true)
		, Object(InObject)
	{
		ExecStatusChangedHandle = IV8::Get().GetExecStatusChangedDelegate().AddLambda([=](bool Status) {
			Tickable = Status;
		});
	}

	virtual ~FJavascriptEditorTick()
	{
		IV8::Get().GetExecStatusChangedDelegate().Remove(ExecStatusChangedHandle);
	}

	virtual void Tick(float DeltaTime) override
	{
		if (Tickable)
		{
			FEditorScriptExecutionGuard ScriptGuard;

			Object->OnTick.ExecuteIfBound(DeltaTime);
		}
	}

	virtual bool IsTickable() const override
	{
		return true;
	}

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FJavascriptEditorTick, STATGROUP_Tickables);
	}
};
#endif

UJavascriptEditorTick::UJavascriptEditorTick(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
#if WITH_EDITOR
	if (!IsTemplate(RF_ClassDefaultObject))
	{
		Tickable = new FJavascriptEditorTick(this);
	}
#endif
}

#if WITH_EDITOR
void UJavascriptEditorTick::BeginDestroy()
{
	Super::BeginDestroy();

	if (Tickable)
	{
		delete Tickable;
	}
}

UEditorEngine* UJavascriptEditorTick::GetEngine()
{
	return Cast<UEditorEngine>(GEngine);
}

void UJavascriptEditorTick::ForceTick(float DeltaTime)
{
	if (Tickable)
	{
		Tickable->Tick(DeltaTime);
	}
}
#endif