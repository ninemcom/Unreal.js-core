#include "JavascriptComponent.h"
#include "JavascriptContext.h"
#include "JavascriptStats.h"
#include "Engine/World.h"
#include "V8PCH.h"
#include "UnrealEngine.h"
#include "IV8.h"


DECLARE_CYCLE_STAT(TEXT("Javascript Component Tick Time"), STAT_JavascriptComponentTickTime, STATGROUP_Javascript);

UJavascriptComponent::UJavascriptComponent(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = false;
	bAutoActivate = true;
	bWantsInitializeComponent = true;
}

void UJavascriptComponent::OnRegister()
{
	auto ContextOwner = GetOuter();
	if (ContextOwner && !HasAnyFlags(RF_ClassDefaultObject) && !ContextOwner->HasAnyFlags(RF_ClassDefaultObject))
	{
		if (GetWorld() && ((GetWorld()->IsGameWorld() && !GetWorld()->IsPreviewWorld()) || bActiveWithinEditor))
		{
			double start = FPlatformTime::Seconds();
			auto Context = NewObject<UJavascriptContext>();
			UE_LOG(Javascript, Log, TEXT("took %.2lfs to create context"), FPlatformTime::Seconds() - start);

			JavascriptContext = Context;

			start = FPlatformTime::Seconds();
			Context->Expose("Root", this);
			UE_LOG(Javascript, Log, TEXT("took %.2lfs to expose Root"), FPlatformTime::Seconds() - start);

			start = FPlatformTime::Seconds();
			Context->Expose("GWorld", GetWorld());
			UE_LOG(Javascript, Log, TEXT("took %.2lfs to expose GWorld"), FPlatformTime::Seconds() - start);

			start = FPlatformTime::Seconds();
			Context->Expose("GEngine", GEngine);
			UE_LOG(Javascript, Log, TEXT("took %.2lfs to expose GEngine"), FPlatformTime::Seconds() - start);
		}
	}

	Super::OnRegister();
}

void UJavascriptComponent::Activate(bool bReset)
{
	Super::Activate(bReset);

	if (JavascriptContext)
	{
		double start = FPlatformTime::Seconds();
		JavascriptContext->RunFile(*ScriptSourceFile);
		UE_LOG(Javascript, Log, TEXT("script %s run for %.2lfs"), *ScriptSourceFile, FPlatformTime::Seconds() - start);

		SetComponentTickEnabled(OnTick.IsBound());	
	}

	OnBeginPlay.ExecuteIfBound();
}

void UJavascriptComponent::Deactivate()
{	
	OnEndPlay.ExecuteIfBound();

	Super::Deactivate();
}

void UJavascriptComponent::BeginDestroy()
{
	if (bIsActive)
	{
		Deactivate();
	}

	Super::BeginDestroy();
}

void UJavascriptComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	check(bRegistered);

	SCOPE_CYCLE_COUNTER(STAT_JavascriptComponentTickTime);

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	OnTick.ExecuteIfBound(DeltaTime);
}

void UJavascriptComponent::ForceGC()
{
	JavascriptContext->RequestV8GarbageCollection();
}

void UJavascriptComponent::Expose(FString ExposedAs, UObject* Object)
{
	JavascriptContext->Expose(ExposedAs, Object);
}

void UJavascriptComponent::Invoke(FName Name)
{
	OnInvoke.ExecuteIfBound(Name);
}

void UJavascriptComponent::ProcessEvent(UFunction* Function, void* Parms)
{
	if (JavascriptContext && JavascriptContext->CallProxyFunction(this, this, Function, Parms))
	{
		return;
	}

	Super::ProcessEvent(Function, Parms);
}

UObject* UJavascriptComponent::ResolveAsset(FName Name, bool bTryLoad)
{
	for (const auto& Item : Assets)
	{
		if (Item.Name == Name)
		{
			return bTryLoad ? Item.Asset.TryLoad() : Item.Asset.ResolveObject();
		}
	}

	return nullptr;
}

UClass* UJavascriptComponent::ResolveClass(FName Name)
{	
	for (const auto& Item : ClassAssets)
	{
		if (Item.Name == Name)
		{
			return Item.Class;
		}
	}

	return nullptr;
}

