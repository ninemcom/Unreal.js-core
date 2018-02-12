
#include "JavascriptContext_Private.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptComponent.h"
#include "FileManager.h"
#include "Config.h"
#include "Translator.h"
#include "Exception.h"
#include "IV8.h"
#include "V8PCH.h"
#include "Engine/Blueprint.h"
#include "FileHelper.h"
#include "Paths.h"
#include "JavascriptIsolate_Private.h"
#include "PropertyPortFlags.h"

#if WITH_EDITOR
#include "TypingGenerator.h"
#endif

#include "Helpers.h"
#include "JavascriptGeneratedClass_Native.h"
#include "JavascriptGeneratedClass.h"
#include "JavascriptGeneratedFunction.h"
#include "StructMemoryInstance.h"

#include "JavascriptStats.h"

#include "../../Launch/Resources/Version.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

static const int kContextEmbedderDataIndex = 1;
static const int32 MagicNumber = 0x2852abd3;
static const FString URL_FilePrefix(TEXT("file:///"));

static FString LocalPathToURL(FString Path)
{
	return URL_FilePrefix + Path.Replace(TEXT("\\"), TEXT("/")).Replace(TEXT(" "), TEXT("%20"));
}

static FString URLToLocalPath(FString URL)
{
	if (URL.StartsWith(*URL_FilePrefix))
	{
		URL = URL.Mid(URL_FilePrefix.Len()).Replace(TEXT("%20"), TEXT(" "));
	}
#if PLATFORM_WINDOWS
	URL = URL.Replace(TEXT("\\"), TEXT("/"));
#endif
	return URL;
}

static TArray<FString> StringArrayFromChakra(JsValueRef InArray)
{
	TArray<FString> OutArray;
	if (InArray != JS_INVALID_REFERENCE && chakra::IsArray(InArray))
	{
		int len = chakra::Length(InArray);
		for (decltype(len) Index = 0; Index < len; ++Index)
		{
			OutArray.Add(chakra::StringFromChakra(chakra::GetIndex(InArray, Index)));
		}
	}

	return OutArray;
};

#if WITH_EDITOR
template <typename Type>
static void SetMetaData(Type* Object, const FString& Key, const FString& Value)
{
	if (Key.Compare(TEXT("None"), ESearchCase::IgnoreCase) == 0 || Key.Len() == 0) return;

	if (Value.Len() == 0)
	{
		Object->SetMetaData(*Key, TEXT("true"));
	}
	else
	{
		Object->SetMetaData(*Key, *Value);
	}
}
#endif

static void SetFunctionFlags(UFunction* Function, const TArray<FString>& Flags)
{
	static struct FKeyword {
		const TCHAR* Keyword;
		EFunctionFlags Flags;
	} Keywords[] = {
		{ TEXT("Exec"), FUNC_Exec },
		{ TEXT("Server"), FUNC_Net | FUNC_NetServer },
		{ TEXT("Client"), FUNC_Net | FUNC_NetClient },
		{ TEXT("NetMulticast"), FUNC_Net | FUNC_NetMulticast },
		{ TEXT("Event"), FUNC_Event },
		{ TEXT("Delegate"), FUNC_Delegate },
		{ TEXT("MulticastDelegate"), FUNC_MulticastDelegate | FUNC_Delegate },
		{ TEXT("Reliable"), FUNC_NetReliable },
		{ TEXT("Unreliable"), FUNC_NetResponse }
	};

	for (const auto& Flag : Flags)
	{
		FString Left, Right;
		if (!Flag.Split(TEXT(":"), &Left, &Right))
		{
			Left = Flag;
		}

		bool bHasMatch = false;
		for (const auto& Keyword : Keywords)
		{
			if (Left.Compare(Keyword.Keyword, ESearchCase::IgnoreCase) == 0)
			{
				Function->FunctionFlags |= Keyword.Flags;
				bHasMatch = true;
				break;
			}
		}

#if WITH_EDITOR
		if (!bHasMatch)
		{
			SetMetaData(Function, Left, Right);
		}
#endif
	}
}

static void SetClassFlags(UClass* Class, const TArray<FString>& Flags)
{
	static struct FKeyword {
		const TCHAR* Keyword;
		EClassFlags Flags;
	} Keywords[] = {
		{ TEXT("Abstract"), CLASS_Abstract },
		{ TEXT("DefaultConfig"), CLASS_DefaultConfig },
		{ TEXT("Transient"), CLASS_Transient },
		{ TEXT("AdvancedDisplay"), CLASS_AdvancedDisplay },
		{ TEXT("NotPlaceable"), CLASS_NotPlaceable },
		{ TEXT("PerObjectConfig"), CLASS_PerObjectConfig },
		{ TEXT("EditInlineNew"), CLASS_EditInlineNew },
		{ TEXT("CollapseCategories"), CLASS_CollapseCategories },
		{ TEXT("Const"), CLASS_Const },
		{ TEXT("DefaultToInstanced"), CLASS_DefaultToInstanced },
		{ TEXT("Hidden"), CLASS_Hidden },
		{ TEXT("HideDropDown"), CLASS_HideDropDown }
	};

	for (const auto& Flag : Flags)
	{
		FString Left, Right;
		if (!Flag.Split(TEXT(":"), &Left, &Right))
		{
			Left = Flag;
		}

		bool bHasMatch{ false };
		for (const auto& Keyword : Keywords)
		{
			if (Left.Compare(Keyword.Keyword, ESearchCase::IgnoreCase) == 0)
			{
				Class->ClassFlags |= Keyword.Flags;
				bHasMatch = true;
				break;
			}
			else if (Left.StartsWith(TEXT("Not")) && Left.Mid(3).Compare(Keyword.Keyword, ESearchCase::IgnoreCase) == 0)
			{
				Class->ClassFlags &= ~Keyword.Flags;
				bHasMatch = true;
				break;
			}
		}

#if WITH_EDITOR
		if (!bHasMatch)
		{
			SetMetaData(Class, Left, Right);
		}
#endif
	}
}

static void SetStructFlags(UScriptStruct* Struct, const TArray<FString>& Flags)
{
	static struct FKeyword {
		const TCHAR* Keyword;
		EStructFlags Flags;
	} Keywords[] = {
		{ TEXT("Atomic"), STRUCT_Atomic },
		{ TEXT("Immutable"), STRUCT_Immutable }
	};

	for (const auto& Flag : Flags)
	{
		FString Left, Right;
		if (!Flag.Split(TEXT(":"), &Left, &Right))
		{
			Left = Flag;
		}

		bool bHasMatch{ false };
		for (const auto& Keyword : Keywords)
		{
			if (Left.Compare(Keyword.Keyword, ESearchCase::IgnoreCase) == 0)
			{
				Struct->StructFlags = (EStructFlags)(Struct->StructFlags | Keyword.Flags);
				bHasMatch = true;
				break;
			}
		}


#if WITH_EDITOR
		if (!bHasMatch)
		{
			SetMetaData(Struct, Left, Right);
		}
#endif
	}
}

static UProperty* CreateProperty(UObject* Outer, FName Name, const TArray<FString>& Decorators, FString Type, bool bIsArray, bool bIsSubclass, bool bIsMap)
{
	auto SetupProperty = [&](UProperty* NewProperty) {
		static struct FKeyword {
			const TCHAR* Keyword;
			uint64 Flags;
		} Keywords[] = {
			{ TEXT("Const"), CPF_ConstParm },
			{ TEXT("Return"), CPF_ReturnParm },
			{ TEXT("Out"), CPF_OutParm },
			{ TEXT("Replicated"), CPF_Net },
			{ TEXT("NotReplicated"), CPF_RepSkip },
			{ TEXT("ReplicatedUsing"), CPF_Net | CPF_RepNotify },
			{ TEXT("Transient"), CPF_Transient },
			{ TEXT("DuplicateTransient"), CPF_DuplicateTransient },
			{ TEXT("EditFixedSize"), CPF_EditFixedSize },
			{ TEXT("EditAnywhere"), CPF_Edit },
			{ TEXT("EditDefaultsOnly"), CPF_Edit | CPF_DisableEditOnInstance },
			{ TEXT("EditInstanceOnly"), CPF_Edit | CPF_DisableEditOnTemplate },
			{ TEXT("BlueprintReadOnly"), CPF_BlueprintVisible | CPF_BlueprintReadOnly },
			{ TEXT("BlueprintReadWrite"), CPF_BlueprintVisible },
			{ TEXT("Instanced"), CPF_PersistentInstance | CPF_ExportObject | CPF_InstancedReference },
			{ TEXT("GlobalConfig"), CPF_GlobalConfig | CPF_Config },
			{ TEXT("Config"), CPF_Config },
			{ TEXT("TextExportTransient"), CPF_TextExportTransient },
			{ TEXT("NonPIEDuplicateTransient"), CPF_NonPIEDuplicateTransient },
			{ TEXT("Export"), CPF_ExportObject },
			{ TEXT("EditFixedSize"), CPF_EditFixedSize },
			{ TEXT("NotReplicated"), CPF_RepSkip },
			{ TEXT("NonTransactional"), CPF_NonTransactional },
			{ TEXT("BlueprintAssignable"), CPF_BlueprintAssignable },
			{ TEXT("SimpleDisplay"), CPF_SimpleDisplay },
			{ TEXT("AdvancedDisplay"), CPF_AdvancedDisplay },
			{ TEXT("SaveGame"), CPF_SaveGame },
			{ TEXT("AssetRegistrySearchable"), CPF_AssetRegistrySearchable },
			{ TEXT("Interp"), CPF_Edit | CPF_Interp | CPF_BlueprintVisible },
			{ TEXT("NoClear"), CPF_NoClear },
			{ TEXT("VisibleAnywhere"), CPF_Edit | CPF_EditConst },
			{ TEXT("VisibleInstanceOnly"), CPF_Edit | CPF_EditConst | CPF_DisableEditOnTemplate },
			{ TEXT("VisibleDefaultsOnly"), CPF_Edit | CPF_EditConst | CPF_DisableEditOnInstance },
		};

		for (const auto& Flag : Decorators)
		{
			FString Left, Right;
			if (!Flag.Split(TEXT(":"), &Left, &Right))
			{
				Left = Flag;
			}


			bool bHasMatch{ false };

			for (const auto& Keyword : Keywords)
			{
				if (Left.Compare(Keyword.Keyword, ESearchCase::IgnoreCase) == 0)
				{
					NewProperty->SetPropertyFlags(Keyword.Flags);

					if (Keyword.Flags & CPF_RepNotify)
					{
						NewProperty->RepNotifyFunc = FName(*Right);
					}

					bHasMatch = true;
					break;
				}
			}

#if WITH_EDITOR
			if (!bHasMatch)
			{
				SetMetaData(NewProperty, Left, Right);
			}
#endif
		}

		return NewProperty;
	};

	auto Create = [&]() -> UProperty* {
		auto Inner = [&](UObject* Outer, const FString& Type) -> UProperty* {
			// Find TypeObject (to make UObjectHash happy)
			auto FindTypeObject = [](const TCHAR* ObjectName) -> UObject* {
				const TCHAR* PackagesToSearch[] = {
					TEXT("Engine"),
					TEXT("CoreUObject")
				};
				UObject* TypeObject = nullptr;
				for (auto PackageToSearch : PackagesToSearch)
				{
					TypeObject = StaticFindObject(UObject::StaticClass(), (UObject*)ANY_PACKAGE, *FString::Printf(TEXT("/Script/%s.%s"), PackageToSearch, ObjectName));
					if (TypeObject) return TypeObject;
				}

				TypeObject = StaticFindObject(UObject::StaticClass(), (UObject*)ANY_PACKAGE, ObjectName);
				if (TypeObject) return TypeObject;

				TypeObject = StaticLoadObject(UObject::StaticClass(), nullptr, ObjectName);
				if (TypeObject) return TypeObject;

				return nullptr;
			};

			if (Type == FString("bool"))
			{
				auto q = NewObject<UBoolProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("int"))
			{
				auto q = NewObject<UIntProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("string"))
			{
				auto q = NewObject<UStrProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("float"))
			{
				auto q = NewObject<UFloatProperty>(Outer, Name);
				return q;
			}
			else
			{
				UObject* TypeObject = FindTypeObject(*Type);

				if (auto p = Cast<UClass>(TypeObject))
				{
					if (bIsSubclass)
					{
						auto q = NewObject<UClassProperty>(Outer, Name);
						q->SetPropertyClass(UClass::StaticClass());
						q->SetMetaClass(p);
						return q;
					}
					else
					{
						auto q = NewObject<UObjectProperty>(Outer, Name);
						q->SetPropertyClass(p);
						return q;
					}
				}
				else  if (auto p = Cast<UBlueprint>(TypeObject))
				{
					if (bIsSubclass)
					{
						auto q = NewObject<UClassProperty>(Outer, Name);
						q->SetPropertyClass(UClass::StaticClass());
						q->SetMetaClass(p->GeneratedClass);
						return q;
					}
					else
					{
						auto q = NewObject<UObjectProperty>(Outer, Name);
						q->SetPropertyClass(p->GeneratedClass);
						return q;
					}
				}
				else if (auto p = Cast<UScriptStruct>(TypeObject))
				{
					auto q = NewObject<UStructProperty>(Outer, Name);
					q->Struct = p;
					return q;
				}
				else if (auto p = Cast<UEnum>(TypeObject))
				{
					auto q = NewObject<UByteProperty>(Outer, Name);
					q->Enum = p;
					return q;
				}
				else
				{
					auto q = NewObject<UInt64Property>(Outer, Name);
					return q;
				}
			}
		};

		if (bIsMap)
		{
			auto q = NewObject<UMapProperty>(Outer, Name);
			FString Left, Right;
			if (Type.Split(TEXT(":"), &Left, &Right))
			{
				auto Key = FName(*Name.ToString().Append(TEXT("_Key")));
				if (auto KeyProperty = CreateProperty(q, Key, Decorators, Left, false, false, false))
				{
					q->KeyProp = KeyProperty;
					q->KeyProp->SetPropertyFlags(CPF_HasGetValueTypeHash);
					auto Value = FName(*Name.ToString().Append(TEXT("_Value")));
					if (auto ValueProperty = CreateProperty(q, Value, Decorators, Right, bIsArray, bIsSubclass, false))
					{
						q->ValueProp = ValueProperty;
						if (q->ValueProp && q->ValueProp->HasAnyPropertyFlags(CPF_ContainsInstancedReference))
						{
							q->ValueProp->SetPropertyFlags(CPF_ContainsInstancedReference);
						}
					}
					else
						q->MarkPendingKill();
				}
				else
					q->MarkPendingKill();
			}

			return q;
		}
		else if (bIsArray)
		{
			auto q = NewObject<UArrayProperty>(Outer, Name);
			q->Inner = SetupProperty(Inner(q, Type));
			return q;
		}
		else
		{
			return Inner(Outer, Type);
		}
	};

	return SetupProperty(Create());
}

static UProperty* CreatePropertyFromDecl(UObject* Outer, JsValueRef PropertyDecl)
{
	JsValueRef Decl = PropertyDecl;
	JsValueRef Name = chakra::GetProperty(Decl, "Name");
	JsValueRef Type = chakra::GetProperty(Decl, "Type");
	JsValueRef Decorators = chakra::GetProperty(Decl, "Decorators");
	JsValueRef IsArray = chakra::GetProperty(Decl, "IsArray");
	JsValueRef IsSubClass = chakra::GetProperty(Decl, "IsSubclass");
	JsValueRef IsMap = chakra::GetProperty(Decl, "IsMap");
	return CreateProperty(
		Outer,
		*chakra::StringFromChakra(Name),
		StringArrayFromChakra(Decorators),
		chakra::StringFromChakra(Type),
		IsArray != JS_INVALID_REFERENCE && chakra::BoolFrom(IsArray),
		IsSubClass != JS_INVALID_REFERENCE && chakra::BoolFrom(IsSubClass),
		IsMap != JS_INVALID_REFERENCE && chakra::BoolFrom(IsMap)
		);
}

static UProperty* DuplicateProperty(UObject* Outer, UProperty* Property, FName Name)
{
	auto SetupProperty = [&](UProperty* NewProperty) {
		NewProperty->SetPropertyFlags(Property->GetPropertyFlags());
		return NewProperty;
	};

	auto Clone = [&]() -> UProperty* {
		if (auto p = Cast<UStructProperty>(Property))
		{
			auto q = NewObject<UStructProperty>(Outer, Name);
			q->Struct = p->Struct;
			return q;
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			auto q = NewObject<UArrayProperty>(Outer, Name);
			q->Inner = DuplicateProperty(q, p->Inner, p->Inner->GetFName());
			return q;
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			auto q = NewObject<UByteProperty>(Outer, Name);
			q->Enum = p->Enum;
			return q;
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			auto q = NewObject<UBoolProperty>(Outer, Name);
			q->SetBoolSize(sizeof(bool), true);
			return q;
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			auto q = NewObject<UClassProperty>(Outer, Name);
			q->SetMetaClass(p->MetaClass);
			q->PropertyClass = UClass::StaticClass();
			return q;
		}
		else if (auto p = Cast<UObjectProperty>(Property))
		{
			auto q = NewObject<UObjectProperty>(Outer, Name);
			q->SetPropertyClass(p->PropertyClass);
			return q;
		}
		else
		{
			return static_cast<UProperty*>(StaticDuplicateObject(Property, Outer, *(Name.ToString())));
		}
	};

	return SetupProperty(Clone());
};

void UJavascriptGeneratedFunction::Thunk(FFrame& Stack, RESULT_DECL)
{
	auto Function = static_cast<UJavascriptGeneratedFunction*>(Stack.CurrentNativeFunction);

	auto ProcessInternal = [&](FFrame& Stack, RESULT_DECL)
	{
		if (Function->JavascriptContext.IsValid())
		{
			auto Context = Function->JavascriptContext.Pin();

			FContextScope context_scope(Context->context());

			bool bCallRet = Context->CallProxyFunction(Function->GetOuter(), this, Function, Stack.Locals);
			if (!bCallRet)
			{
				return;
			}

			UProperty* ReturnProp = ((UFunction*)Stack.Node)->GetReturnProperty();
			if (ReturnProp != NULL)
			{
				const bool bHasReturnParam = Function->ReturnValueOffset != MAX_uint16;
				uint8* ReturnValueAdress = bHasReturnParam ? (Stack.Locals + Function->ReturnValueOffset) : nullptr;
				if (ReturnValueAdress)
					FMemory::Memcpy(RESULT_PARAM, ReturnValueAdress, ReturnProp->ArrayDim * ReturnProp->ElementSize);
			}

			bool bHasAnyOutParams = false;
			if (Function && Function->HasAnyFunctionFlags(FUNC_HasOutParms))
			{
				// Iterate over input parameters
				for (TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
				{
					// This is 'out ref'!
					if ((It->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
					{
						bHasAnyOutParams = true;
						break;
					}
				}
			}

			if (bHasAnyOutParams)
			{
				auto OutParm = Stack.OutParms;

				// Iterate over parameters again
				for (TFieldIterator<UProperty> It(Function); It; ++It)
				{
					UProperty* Param = *It;

					auto PropertyFlags = Param->GetPropertyFlags();
					if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
					{
						auto Property = OutParm->Property;
						if (Property != nullptr)
						{
							auto ValueAddress = Property->ContainerPtrToValuePtr<uint8>(Stack.Locals);
							FMemory::Memcpy(OutParm->PropAddr, ValueAddress, Property->ArrayDim * Property->ElementSize);
						}
					}

					if (PropertyFlags & CPF_OutParm)
						OutParm = OutParm->NextOutParm;
				}
			}
		}
	};

	bool bIsVMVirtual = Function->GetSuperFunction() && Cast<UBlueprintGeneratedClass>(Function->GetSuperFunction()->GetOuter()) != nullptr;
	if (bIsVMVirtual && *Stack.Code != EX_EndFunctionParms)
	{
		uint8* Frame = NULL;
/*#if USE_UBER_GRAPH_PERSISTENT_FRAME
		Frame = GetClass()->GetPersistentUberGraphFrame(this, Function);
#endif*/
		const bool bUsePersistentFrame = (NULL != Frame);
		if (!bUsePersistentFrame)
		{
			Frame = (uint8*)FMemory_Alloca(Function->PropertiesSize);
			FMemory::Memzero(Frame, Function->PropertiesSize);
		}
		FFrame NewStack(this, Function, Frame, &Stack, Function->Children);
		FOutParmRec** LastOut = &NewStack.OutParms;
		UProperty* Property;

		// Check to see if we need to handle a return value for this function.  We need to handle this first, because order of return parameters isn't always first.
		if (Function->HasAnyFunctionFlags(FUNC_HasOutParms))
		{
			// Iterate over the function parameters, searching for the ReturnValue
			for (TFieldIterator<UProperty> ParmIt(Function); ParmIt; ++ParmIt)
			{
				Property = *ParmIt;
				if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					CA_SUPPRESS(6263)
					FOutParmRec* RetVal = (FOutParmRec*)FMemory_Alloca(sizeof(FOutParmRec));

					// Our context should be that we're in a variable assignment to the return value, so ensure that we have a valid property to return to
					check(RESULT_PARAM != NULL);
					RetVal->PropAddr = (uint8*)RESULT_PARAM;
					RetVal->Property = Property;
					NewStack.OutParms = RetVal;

					// A function can only have one return value, so we can stop searching
					break;
				}
			}
		}

		for (Property = (UProperty*)Function->Children; *Stack.Code != EX_EndFunctionParms; Property = (UProperty*)Property->Next)
		{
			checkfSlow(Property, TEXT("NULL Property in Function %s"), *Function->GetPathName());

			Stack.MostRecentPropertyAddress = NULL;

			// Skip the return parameter case, as we've already handled it above
			const bool bIsReturnParam = ((Property->PropertyFlags & CPF_ReturnParm) != 0);
			if (bIsReturnParam)
			{
				continue;
			}

			if (Property->PropertyFlags & CPF_OutParm)
			{
				// evaluate the expression for this parameter, which sets Stack.MostRecentPropertyAddress to the address of the property accessed
				Stack.Step(Stack.Object, NULL);

				CA_SUPPRESS(6263)
				FOutParmRec* Out = (FOutParmRec*)FMemory_Alloca(sizeof(FOutParmRec));
				// set the address and property in the out param info
				// warning: Stack.MostRecentPropertyAddress could be NULL for optional out parameters
				// if that's the case, we use the extra memory allocated for the out param in the function's locals
				// so there's always a valid address
				ensure(Stack.MostRecentPropertyAddress); // possible problem - output param values on local stack are neither initialized nor cleaned.
				Out->PropAddr = (Stack.MostRecentPropertyAddress != NULL) ? Stack.MostRecentPropertyAddress : Property->ContainerPtrToValuePtr<uint8>(NewStack.Locals);
				Out->Property = Property;

				// add the new out param info to the stack frame's linked list
				if (*LastOut)
				{
					(*LastOut)->NextOutParm = Out;
					LastOut = &(*LastOut)->NextOutParm;
				}
				else
				{
					*LastOut = Out;
				}
			}
			else
			{
				// copy the result of the expression for this parameter into the appropriate part of the local variable space
				uint8* Param = Property->ContainerPtrToValuePtr<uint8>(NewStack.Locals);
				checkSlow(Param);

				Property->InitializeValue_InContainer(NewStack.Locals);

				Stack.Step(Stack.Object, Param);
			}
		}
		Stack.Code++;
#if UE_BUILD_DEBUG
		// set the next pointer of the last item to NULL so we'll properly assert if something goes wrong
		if (*LastOut)
		{
			(*LastOut)->NextOutParm = NULL;
		}
#endif

		if (!bUsePersistentFrame)
		{
			// Initialize any local struct properties with defaults
			for (UProperty* LocalProp = Function->FirstPropertyToInit; LocalProp != NULL; LocalProp = (UProperty*)LocalProp->Next)
			{
				LocalProp->InitializeValue_InContainer(NewStack.Locals);
			}
		}

		const bool bIsValidFunction = (Function->FunctionFlags & FUNC_Native) || (Function->Script.Num() > 0);

		// Execute the code.
		if (bIsValidFunction)
		{
			ProcessInternal(NewStack, RESULT_PARAM);
		}

		if (!bUsePersistentFrame)
		{
			// destruct properties on the stack, except for out params since we know we didn't use that memory
			for (UProperty* Destruct = Function->DestructorLink; Destruct; Destruct = Destruct->DestructorLinkNext)
			{
				if (!Destruct->HasAnyPropertyFlags(CPF_OutParm))
				{
					Destruct->DestroyValue_InContainer(NewStack.Locals);
				}
			}
		}
	}
	else
	{
		P_FINISH;
		ProcessInternal(Stack, RESULT_PARAM);
	}
}

namespace {
	UClass* CurrentClassUnderConstruction = nullptr;
	void CallClassConstructor(UClass* Class, const FObjectInitializer& ObjectInitializer)
	{
		if (Cast<UJavascriptGeneratedClass_Native>(Class) || Cast<UJavascriptGeneratedClass>(Class))
		{
			CurrentClassUnderConstruction = Class;
		}
		Class->ClassConstructor(ObjectInitializer);
		CurrentClassUnderConstruction = nullptr;
	};
}

class FJavascriptContextImplementation : public FJavascriptContext
{
	friend class UJavascriptContext;

	TArray<const FObjectInitializer*> ObjectInitializerStack;

	virtual const FObjectInitializer* GetObjectInitializer() override
	{
		return ObjectInitializerStack.Num() ? ObjectInitializerStack.Last(0) : nullptr;
	}

	int32 Magic{ MagicNumber };

	Persistent<JsContextRef> context_;
	IJavascriptDebugger* debugger{ nullptr };
	IJavascriptInspector* inspector{ nullptr };

	TMap<FString, UObject*> WKOs;

public:
	JsContextRef context() const { return context_.Get(); }
	bool IsValid() const { return Magic == MagicNumber; }

public:

	TMap<FString, Persistent<JsValueRef>> Modules;
	TArray<FString>& Paths;

	void SetAsDebugContext(int32 InPort)
	{
		if (debugger) return;

		FContextScope context_scope(context());

		debugger = IJavascriptDebugger::Create(InPort, context());
	}

	bool IsDebugContext() const
	{
		return debugger != nullptr;
	}

	void ResetAsDebugContext()
	{
		if (debugger)
		{
			FContextScope context_scope(context());

			debugger->Destroy();
			debugger = nullptr;
		}
	}

	void CreateInspector(int32 Port)
	{
		if (inspector) return;

		FContextScope context_scope(context());

		inspector = IJavascriptInspector::Create(Port, context());
	}

	void DestroyInspector()
	{
		if (inspector)
		{
			FContextScope context_scope(context());

			inspector->Destroy();
			inspector = nullptr;
		}
	}

	FJavascriptContextImplementation(JsRuntimeHandle InRuntime, TArray<FString>& InPaths)
		: Paths(InPaths)
	{
		check(InRuntime != JS_INVALID_RUNTIME_HANDLE);

		JsContextRef context = JS_INVALID_REFERENCE;
		JsCreateContext(InRuntime, &context);
		//context->SetAlignedPointerInEmbedderData(kContextEmbedderDataIndex, this);
		// Todo: setup globaltemplate

		context_.Reset(context);

		ExposeGlobals();

		Paths = IV8::Get().GetGlobalScriptSearchPaths();
	}

	~FJavascriptContextImplementation()
	{
		PurgeModules();

		ReleaseAllPersistentHandles();

		ResetAsDebugContext();
		DestroyInspector();

		context_.Reset();
	}

	void ReleaseAllPersistentHandles()
	{
		// Release all object instances
		ObjectToObjectMap.Empty();

		// Release all struct instances
		MemoryToObjectMap.Empty();
	}

	void ExposeGlobals()
	{
		FContextScope context_scope(context());

		ExposeRequire();
		ExportUnrealEngineClasses();
		ExportUnrealEngineStructs();

		ExposeMemory2();
	}

	void PurgeModules()
	{
		Modules.Empty();
	}

	void ExportUnrealEngineClasses()
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Context = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Context->context());

			FString Name = chakra::StringFromChakra(arguments[1]);
			JsValueRef Opts = arguments[2];
			UObject* Outer = chakra::UObjectFromChakra(chakra::GetProperty(Opts, "Outer"));
			UClass* ParentClass = chakra::UClassFromChakra(chakra::GetProperty(Opts, "Parent"));
			Outer = Outer ? Outer : GetTransientPackage();
			ParentClass = ParentClass ? ParentClass : UObject::StaticClass();

			UBlueprintGeneratedClass* Class = nullptr;
			if (Cast<UBlueprintGeneratedClass>(ParentClass))
			{
				auto Klass = NewObject<UJavascriptGeneratedClass>(Outer, *Name, RF_Public);
				Klass->JavascriptContext = Context->AsShared();
				Class = Klass;
			}
			else
			{
				auto Klass = NewObject<UJavascriptGeneratedClass_Native>(Outer, *Name, RF_Public);
				Klass->JavascriptContext = Context->AsShared();
				Class = Klass;

				// This flag is necessary for proper initialization
				Class->ClassFlags |= CLASS_Native;
			}

			// Create a blueprint
			auto Blueprint = NewObject<UBlueprint>(Outer);
			Blueprint->GeneratedClass = Class;
			Class->ClassGeneratedBy = Blueprint;

			auto ClassConstructor = [](const FObjectInitializer& ObjectInitializer){
				auto Class = static_cast<UBlueprintGeneratedClass*>(CurrentClassUnderConstruction ? CurrentClassUnderConstruction : ObjectInitializer.GetClass());
				CurrentClassUnderConstruction = nullptr;

				FJavascriptContextImplementation* Context = nullptr;

				if (auto Klass = Cast<UJavascriptGeneratedClass_Native>(Class))
				{
					if (Klass->JavascriptContext.IsValid())
					{
						Context = static_cast<FJavascriptContextImplementation*>(Klass->JavascriptContext.Pin().Get());
					}
				}
				else if (auto Klass = Cast<UJavascriptGeneratedClass>(Class))
				{
					if (Klass->JavascriptContext.IsValid())
					{
						Context = static_cast<FJavascriptContextImplementation*>(Klass->JavascriptContext.Pin().Get());
					}
				}

				if (Context)
				{
					auto Object = ObjectInitializer.GetObj();

					FContextScope context_scope(Context->context());

					JsValueRef Holder = Context->ExportObject(Class);

					JsValueRef proxy = chakra::GetProperty(Holder, "proxy");
					if (chakra::IsEmpty(proxy) || !chakra::IsObject(proxy))
					{
						chakra::Throw(TEXT("Invalid proxy : construct class"));
						return;
					}

					Context->ObjectInitializerStack.Add(&ObjectInitializer);

					JsValueRef This = Context->ExportObject(Object);

					JsContextRef context = Context->context();

					{
						JsValueRef func = chakra::GetProperty(proxy, "prector");

						if (chakra::IsFunction(func))
						{
							CallJavascriptFunction(context, This, nullptr, func, nullptr);
						}
					}

					CallClassConstructor(Class->GetSuperClass(), ObjectInitializer);

					{
						JsValueRef func = chakra::GetProperty(proxy, "ctor");

						if (chakra::IsFunction(func))
						{
							CallJavascriptFunction(context, This, nullptr, func, nullptr);
						}
					}

					Context->ObjectInitializerStack.RemoveAt(Context->ObjectInitializerStack.Num() - 1, 1);
				}
				else
				{
					CallClassConstructor(Class->GetSuperClass(), ObjectInitializer);
				}
			};

			Class->ClassConstructor = ClassConstructor;

			// Set properties we need to regenerate the class with
			Class->PropertyLink = ParentClass->PropertyLink;
			Class->ClassWithin = ParentClass->ClassWithin;
			Class->ClassConfigName = ParentClass->ClassConfigName;

			Class->SetSuperStruct(ParentClass);
			Class->ClassFlags |= (ParentClass->ClassFlags & (CLASS_Inherit | CLASS_ScriptInherit | CLASS_CompiledFromBlueprint));
			Class->ClassCastFlags |= ParentClass->ClassCastFlags;

			auto AddFunction = [&](FName NewFunctionName, JsValueRef TheFunction) -> bool {
				UFunction* ParentFunction = ParentClass->FindFunctionByName(NewFunctionName);

				UJavascriptGeneratedFunction* Function{ nullptr };

				auto MakeFunction = [&]() {
					Function = NewObject<UJavascriptGeneratedFunction>(Class, NewFunctionName, RF_Public);
					Function->JavascriptContext = Context->AsShared();
					//Function->RepOffset = MAX_uint16;
					Function->ReturnValueOffset = MAX_uint16;
					Function->FirstPropertyToInit = NULL;

					Function->Script.Add(EX_EndFunctionParms);
				};

				// Overridden function should have its parent function
				if (ParentFunction)
				{
					MakeFunction();

					Function->SetSuperStruct(ParentFunction);

					auto InitializeProperties = [](UFunction* Function, UFunction* ParentFunction) {
						UField** Storage = &Function->Children;
						UProperty** PropertyStorage = &Function->PropertyLink;

						for (TFieldIterator<UProperty> PropIt(ParentFunction, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
						{
							UProperty* Property = *PropIt;
							if (Property->HasAnyPropertyFlags(CPF_Parm))
							{
								UProperty* NewProperty = DuplicateProperty(Function, Property, Property->GetFName());

								*Storage = NewProperty;
								Storage = &NewProperty->Next;

								*PropertyStorage = NewProperty;
								PropertyStorage = &NewProperty->PropertyLinkNext;
							}
						}
					};

					if (ParentFunction)
					{
						InitializeProperties(Function, ParentFunction);
					}
				}
				else
				{
					JsValueRef FunctionObj = TheFunction;
					auto IsUFUNCTION = chakra::GetProperty(FunctionObj, "IsUFUNCTION");
					if (chakra::IsEmpty(IsUFUNCTION) || !chakra::BoolEvaluate(IsUFUNCTION))
					{
						return false;
					}

					MakeFunction();

					JsValueRef Decorators = chakra::GetProperty(FunctionObj, "Decorators");
					if (!chakra::IsEmpty(Decorators) && chakra::IsArray(Decorators))
					{
						SetFunctionFlags(Function, StringArrayFromChakra(Decorators));
					}

					auto InitializeProperties = [](UFunction* Function, JsValueRef Signature) {
						UField** Storage = &Function->Children;
						UProperty** PropertyStorage = &Function->PropertyLink;

						if (!chakra::IsEmpty(Signature) && chakra::IsArray(Signature))
						{
							JsValueRef arr = Signature;
							int len = chakra::Length(arr);

							for (decltype(len) Index = 0; Index < len; ++Index)
							{
								JsValueRef PropertyDecl = chakra::GetIndex(arr, Index);

								UProperty* NewProperty = CreatePropertyFromDecl(Function, PropertyDecl);

								if (NewProperty)
								{
									NewProperty->SetPropertyFlags(CPF_Parm);

									*Storage = NewProperty;
									Storage = &NewProperty->Next;

									*PropertyStorage = NewProperty;
									PropertyStorage = &NewProperty->PropertyLinkNext;
								}
							}
						}
					};

					JsValueRef Signature = chakra::GetProperty(FunctionObj, "Signature");

					InitializeProperties(Function, Signature);
				}

				auto FinalizeFunction = [](UFunction* Function) {
					Function->Bind();
					Function->StaticLink(true);

					Function->FunctionFlags |= FUNC_Native;

					for (TFieldIterator<UProperty> PropIt(Function, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
					{
						UProperty* Property = *PropIt;
						if (Property->HasAnyPropertyFlags(CPF_Parm))
						{
							++Function->NumParms;
							Function->ParmsSize = Property->GetOffset_ForUFunction() + Property->GetSize();

							if (Property->HasAnyPropertyFlags(CPF_OutParm))
							{
								Function->FunctionFlags |= FUNC_HasOutParms;
							}

							if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
							{
								Function->ReturnValueOffset = Property->GetOffset_ForUFunction();

								if (!Property->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
								{
									Property->DestructorLinkNext = Function->DestructorLink;
									Function->DestructorLink = Property;
								}
							}
						}
						else
						{
							if (!Property->HasAnyPropertyFlags(CPF_ZeroConstructor))
							{
								Function->FirstPropertyToInit = Property;
								Function->FunctionFlags |= FUNC_HasDefaults;
								break;
							}
						}
					}
				};

				FinalizeFunction(Function);

				Function->SetNativeFunc((Native)&UJavascriptGeneratedFunction::Thunk);

				Function->Next = Class->Children;
				Class->Children = Function;

				// Add the function to it's owner class function name -> function map
				Class->AddFunctionToFunctionMap(Function, Function->GetFName());

				return true;
			};

			JsValueRef ClassFlags = chakra::GetProperty(Opts, "ClassFlags");
			if (!chakra::IsEmpty(ClassFlags) && chakra::IsArray(ClassFlags))
			{
				SetClassFlags(Class, StringArrayFromChakra(ClassFlags));
			}

			JsValueRef PropertyDecls = chakra::GetProperty(Opts, "Properties");
			if (!chakra::IsEmpty(PropertyDecls) && chakra::IsArray(PropertyDecls))
			{
				JsValueRef arr = PropertyDecls;
				int len = chakra::Length(arr);

				for (decltype(len) Index = 0; Index < len; ++Index)
				{
					JsValueRef PropertyDecl = chakra::GetIndex(arr, len - Index - 1);
					if (chakra::IsObject(PropertyDecl))
					{
						auto Property = CreatePropertyFromDecl(Class, PropertyDecl);

						if (Property)
						{
							Class->AddCppProperty(Property);

							if (Property->HasAnyPropertyFlags(CPF_Net))
							{
								Class->NumReplicatedProperties++;
							}
						}
					}
				}
			}

			JsValueRef Functions = chakra::GetProperty(Opts, "Functions");
			TMap<FString, JsValueRef> Others;
			if (!chakra::IsEmpty(Functions) && chakra::IsObject(Functions))
			{
				JsValueRef FuncMap = Functions;
				JsValueRef Keys = JS_INVALID_REFERENCE;
				JsGetOwnPropertyNames(FuncMap, &Keys);

				int NumKeys = chakra::Length(Keys);

				for (decltype(NumKeys) Index = 0; Index < NumKeys; ++Index)
				{
					JsValueRef Name = chakra::GetIndex(Keys, Index);
					FString UName = chakra::StringFromChakra(Name);
					JsValueRef Function = chakra::GetProperty(FuncMap, chakra::StringFromChakra(Name));

					if (!chakra::IsFunction(Function)) continue;

					if (UName != TEXT("prector") && UName != TEXT("ctor") && UName != TEXT("constructor"))
					{
						if (!AddFunction(*UName, Function))
						{
							Others.Add(UName, Function);
						}
					}
				}
			}

			Class->Bind();
			Class->StaticLink(true);

			{
				JsFunctionRef FinalClass = Context->ExportClass(Class,false);
				JsValueRef ProtoType = chakra::GetPrototype(FinalClass);

				for (auto It = Others.CreateIterator(); It; ++It)
				{
					chakra::SetProperty(ProtoType, It.Key(), It.Value());
				}

				Context->RegisterClass(Class, FinalClass);
			}

			JsFunctionRef FinalClass = Context->ExportObject(Class);
			chakra::SetProperty(FinalClass, "proxy", Functions);

			// Make sure CDO is ready for use
			Class->GetDefaultObject();

#if ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 12
			// Assemble reference token stream for garbage collection/ RTGC.
			if (!Class->HasAnyClassFlags(CLASS_TokenStreamAssembled))
			{
				Class->AssembleReferenceTokenStream();
			}
#endif
			return FinalClass;
		};

		FContextScope scope(context());
		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);

		chakra::SetProperty(global, "CreateClass", chakra::FunctionTemplate(fn, this));
	}

	void ExportUnrealEngineStructs()
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Context = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Context->context());

			FString Name = chakra::StringFromChakra(arguments[1]);
			JsValueRef Opts = arguments[2];
			UObject* Outer = chakra::UObjectFromChakra(chakra::GetProperty(Opts, "Outer"));
			UScriptStruct* ParentStruct = reinterpret_cast<UScriptStruct*>(chakra::UClassFromChakra(chakra::GetProperty(Opts, "Parent")));
			Outer = Outer ? Outer : GetTransientPackage();

			UScriptStruct* Struct = NewObject<UScriptStruct>(Outer, *Name, RF_Public);

			// Set properties we need to regenerate the class with
			if (ParentStruct)
			{
				Struct->PropertyLink = ParentStruct->PropertyLink;
				Struct->SetSuperStruct(ParentStruct);
				Struct->StructFlags = (EStructFlags)(ParentStruct->StructFlags & STRUCT_Inherit);
			}

			JsValueRef StructFlags = chakra::GetProperty(Opts, "StructFlags");
			if (!chakra::IsEmpty(StructFlags) && chakra::IsArray(StructFlags))
			{
				SetStructFlags(Struct, StringArrayFromChakra(StructFlags));
			}

			JsValueRef PropertyDecls = chakra::GetProperty(Opts, "Properties");
			if (!chakra::IsEmpty(PropertyDecls) && chakra::IsArray(PropertyDecls))
			{
				JsValueRef arr = PropertyDecls;
				int len = chakra::Length(arr);

				for (decltype(len) Index = 0; Index < len; ++Index)
				{
					auto PropertyDecl = chakra::GetIndex(arr, len - Index - 1);
					if (chakra::IsObject(PropertyDecl))
					{
						auto Property = CreatePropertyFromDecl(Struct, PropertyDecl);

						if (Property)
						{
							Struct->AddCppProperty(Property);
						}
					}
				}
			}

			Struct->Bind();
			Struct->StaticLink(true);

			auto FinalClass = Context->ExportObject(Struct);

			return FinalClass;
		};

		FContextScope scope(context());
		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);

		chakra::SetProperty(global, "CreateStruct", chakra::FunctionTemplate(fn, this));
	}

	void ExposeRequire()
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (argumentCount != 2 || !chakra::IsString(arguments[1]))
			{
				return chakra::Undefined();
			}

			FJavascriptContextImplementation* Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());

			FString required_module = chakra::StringFromChakra(arguments[1]);

			bool found = false;

			JsValueRef returnValue = JS_INVALID_REFERENCE;
			auto inner = [&](const FString& script_path)
			{
				FString full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*script_path);
#if PLATFORM_WINDOWS
				full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
				auto it = Self->Modules.Find(full_path);
				if (it)
				{
					returnValue = it->Get();
					return true;
				}

				FString Text;
				if (FFileHelper::LoadFileToString(Text, *script_path))
				{
					Text = FString::Printf(TEXT("(function (global, __filename, __dirname) { var module = { exports : {}, filename : __filename }, exports = module.exports; (function () { %s\n })()\n;return module.exports;}(this,'%s', '%s'));"), *Text, *script_path, *FPaths::GetPath(script_path));
					JsValueRef exports = Self->RunScript(full_path, Text, 0);
					if (chakra::IsEmpty(exports))
					{
						UE_LOG(Javascript, Log, TEXT("Invalid script for require"));
					}
					Self->Modules.Add(full_path, exports);
					returnValue = exports;
					found = true;
					return true;
				}

				return false;
			};

			auto inner_maybejs = [&](const FString& script_path)
			{
				if (!script_path.EndsWith(TEXT(".js")))
				{
					return inner(script_path + TEXT(".js"));
				}
				else
				{
					return inner(script_path);
				}
			};

			auto inner_package_json = [&](const FString& script_path)
			{
				FString Text;
				if (FFileHelper::LoadFileToString(Text, *(script_path / TEXT("package.json"))))
				{
					Text = FString::Printf(TEXT("(function (json) {return json.main;})(%s);"), *Text);
					auto full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*script_path);
#if PLATFORM_WINDOWS
					full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
					JsValueRef exports = Self->RunScript(full_path, Text, 0);
					if (chakra::IsEmpty(exports) || !chakra::IsString(exports))
					{
						return false;
					}
					else
					{
						return inner_maybejs(script_path / chakra::StringFromChakra(exports));
					}
				}

				return false;
			};

			auto inner_json = [&](const FString& script_path)
			{
				auto full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*script_path);
#if PLATFORM_WINDOWS
				full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
				auto it = Self->Modules.Find(full_path);
				if (it)
				{
					returnValue = it->Get();
					found = true;
					return true;
				}

				FString Text;
				if (FFileHelper::LoadFileToString(Text, *script_path))
				{
					Text = FString::Printf(TEXT("(function (json) {return json;})(%s);"), *Text);

#if PLATFORM_WINDOWS
					full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
					JsValueRef exports = Self->RunScript(full_path, Text, 0);
					if (chakra::IsEmpty(exports) || !chakra::IsObject(exports))
					{
						return false;
					}
					else
					{
						Self->Modules.Add(full_path, exports);
						returnValue = exports;
						found = true;
						return true;
					}
				}

				return false;
			};

			auto inner2 = [&](FString base_path)
			{
				if (!FPaths::DirectoryExists(base_path)) return false;

				auto script_path = base_path / required_module;
				if (script_path.EndsWith(TEXT(".js")))
				{
					if (inner(script_path)) return true;
				}
				else
				{
					if (inner(script_path + TEXT(".js"))) return true;
				}
				if (script_path.EndsWith(TEXT(".json")))
				{
					if (inner_json(script_path)) return true;
				}
				else
				{
					if (inner_json(script_path + TEXT(".json"))) return true;
				}

				if (inner(script_path / TEXT("index.js"))) return true;
				if (inner_package_json(script_path)) return true;

				return false;
			};

			auto load_module_paths = [&](FString base_path)
			{
				TArray<FString> Dirs;
				TArray<FString> Parsed;
				base_path.ParseIntoArray(Parsed, TEXT("/"));
				auto PartCount = Parsed.Num();
				while (PartCount > 0) {
					if (Parsed[PartCount-1].Equals(TEXT("node_modules")))
					{
						PartCount--;
						continue;
					}
					else
					{
						TArray<FString> Parts;
						for (int i = 0; i < PartCount; i++) Parts.Add(Parsed[i]);
						FString Dir = FString::Join(Parts, TEXT("/"));
						Dirs.Add(Dir);
					}
					PartCount--;
				}

				return Dirs;
			};

			auto current_script_path = FPaths::GetPath(chakra::StringFromChakra(StackTrace::CurrentStackTrace(isolate, 1, StackTrace::kScriptName)->GetFrame(0)->GetScriptName()));
			current_script_path = URLToLocalPath(current_script_path);

			if (!(required_module[0] == '.' && inner2(current_script_path)))
			{
				for (const auto& path : load_module_paths(current_script_path))
				{
					if (inner2(path)) break;
					if (inner2(path / TEXT("node_modules"))) break;
				}

				for (const auto& path : Self->Paths)
				{
					if (inner2(path)) break;
					if (inner2(path / TEXT("node_modules"))) break;
				}
			}

			if (!found)
			{
				return chakra::Undefined();
			}

			return returnValue;
		};

		auto fn2 = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());
			Self->PurgeModules();

			return chakra::Undefined();
		};

		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);

		chakra::SetProperty(global, "require", chakra::FunctionTemplate(fn, this));
		chakra::SetProperty(global, "purge_modules", chakra::FunctionTemplate(fn2, this));

		auto getter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());

			JsValueRef out = JS_INVALID_REFERENCE;
			JsCreateObject(&out);

			for (auto it = Self->Modules.CreateConstIterator(); it; ++it)
			{
				const FString& name = it.Key();
				const JsValueRef& module = it.Value().Get();

				FString FullPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*name);
				chakra::SetProperty(out, name, chakra::String(FullPath));
			}

			return out;
		};

		chakra::FPropertyDescriptor desc;
		desc.Getter = chakra::FunctionTemplate(getter, this);
		chakra::SetAccessor(global, "modules", desc);
	}

	void ExposeMemory2()
	{
		FContextScope scope(context());
		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);

		JsValueRef Template = chakra::FunctionTemplate();

		auto add_fn = [&](const char* name, JsNativeFunction fn) {
			chakra::SetProperty(global, name, chakra::FunctionTemplate(fn, this));
		};

		add_fn("$memaccess", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			auto Context = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			if (argumentCount == 4 && chakra::IsFunction(arguments[3]))
			{
				auto Instance = FStructMemoryInstance::FromChakra(arguments[1]);
				JsValueRef function = arguments[3];

				// If given value is an instance
				if (Instance)
				{
					auto Memory = Instance->GetMemory();
					auto GivenStruct = Instance->Struct;

					if (Memory && Instance->Struct->IsChildOf(FJavascriptRawAccess::StaticStruct()))
					{
						auto Source = reinterpret_cast<FJavascriptRawAccess*>(Memory);

						FString Name = chakra::StringFromChakra(arguments[2]);

						for (auto Index = 0; Index < Source->GetNumData(); ++Index)
						{
							if (Source->GetDataName(Index).ToString() == Name)
							{
								auto ProxyStruct = Source->GetScriptStruct(Index);
								auto Proxy = Source->GetData(Index);
								JsValueRef argv[2] = { arguments[0] };
								if (ProxyStruct && Proxy)
								{
									argv[1] = Context->ExportStructInstance(ProxyStruct, (uint8*)Proxy, FStructMemoryPropertyOwner(Instance));
								}


								JsValueRef returnValue = JS_INVALID_REFERENCE;
								JsCallFunction(function, argv, 2, &returnValue);
								return chakra::Undefined();
							}
						}
					}
				}
			}

			if (argumentCount == 3 && chakra::IsFunction(arguments[2]))
			{
				auto Instance = FStructMemoryInstance::FromChakra(arguments[1]);
				JsValueRef function = arguments[2];

				// If given value is an instance
				if (Instance)
				{
					auto Memory = Instance->GetMemory();
					auto GivenStruct = Instance->Struct;
					if (Memory && Instance->Struct->IsChildOf(FJavascriptMemoryStruct::StaticStruct()))
					{
						auto Source = reinterpret_cast<FJavascriptMemoryStruct*>(Memory);

						JsValueRef argv[2] = { arguments[0] };

						auto Dimension = Source->GetDimension();
						auto Indices = (int32*)FMemory_Alloca(sizeof(int32) * Dimension);
						if (Dimension == 1)
						{
							JsValueRef ab = JS_INVALID_REFERENCE;
							JsCreateExternalArrayBuffer(Source->GetMemory(nullptr), Source->GetSize(0), nullptr, nullptr, &ab);
							argv[1] = ab;

							JsValueRef returnValue = JS_INVALID_REFERENCE;
							JsCallFunction(function, argv, 2, &returnValue);
							return chakra::Undefined();
						}
						else if (Dimension == 2)
						{
							int Outer = Source->GetSize(0);
							int Inner = Source->GetSize(1);
							JsValueRef out_arr = JS_INVALID_REFERENCE;
							JsCreateArray(Outer, &out_arr);

							argv[1] = out_arr;
							for (auto Index = 0; Index < Outer; ++Index)
							{
								Indices[0] = Index;
								JsValueRef ab = JS_INVALID_REFERENCE;
								JsCreateExternalArrayBuffer(Source->GetMemory(Indices), Inner, nullptr, nullptr, &ab);
								chakra::SetIndex(out_arr, Index, ab);
							}

							JsValueRef returnValue = JS_INVALID_REFERENCE;
							JsCallFunction(function, argv, 2, &returnValue);
							return chakra::Undefined();
						}
					}
					if (Memory && Instance->Struct->IsChildOf(FJavascriptRawAccess::StaticStruct()))
					{
						auto Source = reinterpret_cast<FJavascriptRawAccess*>(Memory);

						JsValueRef argv[2] = { arguments[0] };

						auto ProxyStruct = Source->GetScriptStruct(0);
						auto Proxy = Source->GetData(0);
						if (ProxyStruct && Proxy)
						{
							argv[1] = Context->ExportStructInstance(ProxyStruct, (uint8*)Proxy, FStructMemoryPropertyOwner(Instance));
						}

						JsValueRef returnValue = JS_INVALID_REFERENCE;
						JsCallFunction(function, argv, 2, &returnValue);
						return chakra::Undefined();
					}
				}
			}
			chakra::Throw(TEXT("memory.fork requires JavascriptMemoryObject"));
			return chakra::Undefined();
		});
	}

	FString GetScriptFileFullPath(const FString& Filename)
	{
		for (auto Path : Paths)
		{
			auto FullPath = Path / Filename;
			auto Size = IFileManager::Get().FileSize(*FullPath);
			if (Size != INDEX_NONE)
			{
				return IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FullPath);
			}
		}
		return Filename;
	}

	FString ReadScriptFile(const FString& Filename)
	{
		auto Path = GetScriptFileFullPath(Filename);

		FString Text;

		FFileHelper::LoadFileToString(Text, *Path);

		return Text;
	}

	JsValueRef RunFile(const FString& Filename)
	{
		FString Script = ReadScriptFile(Filename);

		FString ScriptPath = GetScriptFileFullPath(Filename);
		FString Text = FString::Printf(TEXT("(function (global,__filename,__dirname) { %s\n;}(this,'%s','%s'));"), *Script, *ScriptPath, *FPaths::GetPath(ScriptPath));
		return RunScript(ScriptPath, Text, 0);
	}

	void Public_RunFile(const FString& Filename)
	{
		RunFile(Filename);
	}

	FString Public_RunScript(const FString& Script, bool bOutput = true)
	{
		FContextScope context_scope(context());

		JsValueRef ret = RunScript(TEXT("(inline)"), Script);
		FString str = chakra::IsEmpty(ret) ? TEXT("(empty)") : chakra::StringFromChakra(ret);

		if (bOutput && !chakra::IsEmpty(ret))
		{
			UE_LOG(Javascript, Log, TEXT("%s"), *str);
		}
		return str;
	}

	void RequestV8GarbageCollection()
	{
		// @todo: using 'ForTesting' function
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsErrorCode err = JsGetRuntime(context(), &runtime);
		check(err == JsNoError);

		JsCollectGarbage(runtime);
	}

	// Should be guarded with proper handle scope
	JsValueRef RunScript(const FString& Filename, const FString& Script, int line_offset = 0)
	{
		FContextScope context_scope(context());

		FString Path = Filename;
#if PLATFORM_WINDOWS
		// HACK for Visual Studio Code
		if (Path.Len() && Path[1] == ':')
		{
			Path = Path.Mid(0, 1).ToLower() + Path.Mid(1);
		}
#endif
		JsValueRef source = chakra::String(Script);
		JsValueRef path = chakra::String(LocalPathToURL(Path));
		JsValueRef returnValue = JS_INVALID_REFERENCE;
		JsErrorCode err = JsRunScript(*Script, 0, *LocalPathToURL(Path), &returnValue);
		if (err == JsNoError)
			return returnValue;

		bool hasException = false;
		if (JsHasException(&hasException) == JsNoError && hasException)
		{
			JsValueRef exception = JS_INVALID_REFERENCE;
			JsGetAndClearException(&exception);

			FString strErr = chakra::StringFromChakra(exception);
			UncaughtException(strErr);
			return chakra::Undefined();
		}

		checkf(false, TEXT("failed to execute script! %d"), err);
		return chakra::Undefined();
	}

    void FindPathFile(FString TargetRootPath, FString TargetFileName, TArray<FString>& OutFiles)
    {
        IFileManager::Get().FindFilesRecursive(OutFiles, TargetRootPath.GetCharArray().GetData(), TargetFileName.GetCharArray().GetData(), true, false);
    }

	void Expose(FString RootName, UObject* Object)
	{
		WKOs.Add(RootName, Object);

		auto RootGetter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			return reinterpret_cast<JsValueRef>(callbackState);
		};

		FContextScope context_scope(context());

		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);

		chakra::FPropertyDescriptor desc;
		desc.Getter = chakra::FunctionTemplate(RootGetter, ExportObject(Object));

		chakra::SetAccessor(global, RootName, desc);
	}

	JsValueRef ExportObject(UObject* Object, bool bForce = false) override
	{
		// implement
	}

	virtual JsFunctionRef ExportStruct(UScriptStruct* ScriptStruct) = 0;
	virtual JsFunctionRef ExportClass(UClass* Class, bool bAutoRegister = true) = 0;
	virtual void RegisterClass(UClass* Class, JsFunctionRef Template) = 0;
	virtual JsValueRef GetGlobalTemplate() = 0;
	virtual JsValueRef ExportStructInstance(UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner) = 0;


	bool WriteAliases(const FString& Filename)
	{
#if WITH_EDITOR
		struct TokenWriter
		{
			FString Text;

			TokenWriter& push(const char* something)
			{
				Text.Append(ANSI_TO_TCHAR(something));
				return *this;
			}

			TokenWriter& push(const FString& something)
			{
				Text.Append(something);
				return *this;
			}

			const TCHAR* operator * ()
			{
				return *Text;
			}
		};

		TokenWriter w;

		int DefaultValueId = 0;
		JsValueRef Packer = RunScript(TEXT(""), TEXT("JSON.stringify"));

		auto guard_pre = [&] { w.push("try { "); };
		auto guard_post = [&] { w.push(" } catch (e) {};\n"); };

		for (auto it = ClassToFunctionTemplateMap.CreateConstIterator(); it; ++it)
		{
			const UClass* ClassToExport = it.Key();

			// Skip a generated class
			if (ClassToExport->ClassGeneratedBy) continue;

			auto ClassName = FV8Config::Safeify(ClassToExport->GetName());

			// Function with default value
			{
				// Iterate over all functions
				for (TFieldIterator<UFunction> FuncIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
				{
					auto Function = *FuncIt;

					// Parse all function parameters.
					uint8* Parms = (uint8*)FMemory_Alloca(Function->ParmsSize);
					FMemory::Memzero(Parms, Function->ParmsSize);

					bool bHasDefault = false;
					TArray<FString> Parameters, ParametersWithDefaults;

					for (TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
					{
						auto Property = *It;
						const FName MetadataCppDefaultValueKey(*(FString(TEXT("CPP_Default_")) + Property->GetName()));
						const FString MetadataCppDefaultValue = Function->GetMetaData(MetadataCppDefaultValueKey);
						FString Parameter = Property->GetName();
						FString ParameterWithValue = Parameter;
						if (!MetadataCppDefaultValue.IsEmpty())
						{
							const uint32 ExportFlags = PPF_None;
							auto Buffer = It->ContainerPtrToValuePtr<uint8>(Parms);
							const TCHAR* Result = It->ImportText(*MetadataCppDefaultValue, Buffer, ExportFlags, NULL);
							if (Result)
							{
								bHasDefault = true;
								auto DefaultValue = ReadProperty(Property, Parms, FNoPropertyOwner());
								{
									FContextScope context_scope(context());

									JsValueRef args[] = { Packer, DefaultValue };
									JsValueRef ret = JS_INVALID_REFERENCE;
									JsCallFunction(Packer, args, 2, &ret);
									FString Ret = chakra::StringFromChakra(ret);
									ParameterWithValue = FString::Printf(TEXT("%s = %s"), *Parameter, *Ret);
								}

								It->DestroyValue_InContainer(Parms);
							}
						}
						Parameters.Add(Parameter);
						ParametersWithDefaults.Add(ParameterWithValue);
					}

					if (bHasDefault)
					{
						auto Name = FV8Config::Safeify(Function->GetName());
						auto FnId = FString::Printf(TEXT("fnprepatch_%d"), DefaultValueId++);

						guard_pre();

						w.push("let ");
						w.push(FnId);
						w.push(" = ");
						w.push(ClassName);
						w.push(".prototype.");
						w.push(Name);
						w.push(";");
						w.push(ClassName);
						w.push(".prototype.");
						w.push(Name);
						w.push(" = function (");
						w.push(FString::Join(ParametersWithDefaults, TEXT(", ")));
						w.push(") { return ");
						w.push(FnId);
						w.push(".call(this, ");
						w.push(FString::Join(Parameters, TEXT(", ")));
						w.push(") };");

						guard_post();
					}
				}
			}

			TArray<UFunction*> Functions;
			BlueprintFunctionLibraryMapping.MultiFind(ClassToExport, Functions);

			auto conditional_emit_alias = [&](UFunction* Function, bool is_thunk) {
				auto Alias = FV8Config::GetAlias(Function);
				if (FV8Config::CanExportFunction(ClassToExport, Function) && Alias.Len() > 0)
				{
					guard_pre();

					w.push(ClassName);
					w.push(".prototype.");
					w.push(Alias);
					w.push(" = ");
					w.push(ClassName);
					w.push(".prototype.");
					w.push(FV8Config::Safeify(Function->GetName()));
					w.push(";");

					guard_post();

					if (!is_thunk && Function->FunctionFlags & FUNC_Static)
					{
						guard_pre();

						w.push(ClassName);
						w.push(".");
						w.push(Alias);
						w.push(" = ");
						w.push(ClassName);
						w.push(".");
						w.push(FV8Config::Safeify(Function->GetName()));
						w.push(";");

						guard_post();
					}
				}
			};

			for (auto Function : Functions)
			{
				conditional_emit_alias(Function, true);
			}

			for (TFieldIterator<UFunction> FuncIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
			{
				conditional_emit_alias(*FuncIt, false);
			}
		}

		for (auto it = ScriptStructToFunctionTemplateMap.CreateConstIterator(); it; ++it)
		{
			const UStruct* StructToExport = it.Key();

			auto ClassName = FV8Config::Safeify(StructToExport->GetName());

			TArray<UFunction*> Functions;
			BlueprintFunctionLibraryMapping.MultiFind(StructToExport, Functions);

			auto conditional_emit_alias = [&](UFunction* Function) {
				auto Alias = FV8Config::GetAlias(Function);
				if (Alias.Len() > 0)
				{
					guard_pre();

					w.push(ClassName);
					w.push(".prototype.");
					w.push(Alias);
					w.push(" = ");
					w.push(ClassName);
					w.push(".prototype.");
					w.push(FV8Config::Safeify(Function->GetName()));
					w.push(";");

					guard_post();
				}
			};

			for (auto Function : Functions)
			{
				conditional_emit_alias(Function);
			}
		}

		return FFileHelper::SaveStringToFile(*w, *Filename);
#else
		return false;
#endif
	}

	bool WriteDTS(const FString& Filename, bool bIncludingTooltip)
	{
#if WITH_EDITOR
		TypingGenerator instance(*this);

		instance.no_tooltip = !bIncludingTooltip;

		instance.ExportBootstrap();

		for (auto it = ClassToFunctionTemplateMap.CreateConstIterator(); it; ++it)
		{
			instance.Export(it.Key());
		}

		for (auto pair : WKOs)
		{
			auto k = pair.Key;
			auto v = pair.Value;

			instance.ExportWKO(k, v);
		}

		instance.Finalize();

		return instance.Save(Filename);
#else
		return false;
#endif
	}

	JsValueRef GetProxyFunction(UObject* Object, const TCHAR* Name)
	{
		JsValueRef v8_obj = ExportObject(Object);
		JsValueRef proxy = chakra::GetProperty(v8_obj, "proxy");
		if (chakra::IsEmpty(proxy) || !chakra::IsObject(proxy))
		{
			return chakra::Undefined();
		}

		JsValueRef func = chakra::GetProperty(proxy, Name);
		if (chakra::IsEmpty(func) || !chakra::IsFunction(func))
		{
			return chakra::Undefined();
		}

		return func;
	}

	JsValueRef GetProxyFunction(UObject* Object, UFunction* Function)
	{
		return GetProxyFunction(Object, *FV8Config::Safeify(Function->GetName()));
	}

	bool HasProxyFunction(UObject* Holder, UFunction* Function)
	{
		FContextScope scope(context());

		JsValueRef func = GetProxyFunction(Holder, Function);;
		return !chakra::IsEmpty(func) && chakra::IsFunction(func);
	}

	bool CallProxyFunction(UObject* Holder, UObject* This, UFunction* FunctionToCall, void* Parms)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptProxy);

		FContextScope context_scope(context());

		JsValueRef func = GetProxyFunction(Holder, FunctionToCall);
		if (!chakra::IsEmpty(func) && chakra::IsFunction(func))
		{
			JsValueRef global = JS_INVALID_REFERENCE;
			JsGetGlobalObject(&global);

			CallJavascriptFunction(context(), This ? ExportObject(This) : global, FunctionToCall, func, Parms);

			return true;
		}
		else
		{
			return false;
		}
	}

	// To tell Unreal engine's GC not to destroy these objects!
	virtual void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) override;

	virtual void UncaughtException(const FString& Exception) override
	{
		FContextScope context_scope(context());

		JsValueRef global = JS_INVALID_REFERENCE;
		JsGetGlobalObject(&global);
		JsValueRef func = chakra::GetProperty(global, "$uncaughtException");
		if (!chakra::IsEmpty(func) && chakra::IsFunction(func))
		{
			JsValueRef argv[2] = { global };

			argv[1] = chakra::String(Exception);

			JsValueRef returnValue = JS_INVALID_REFERENCE;
			JsCallFunction(func, argv, 2, &returnValue);
		}
	}
};

FJavascriptContext* FJavascriptContext::Create(TSharedPtr<FJavascriptIsolate> InEnvironment, TArray<FString>& InPaths)
{
	return new FJavascriptContextImplementation(InEnvironment, InPaths);
}

// To tell Unreal engine's GC not to destroy these objects!

inline void FJavascriptContextImplementation::AddReferencedObjects(UObject * InThis, FReferenceCollector & Collector)
{
	RequestV8GarbageCollection();

	// All objects
	for (auto It = ObjectToObjectMap.CreateIterator(); It; ++It)
	{
//		UE_LOG(Javascript, Log, TEXT("JavascriptContext referencing %s %s"), *(It.Key()->GetClass()->GetName()), *(It.Key()->GetName()));
		auto Object = It.Key();
		if (Object->IsPendingKill())
		{
			It.RemoveCurrent();
		}
		else
		{
			Collector.AddReferencedObject(It.Key(), InThis);
		}
	}

	// All structs
	for (auto It = MemoryToObjectMap.CreateIterator(); It; ++It)
	{
		auto Struct = It.Key()->Struct;
		if (Struct->IsPendingKill())
		{
			It.RemoveCurrent();
		}
		else
		{
			Collector.AddReferencedObject(Struct, InThis);
		}
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
