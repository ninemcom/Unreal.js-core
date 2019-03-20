#pragma once

#include "CoreMinimal.h"
#include "UserWidget.h"
#include "JavascriptWidget.generated.h"

class UJavascriptWidget;
class UJavascriptContext;
class UPanelSlot;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInputActionEvent, FName, ActionName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInputAxisEvent, float, Axis, FName, AxisName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnReleaseSlateResources, bool, bReleaseChildren);
/**
 * 
 */
UCLASS()
class JAVASCRIPTUMG_API UJavascriptWidget : public UUserWidget
{
	GENERATED_UCLASS_BODY()

public:	
	UPROPERTY(BlueprintReadWrite, Category = "Javascript")
	UJavascriptContext* JavascriptContext;

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	void SetRootWidget(UWidget* Widget);	

	virtual void ProcessEvent(UFunction* Function, void* Parms) override;
	virtual void NativeDestruct() override;
	virtual bool Initialize() override;

	UFUNCTION(BlueprintNativeEvent, Category = "Scripting | Javascript")
	bool InitializeWidget();

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	static void CallSynchronizeProperties(UVisual* WidgetOrSlot);	

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	static bool HasValidCachedWidget(UWidget* Widget);	

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	UPanelSlot* AddChild(UWidget* Content);

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	bool RemoveChild();

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	void OnListenForInputAction(FName ActionName, TEnumAsByte< EInputEvent > EventType, bool bConsume);

	UFUNCTION(BlueprintNativeEvent, Category = "Scripting | Javascript")
	void OnInputActionByName(FName ActionName);

	UFUNCTION(BlueprintNativeEvent, Category = "Scripting | Javascript")
	void OnReleaseInputActionByName(FName ActionName);

	UFUNCTION(BlueprintCallable, Category = "Scripting | Javascript")
	void OnListenForInputAxis(FName AxisName, TEnumAsByte< EInputEvent > EventType, bool bConsume);

	UFUNCTION(BlueprintNativeEvent, Category = "Scripting | Javascript")
	void OnInputAxisByName(float Axis, FName ActionName);
	
	UPROPERTY(BlueprintAssignable, Category = "Scripting | Javascript")
	FOnInputActionEvent OnInputActionEvent;

	UPROPERTY(BlueprintAssignable, Category = "Scripting | Javascript")
	FOnInputActionEvent OnReleaseActionEvent;

	UPROPERTY(BlueprintAssignable, Category = "Scripting | Javascript")
	FOnInputAxisEvent OnInputAxisEvent;

	UPROPERTY(BlueprintAssignable, Category = "Scripting | Javascript")
	FOnReleaseSlateResources OnDestroy;
protected:

	UPROPERTY()
	UPanelSlot* ContentSlot;

protected:

	//UVisual interface
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	//~ End UVisual Interface

	virtual UClass* GetSlotClass() const;
	virtual void OnSlotAdded(UPanelSlot* InSlot);
	virtual void OnSlotRemoved(UPanelSlot* InSlot);
};