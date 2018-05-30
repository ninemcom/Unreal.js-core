#pragma once

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UObject/Class.h"
#include "JavascriptWidgetGeneratedClass.generated.h"

struct FJavascriptContext;

UCLASS()
class V8_API UJavascriptWidgetGeneratedClass : public UWidgetBlueprintGeneratedClass
{
	GENERATED_BODY()

public:		
	TWeakPtr<FJavascriptContext> JavascriptContext;	

	//virtual void InitPropertiesFromCustomList(uint8* DataPtr, const uint8* DefaultDataPtr) override;
};
