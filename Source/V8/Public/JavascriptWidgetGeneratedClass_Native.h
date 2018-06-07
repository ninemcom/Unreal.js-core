#pragma once

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UObject/Class.h"
#include "JavascriptWidgetGeneratedClass_Native.generated.h"

struct FJavascriptContext;

UCLASS()
class V8_API UJavascriptWidgetGeneratedClass_Native : public UWidgetBlueprintGeneratedClass
{
	GENERATED_BODY()

public:		
	TWeakPtr<FJavascriptContext> JavascriptContext;	

	// UObject interface
	virtual void Serialize(FArchive& Ar) override { UClass::Serialize(Ar);  }
	virtual void PostLoad() override { UClass::PostLoad(); }
	virtual void PostInitProperties() override { UClass::PostInitProperties(); }
	// End UObject interface

	// UClass interface
#if WITH_EDITOR
	virtual UClass* GetAuthoritativeClass() override { return UClass::GetAuthoritativeClass();  }
	virtual void ConditionalRecompileClass(TArray<UObject*>* ObjLoaded) override { UClass::ConditionalRecompileClass(ObjLoaded);  }
	virtual UObject* GetArchetypeForCDO() const override { return UClass::GetArchetypeForCDO();  }
#endif //WITH_EDITOR
	virtual bool IsFunctionImplementedInBlueprint(FName InFunctionName) const override { return false;  }
	virtual uint8* GetPersistentUberGraphFrame(UObject* Obj, UFunction* FuncToCheck) const override { return nullptr;  }
#if ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION < 12
	virtual void CreatePersistentUberGraphFrame(UObject* Obj, bool bCreateOnlyIfEmpty = false, bool bSkipSuperClass = false) const override {}
#else
	virtual void CreatePersistentUberGraphFrame(UObject* Obj, bool bCreateOnlyIfEmpty = false, bool bSkipSuperClass = false, UClass* OldClass = nullptr) const override {}
#endif
	virtual void DestroyPersistentUberGraphFrame(UObject* Obj, bool bSkipSuperClass = false) const override {}
	virtual void Link(FArchive& Ar, bool bRelinkExistingProperties) override { UClass::Link(Ar, bRelinkExistingProperties); }
	virtual void PurgeClass(bool bRecompilingOnLoad) override { UClass::PurgeClass(bRecompilingOnLoad);  }
	virtual void Bind() override { UClass::Bind(); }
	virtual void GetRequiredPreloadDependencies(TArray<UObject*>& DependenciesOut) override { UClass::GetRequiredPreloadDependencies(DependenciesOut);  }
	virtual UObject* FindArchetype(UClass* ArchetypeClass, const FName ArchetypeName) const override { return UClass::FindArchetype(ArchetypeClass, ArchetypeName);  }
	// End UClass interface

	//virtual void InitPropertiesFromCustomList(uint8* DataPtr, const uint8* DefaultDataPtr) override;
};
