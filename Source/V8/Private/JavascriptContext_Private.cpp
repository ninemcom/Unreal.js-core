
#include "JavascriptContext_Private.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptComponent.h"
#include "JavascriptMemoryObject.h"
#include "FileManager.h"
#include "Config.h"
#include "Delegates.h"
#include "Translator.h"
#include "Exception.h"
#include "IV8.h"
#include "V8PCH.h"
#include "Engine/Blueprint.h"
#include "FileHelper.h"
#include "Paths.h"
#include "Ticker.h"
#include "JavascriptIsolate_Private.h"
#include "PropertyPortFlags.h"
#include "UObjectIterator.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/World.h"
#include "Engine/UserDefinedStruct.h"
#include "ScopedArguments.h"
#include "ScriptMacros.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/StringTableCore.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#include "TypingGenerator.h"
#endif

#include "Helpers.h"
#include "JavascriptGeneratedClass_Native.h"
#include "JavascriptGeneratedClass.h"
#include "JavascriptWidgetGeneratedClass.h"
#include "JavascriptGeneratedFunction.h"
#include "StructMemoryInstance.h"

#include "JavascriptStats.h"

#include "../../Launch/Resources/Version.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

static const int kContextEmbedderDataIndex = 1;
static const int32 MagicNumber = 0x2852abd3;
static const FString URL_FilePrefix(TEXT("file:///"));

// HACK FOR ACCESS PRIVATE MEMBERS
class hack_private_key {};
static UClass* PlaceholderUClass;

// for UStructProperty
struct FStructDummy {};

template<>
FObjectInitializer const& FObjectInitializer::SetDefaultSubobjectClass<hack_private_key>(TCHAR const*SubobjectName) const
{
	AssertIfSubobjectSetupIsNotAllowed(SubobjectName);
	ComponentOverrides.Add(SubobjectName, PlaceholderUClass, *this);
	return *this;
}
// END OF HACKING

template <typename CppType>
struct TStructReader
{
	UScriptStruct* ScriptStruct;

	TStructReader(UScriptStruct* InScriptStruct)
		: ScriptStruct(InScriptStruct)
	{}

	bool Read(JsValueRef Value, CppType& Target) const;
};

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

FString PropertyNameToString(UProperty* Property)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name);
		}
	}
	return name.ToString();
}

bool MatchPropertyName(UProperty* Property, FName NameToMatch)
{
	auto Struct = Property->GetOwnerStruct();
	auto name = Property->GetFName();
	if (Struct)
	{
		if (auto s = Cast<UUserDefinedStruct>(Struct))
		{
			return s->PropertyNameToDisplayName(name) == NameToMatch.ToString();
		}
	}
	return name == NameToMatch;
}

struct FPrivateJavascriptFunction
{
	Persistent<JsContextRef> context;
	Persistent<JsFunctionRef> Function;
};

struct FPrivateJavascriptRef
{
	Persistent<JsValueRef> Object;
};

static JsValueRef GCurrentContents = JS_INVALID_REFERENCE;

int32 FArrayBufferAccessor::GetSize()
{
	if (GCurrentContents == JS_INVALID_REFERENCE)
		return 0;

	ChakraBytePtr dataPtr = nullptr;
	unsigned dataSize = 0;
	JsGetArrayBufferStorage(GCurrentContents, &dataPtr, &dataSize);
	return dataSize;
}

void* FArrayBufferAccessor::GetData()
{
	if (GCurrentContents == JS_INVALID_REFERENCE)
		return nullptr;

	ChakraBytePtr dataPtr = nullptr;
	unsigned dataSize = 0;
	JsGetArrayBufferStorage(GCurrentContents, &dataPtr, &dataSize);
	return dataPtr;
}

void FArrayBufferAccessor::Discard()
{
	GCurrentContents = JS_INVALID_REFERENCE;
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
			if (Type.Split(TEXT("::"), &Left, &Right))
			{
				auto Key = FName(*Name.ToString().Append(TEXT("_Key")));
				TArray<FString> Empty;
				if (auto KeyProperty = CreateProperty(q, Key, Empty, Left, false, false, false))
				{
					q->KeyProp = KeyProperty;
					auto Value = FName(*Name.ToString().Append(TEXT("_Value")));
					if (auto ValueProperty = CreateProperty(q, Value, Decorators, Right, bIsArray, bIsSubclass, false))
					{
						q->ValueProp = ValueProperty;
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

void UJavascriptGeneratedFunction::Thunk(UObject* Obj, FFrame& Stack, RESULT_DECL)
{
	auto Function = static_cast<UJavascriptGeneratedFunction*>(Stack.CurrentNativeFunction);
	auto ProcessInternal = [&](FFrame& Stack, RESULT_DECL)
	{
		if (Function->JavascriptContext.IsValid())
		{
			auto Context = Function->JavascriptContext.Pin();

			FContextScope context_scope(Context->context());

			bool bCallRet = Context->CallProxyFunction(Function->GetOuter(), Obj, Function, Stack.Locals);
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
		FFrame NewStack(Obj, Function, Frame, &Stack, Function->Children);
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
		if (Cast<UJavascriptGeneratedClass_Native>(Class) || Cast<UJavascriptGeneratedClass>(Class) || Cast<UJavascriptWidgetGeneratedClass>(Class))
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

	IDelegateManager* Delegates;

	Persistent<JsValueRef> GlobalTemplate;
	Persistent<JsValueRef> TextTemplate; // for FText conversion

	// Allocator instance should be set for V8's ArrayBuffer's
	//FMallocArrayBufferAllocator AllocatorInstance;

	bool bGCRequested;
	FTickerDelegate TickDelegate;
	FDelegateHandle TickHandle;
	bool RunInGameThread;
	TArray<JsValueRef> PromiseTasks;


	virtual const FObjectInitializer* GetObjectInitializer() override
	{
		return ObjectInitializerStack.Num() ? ObjectInitializerStack.Last(0) : nullptr;
	}

	int32 Magic{ MagicNumber };

	Persistent<JsContextRef> context_;
	IJavascriptDebugger* debugger{ nullptr };
	IJavascriptInspector* inspector{ nullptr };
	bool debugRequested = false;
	int debugPort = 0;

	TMap<FString, UObject*> WKOs;

public:
	JsContextRef context() override { return context_.Get(); }
	bool IsValid() const { return Magic == MagicNumber; }

public:

	struct FObjectPropertyAccessors
	{
		static void* This(JsValueRef self)
		{
			return chakra::UObjectFromChakra(self);
		}

		static JsValueRef Get(FJavascriptContext* ctx, JsValueRef self, UProperty* Property)
		{
			UObject* Object = chakra::UObjectFromChakra(self);

			if (Object->IsValidLowLevelFast())
			{
				FScopeCycleCounterUObject ContextScope(Object);
				FScopeCycleCounterUObject PropertyScope(Property);
				SCOPE_CYCLE_COUNTER(STAT_JavascriptPropertyGet);

				if (auto p = Cast<UMulticastDelegateProperty>(Property))
				{
					return static_cast<FJavascriptContextImplementation*>(ctx)->Delegates->GetProxy(self, Object, p);
				}
				else if (auto p = Cast<UDelegateProperty>(Property))
				{
					return static_cast<FJavascriptContextImplementation*>(ctx)->Delegates->GetProxy(self, Object, p);
				}
				else
				{
					return ctx->ReadProperty(Property, (uint8*)Object, FObjectPropertyOwner(Object));
				}
			}
			else
			{
				return chakra::Undefined();
			}
		}

		//@TODO : Property-type 'routing' is not necessary!
		static void Set(FJavascriptContext* ctx, JsValueRef self, UProperty* Property, JsValueRef value)
		{
			UObject* Object = chakra::UObjectFromChakra(self);

			// Direct access to delegate
			auto SetDelegate = [&](JsValueRef proxy) {
				if (!chakra::IsObject(proxy))
				{
					chakra::Throw(TEXT("Set delegate on invalid instance"));
					return;
				}

				if (!(chakra::IsFunction(value) || chakra::IsNull(value) || chakra::IsArray(value)))
				{
					chakra::Throw(TEXT("Only [function] or null allowed to set delegate"));
					return;
				}

				// call clear
				JsValueRef clear_fn = chakra::GetProperty(proxy, "Clear");
				JsValueRef returnValue = JS_INVALID_REFERENCE;
				JsCallFunction(clear_fn, &proxy, 1, &returnValue);

				JsValueRef add_fn = chakra::GetProperty(proxy, "Add");

				// "whole array" can be set
				if (chakra::IsArray(value))
				{
					JsValueRef arr = value;
					int Length = chakra::Length(arr);
					for (decltype(Length) Index = 0; Index < Length; ++Index)
					{
						JsValueRef elem = chakra::GetIndex(arr, Index);
						JsValueRef args[] = { proxy, elem };
						JsCallFunction(add_fn, args, 2, &returnValue);
					}
				}
				// only one delegate
				else if (!chakra::IsNull(value))
				{
					JsValueRef args[] = { proxy, value };
					JsCallFunction(add_fn, args, 2, &returnValue);
				}
			};

			if (Object->IsValidLowLevelFast())
			{
				FScopeCycleCounterUObject ContextScope(Object);
				FScopeCycleCounterUObject PropertyScope(Property);
				SCOPE_CYCLE_COUNTER(STAT_JavascriptPropertySet);

				// Multicast delegate
				if (auto p = Cast<UMulticastDelegateProperty>(Property))
				{
					auto proxy = static_cast<FJavascriptContextImplementation*>(ctx)->Delegates->GetProxy(self, Object, p);
					SetDelegate(proxy);
				}
				// delegate
				else if (auto p = Cast<UDelegateProperty>(Property))
				{
					auto proxy = static_cast<FJavascriptContextImplementation*>(ctx)->Delegates->GetProxy(self, Object, p);
					SetDelegate(proxy);
				}
				else
				{
					ctx->WriteProperty(Property, (uint8*)Object, value);
				}
			}
		}
	};

	struct FStructPropertyAccessors
	{
		static void* This(JsValueRef self)
		{
			return FStructMemoryInstance::FromChakra(self)->GetMemory();
		}

		static JsValueRef Get(FJavascriptContext* context, JsValueRef self, UProperty* Property)
		{
			auto Instance = FStructMemoryInstance::FromChakra(self);
			if (Instance)
			{
				return context->ReadProperty(Property, Instance->GetMemory(), FStructMemoryPropertyOwner(Instance));
			}
			else
			{
				return chakra::Undefined();
			}
		}

		static void Set(FJavascriptContext* context, JsValueRef self, UProperty* Property, JsValueRef value)
		{
			auto Instance = FStructMemoryInstance::FromChakra(self);
			if (Instance)
			{
				context->WriteProperty(Property, Instance->GetMemory(), value);
			}
			else
			{
				chakra::Throw(TEXT("Null struct"));
			}
		}
	};

	int NextModuleSourceContext = 0;
	TMap<FString, Persistent<JsValueRef>> Modules;
	TArray<FString>& Paths;

	void SetAsDebugContext(int32 InPort)
	{
		if (debugger || debugRequested) return;

		debugRequested = true;
		debugPort = InPort;
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

	void EnqueuePromiseTask(JsValueRef Task)
	{
		JsCheck(JsAddRef(Task, nullptr));
		PromiseTasks.Add(Task);
	}

	FJavascriptContextImplementation(JsRuntimeHandle InRuntime, TArray<FString>& InPaths)
		: Paths(InPaths)
		, bGCRequested(false)
	{
		check(InRuntime != JS_INVALID_RUNTIME_HANDLE);

		JsContextRef context = JS_INVALID_REFERENCE;
		JsCheck(JsCreateContext(InRuntime, &context));
		JsCheck(JsSetContextData(context, this));

		context_.Reset(context);

		// Set our array buffer allocator instance
		//params.array_buffer_allocator = &AllocatorInstance;

		// Bind this instance to newly created V8 isolate
		Delegates = IDelegateManager::Create();

		GenerateBlueprintFunctionLibraryMapping();

		InitializeGlobalTemplate();

		TickDelegate = FTickerDelegate::CreateRaw(this, &FJavascriptContextImplementation::HandleTicker);
		TickHandle = FTicker::GetCoreTicker().AddTicker(TickDelegate);
		ResumeTick();

		ExposeGlobals();
	}

	~FJavascriptContextImplementation()
	{
		PurgeModules();

		ReleaseAllPersistentHandles();

		ResetAsDebugContext();
		DestroyInspector();

		Delegates->Destroy();
		Delegates = nullptr;

		PauseTick();
		FTicker::GetCoreTicker().RemoveTicker(TickHandle);

		context_.Reset();
	}

	void ReleaseAllPersistentHandles()
	{
		// Release all object instances
		ObjectToObjectMap.Empty();

		// Release all struct instances
		MemoryToObjectMap.Empty();

		// Release all exported classes
		ClassToFunctionTemplateMap.Empty();

		// Release all exported structs(non-class)
		ScriptStructToFunctionTemplateMap.Empty();

		// Release global template
		GlobalTemplate.Reset();
	}

	void InitializeGlobalTemplate()
	{
		// Declares isolate/handle scope
		FContextScope ContextScope(context());

		// Create a new object template
		JsValueRef ObjectTemplate = JS_INVALID_REFERENCE;
		JsCheck(JsCreateObject(&ObjectTemplate));

		// Save it into the persistant handle
		GlobalTemplate.Reset(ObjectTemplate);

		// Export all structs
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			ExportStruct(*It);
		}

		// Export all classes
		for (TObjectIterator<UClass> It; It; ++It)
		{
			ExportClass(*It);
		}

		// Export all enums
		for (TObjectIterator<UEnum> It; It; ++It)
		{
			ExportEnum(*It);
		}

		ExportText(ObjectTemplate);

		ExportGC(ObjectTemplate);

		ExportConsole(ObjectTemplate);

		ExportMemory(ObjectTemplate);

		ExportMisc(ObjectTemplate);
	}

	void ExposeGlobals()
	{
		FContextScope context_scope(context());

		// make task queue working
		JsCheck(JsSetPromiseContinuationCallback([](JsValueRef Task, void* callbackState) {
			reinterpret_cast<FJavascriptContextImplementation*>(callbackState)->EnqueuePromiseTask(Task);
		}, this));

		CopyGlobalTemplate();
		ExposeRequire();
		ExportUnrealEngineClasses();
		ExportUnrealEngineStructs();

		ExposeMemory2();
	}

	void PurgeModules()
	{
		Modules.Empty();
	}

	void PauseTick() override
	{
		UE_LOG(Javascript, Log, TEXT("%p: Pause Tick"), this);
		RunInGameThread = false;
	}

	void ResumeTick() override
	{
		UE_LOG(Javascript, Log, TEXT("%p: Resume Tick"), this);
		RunInGameThread = true;
	}

	void GenerateBlueprintFunctionLibraryMapping()
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;

			// Blueprint function library only
			if (Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
			{
				// Iterate over all functions
				for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
				{
					auto Function = *FuncIt;
					TFieldIterator<UProperty> It(Function);

					// It should be a static function 
					if ((Function->FunctionFlags & FUNC_Static) && It)
					{
						// and have first argument to bind with.
						if ((It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
						{
							// The first argument should be type of object
							if (auto p = Cast<UObjectPropertyBase>(*It))
							{
								auto TargetClass = p->PropertyClass;

								// GetWorld() may fail and crash, so target class is bound to UWorld
								if (TargetClass == UObject::StaticClass() && (p->GetName() == TEXT("WorldContextObject") || p->GetName() == TEXT("WorldContext")))
								{
									TargetClass = UWorld::StaticClass();
								}

								BlueprintFunctionLibraryMapping.Add(TargetClass, Function);
								continue;
							}
							else if (auto p = Cast<UStructProperty>(*It))
							{
								BlueprintFunctionLibraryMapping.Add(p->Struct, Function);
								continue;
							}
						}

						// Factory function?
						for (auto It2 = It; It2; ++It2)
						{
							if ((It2->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == (CPF_Parm | CPF_ReturnParm))
							{
								if (auto p = Cast<UStructProperty>(*It2))
								{
									BlueprintFunctionLibraryFactoryMapping.Add(p->Struct, Function);
									break;
								}
							}
						}
					}
				}
			}
		}
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
			if (UWidgetBlueprintGeneratedClass* WidgetBlueprintClass = Cast<UWidgetBlueprintGeneratedClass>(ParentClass))
			{
				auto Klass = NewObject<UJavascriptWidgetGeneratedClass>(Outer, *Name, RF_Public);
				Klass->JavascriptContext = Context->AsShared();
				Class = Klass;

				if (WidgetBlueprintClass->HasTemplate())
					Klass->SetTemplate(WidgetBlueprintClass->GetTemplate());
			}
			else if (Cast<UBlueprintGeneratedClass>(ParentClass))
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

			auto ClassConstructor = [](const FObjectInitializer& ObjectInitializer) {
				auto Class = static_cast<UBlueprintGeneratedClass*>(CurrentClassUnderConstruction ? CurrentClassUnderConstruction : ObjectInitializer.GetClass());
				CurrentClassUnderConstruction = nullptr;

				FJavascriptContextImplementation* Context = nullptr;

				if (auto Klass = Cast<UJavascriptWidgetGeneratedClass>(Class))
				{
					if (Klass->JavascriptContext.IsValid())
					{
						Context = static_cast<FJavascriptContextImplementation*>(Klass->JavascriptContext.Pin().Get());
					}
				}
				else if (auto Klass = Cast<UJavascriptGeneratedClass_Native>(Class))
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

				Function->SetNativeFunc(&UJavascriptGeneratedFunction::Thunk);

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
				TArray<FString> Keys = chakra::PropertyNames(FuncMap);

				for (const FString& Name : Keys)
				{
					JsValueRef Function = chakra::GetProperty(FuncMap, Name);

					if (!chakra::IsFunction(Function)) continue;

					if (Name != TEXT("prector") && Name != TEXT("ctor") && Name != TEXT("constructor"))
					{
						if (!AddFunction(*Name, Function))
						{
							Others.Add(Name, Function);
						}
					}
				}
			}

			Class->Bind();
			Class->StaticLink(true);

			{
				JsFunctionRef FinalClass = Context->ExportClass(Class, false);
				JsValueRef ProtoType = chakra::GetProperty(FinalClass, "prototype");

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
			Class->UpdateCustomPropertyListForPostConstruction();

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
		JsCheck(JsGetGlobalObject(&global));

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
		JsCheck(JsGetGlobalObject(&global));

		chakra::SetProperty(global, "CreateStruct", chakra::FunctionTemplate(fn, this));
	}

	void CopyGlobalTemplate()
	{
		JsValueRef globalTmpl = GetGlobalTemplate();
		JsValueRef keys = JS_INVALID_REFERENCE;
		JsCheck(JsGetOwnPropertyNames(globalTmpl, &keys));

		JsValueRef global = JS_INVALID_REFERENCE;
		JsCheck(JsGetGlobalObject(&global));

		TArray<FString> keysArr = StringArrayFromChakra(keys);
		for (const FString& key : keysArr)
		{
			JsValueRef value = chakra::GetProperty(globalTmpl, key);
			JsCheck(JsSetProperty(global, chakra::PropertyID(key), value, true));
		}
	}

	void DoGarbageCollection()
	{
		FContextScope scope(context());

		// @todo: using 'ForTesting' function
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsCheck(JsGetRuntime(context(), &runtime));
		JsCheck(JsCollectGarbage(runtime));
	}

	void ExportText(JsValueRef GlobalTemplate)
	{
		auto func = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (!isConstructCall)
				return chakra::Undefined();

			JsValueRef self = arguments[0];
			check(argumentCount > 1);

			if (argumentCount == 2)
			{
				check(chakra::IsString(arguments[1]));
				chakra::SetProperty(self, "Source", arguments[1]);
			}
			else if (argumentCount == 3)
			{
				check(chakra::IsString(arguments[1]));
				check(chakra::IsString(arguments[2]));
				chakra::SetProperty(self, "Table", arguments[1]);
				chakra::SetProperty(self, "Key", arguments[2]);
			}
			else if (argumentCount == 4)
			{
				check(chakra::IsString(arguments[1]));
				check(chakra::IsString(arguments[2]));
				check(chakra::IsString(arguments[3]));
				chakra::SetProperty(self, "Namespace", arguments[1]);
				chakra::SetProperty(self, "Key", arguments[2]);
				chakra::SetProperty(self, "Source", arguments[3]);
			}

			return chakra::Undefined();
		};

		auto funcFindText = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (argumentCount < 4)
				return chakra::Undefined();

			JsValueRef Template = reinterpret_cast<JsValueRef>(callbackState);
			JsValueRef Instance = JS_INVALID_REFERENCE;
			JsCheck(JsConstructObject(Template, arguments, 4, &Instance));

			return Instance;
		};

		auto funcFromStringTable = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (argumentCount < 3)
				return chakra::Undefined();

			JsValueRef Template = reinterpret_cast<JsValueRef>(callbackState);
			JsValueRef Instance = JS_INVALID_REFERENCE;
			JsCheck(JsConstructObject(Template, arguments, 3, &Instance));

			return Instance;
		};

		auto funcToString = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			JsValueRef Self = arguments[0];
			FText Text = FText::GetEmpty();

			if (chakra::HasProperty(Self, "Table"))
			{
				FName Table = *chakra::StringFromChakra(chakra::GetProperty(Self, "Table"));
				FString Key = chakra::StringFromChakra(chakra::GetProperty(Self, "Key"));
				FStringTableConstPtr TablePtr = FStringTableRegistry::Get().FindStringTable(Table);

				if (TablePtr.IsValid() && TablePtr->FindEntry(Key).IsValid())
				{
					Text = FText::FromStringTable(Table, Key);
				}
				else
				{
					Text = FText::FromString(Key);
				}
			}
			else if (chakra::HasProperty(Self, "Namespace"))
			{
				JsValueRef Source = chakra::GetProperty(Self, "Source");
				JsValueRef Namespace = chakra::GetProperty(Self, "Namespace");
				JsValueRef Key = chakra::GetProperty(Self, "Key");

				if (!FText::FindText(chakra::StringFromChakra(Namespace), chakra::StringFromChakra(Key), Text))
					Text = FText::FromString(chakra::StringFromChakra(Source));
			}
			else if (chakra::HasProperty(Self, "Source"))
			{
				Text = FText::FromString(chakra::StringFromChakra(chakra::GetProperty(Self, "Source")));
			}
			else
			{
				UE_LOG(Javascript, Warning, TEXT("Could not detext text type from object"));
			}

			return chakra::String(Text.ToString());
		};

		JsValueRef tmpl = JS_INVALID_REFERENCE;
		JsCheck(JsCreateNamedFunction(chakra::String("FText"), func, nullptr, &tmpl));

		JsValueRef prototype = chakra::GetProperty(tmpl, "prototype");
		chakra::SetProperty(tmpl, "FromStringTable", chakra::FunctionTemplate(funcFromStringTable));
		chakra::SetProperty(tmpl, "FindText", chakra::FunctionTemplate(funcFindText));
		chakra::SetProperty(prototype, "toString", chakra::FunctionTemplate(funcToString));
		chakra::SetProperty(prototype, "valueOf", chakra::FunctionTemplate(funcToString));

		chakra::SetProperty(GlobalTemplate, "FText", tmpl);

		this->TextTemplate.Reset(tmpl);
	}

	void ExportGC(JsValueRef GlobalTemplate)
	{
		auto func = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (argumentCount == 1)
			{
				FJavascriptContextImplementation* Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
				Self->DoGarbageCollection();
			}

			return chakra::Undefined();
		};

		chakra::SetProperty(GlobalTemplate, "gc", chakra::FunctionTemplate(func, this));
	}

	void ExposeRequire()
	{
		FContextScope scope(context());

		auto requireImpl = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			if (argumentCount != 3 || !chakra::IsString(arguments[1]) || !chakra::IsString(arguments[2]))
			{
				return chakra::Undefined();
			}

			FJavascriptContextImplementation* Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());

			FString current_script = chakra::StringFromChakra(arguments[1]);
			FString required_module = chakra::StringFromChakra(arguments[2]);

			bool found = false;

			JsValueRef returnValue = JS_INVALID_REFERENCE;
			auto inner = [&](const FString& script_path)
			{
				FString relative_path = script_path.Replace(TEXT("/./"), TEXT("/"));

				// path referencing .. cleansing
				int pos = relative_path.Len();
				while (true)
				{
					int idx = relative_path.Find("/../", ESearchCase::CaseSensitive, ESearchDir::FromEnd, pos);
					if (idx < 0)
						break;

					int parentIdx = relative_path.Find("/", ESearchCase::CaseSensitive, ESearchDir::FromEnd, idx) + 1;
					FString parent = relative_path.Mid(parentIdx, idx - parentIdx);
					if (parent == "..")
					{
						pos = idx + 1;
						continue;
					}

					relative_path = relative_path.Mid(0, parentIdx) + relative_path.Mid(idx + 4);
					pos = relative_path.Len(); // reset counter
				}

				FString full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*relative_path);
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
				if (FFileHelper::LoadFileToString(Text, *relative_path))
				{
					Text = FString::Printf(TEXT("(function (global, __filename, __dirname) { var module = { exports : {}, filename : __filename }, exports = module.exports, require = specifier => global.require(__filename, specifier); (function () { %s\n })();\nreturn module.exports;})(this,'%s', '%s');"), *Text, *relative_path, *FPaths::GetPath(relative_path));
					JsValueRef exports = Self->RunScript(full_path, Text);

					if (chakra::IsEmpty(exports))
					{
						UE_LOG(Javascript, Log, TEXT("Invalid script for require"));
						return false;
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
					JsValueRef exports = Self->RunScript(full_path, Text);
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
					JsValueRef exports = Self->RunScript(full_path, Text);
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

				if (inner_package_json(script_path)) return true;
				if (inner(script_path / TEXT("index.js"))) return true;

				return false;
			};

			auto load_module_paths = [&](FString base_path)
			{
				TArray<FString> Dirs;
				TArray<FString> Parsed;
				base_path.ParseIntoArray(Parsed, TEXT("/"));
				auto PartCount = Parsed.Num();
				while (PartCount > 0) {
					if (Parsed[PartCount - 1].Equals(TEXT("node_modules")))
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

			FString current_script_path = FPaths::GetPath(URLToLocalPath(current_script));

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
				UE_LOG(Javascript, Log, TEXT("Cannot find %s from %s"), *required_module, *current_script);
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
		JsCheck(JsGetGlobalObject(&global));

		chakra::SetProperty(global, "require", chakra::FunctionTemplate(requireImpl, this));
		chakra::SetProperty(global, "purge_modules", chakra::FunctionTemplate(fn2, this));

		auto getter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());

			JsValueRef out = JS_INVALID_REFERENCE;
			JsCheck(JsCreateObject(&out));

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
		JsCheck(JsGetGlobalObject(&global));

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
								JsCheck(JsCallFunction(function, argv, 2, &returnValue));
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
							JsCheck(JsCallFunction(function, argv, 2, &returnValue));
							return chakra::Undefined();
						}
						else if (Dimension == 2)
						{
							int Outer = Source->GetSize(0);
							int Inner = Source->GetSize(1);
							JsValueRef out_arr = JS_INVALID_REFERENCE;
							JsCheck(JsCreateArray(Outer, &out_arr));

							argv[1] = out_arr;
							for (auto Index = 0; Index < Outer; ++Index)
							{
								Indices[0] = Index;
								JsValueRef ab = JS_INVALID_REFERENCE;
								JsCheck(JsCreateExternalArrayBuffer(Source->GetMemory(Indices), Inner, nullptr, nullptr, &ab));
								chakra::SetIndex(out_arr, Index, ab);
							}

							JsValueRef returnValue = JS_INVALID_REFERENCE;
							JsCheck(JsCallFunction(function, argv, 2, &returnValue));
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
						JsCheck(JsCallFunction(function, argv, 2, &returnValue));
						return chakra::Undefined();
					}
				}
			}
			chakra::Throw(TEXT("memory.fork requires JavascriptMemoryObject"));
			return chakra::Undefined();
		});
	}

	bool HandleTicker(float DeltaTime)
	{
		if (!RunInGameThread) return true;

		if (debugRequested)
		{
			FContextScope scope(context());
			debugger = IJavascriptDebugger::Create(debugPort, context());
			debugRequested = false;
		}

		if (debugger == nullptr || !debugger->IsBreak())
		{
			TArray<JsValueRef> tasksCopy(const_cast<const TArray<JsValueRef>&>(PromiseTasks));
			PromiseTasks.Empty();

			FContextScope scope(context());
			JsValueRef global = JS_INVALID_REFERENCE, dummy = JS_INVALID_REFERENCE;
			JsCheck(JsGetGlobalObject(&global));

			for (JsValueRef task : tasksCopy)
			{
				JsCheck(JsCallFunction(task, &global, 1, &dummy));

				bool hasException = false;
				JsCheck(JsHasException(&hasException));
				if (hasException)
				{
					JsValueRef exception = JS_INVALID_REFERENCE;
					JsCheck(JsGetAndClearException(&exception));

					JsValueRef stackValue = chakra::GetProperty(exception, "stack");
					FString strErr = chakra::StringFromChakra(exception);
					FString stack = chakra::StringFromChakra(stackValue);
					UncaughtException(strErr + " " + stack);
				}
				
				JsCheck(JsRelease(task, nullptr));
			}
		}

		// do garbage colliction now
		if (bGCRequested)
		{
			DoGarbageCollection();
			bGCRequested = false;
		}

		return true;
	}

	FString GetScriptFileFullPath(const FString& Filename)
	{
		for (auto Path : Paths)
		{
			auto FullPath = Path / Filename;
			auto Size = IFileManager::Get().FileSize(*FullPath);
			if (Size != INDEX_NONE)
			{
				return FullPath;
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

		FString Text = FString::Printf(TEXT("(function (global,__filename,__dirname) { var require = specifier => global.require(__filename, specifier); %s;\n})(this,'%s','%s');"), *Script, *ScriptPath, *FPaths::GetPath(ScriptPath));
		return RunScript(ScriptPath, Text);
	}

	void Public_RunFile(const FString& Filename)
	{
		RunFile(Filename);
	}

	FString Public_RunScript(const FString& Script, bool bOutput = true)
	{
		FContextScope context_scope(context());

		JsValueRef ret = RunScript(TEXT("(inline)"), Script);
		FString str = "(empty)";
		if (!chakra::IsEmpty(ret))
		{
			JsValueRef strValue = JS_INVALID_REFERENCE;
			JsCheck(JsConvertValueToString(ret, &strValue));

			str = chakra::StringFromChakra(strValue);
		}

		if (bOutput && !chakra::IsEmpty(ret))
		{
			UE_LOG(Javascript, Log, TEXT("%s"), *str);
		}
		return str;
	}

	void RequestV8GarbageCollection()
	{
		bGCRequested = true;
	}

	// Should be guarded with proper handle scope
	JsValueRef RunScript(const FString& Filename, const FString& Script)
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

		JsValueRef script = chakra::String(Script);
		JsErrorCode err = JsNoError;

		// run script with context stack
		err = JsRun(script, NextModuleSourceContext++, chakra::String(LocalPathToURL(Path)), JsParseScriptAttributeNone, &returnValue);

		if (err == JsErrorScriptException)
		{
			JsValueRef exception = JS_INVALID_REFERENCE;
			JsCheck(JsGetAndClearException(&exception));

			UncaughtException(FV8Exception::Report(exception));
			return JS_INVALID_REFERENCE;
		}

		if (err == JsErrorScriptCompile)
		{
			UE_LOG(Javascript, Error, TEXT("Failed to compile script %s"), *Filename);
			UE_LOG(Javascript, Error, TEXT("%s"), *Script);
			UncaughtException("Invalid command");

			JsValueRef exception = JS_INVALID_REFERENCE;
			JsCheck(JsGetAndClearException(&exception));

			JsValueRef strErr = JS_INVALID_REFERENCE;
			JsCheck(JsConvertValueToString(exception, &strErr));

			UncaughtException(chakra::StringFromChakra(strErr));
			return JS_INVALID_REFERENCE;
		}

		if (err != JsNoError)
		{
			checkf(false, TEXT("failed to execute script! %d"), (int)err);
			return JS_INVALID_REFERENCE;
		}

		return returnValue;
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
		JsCheck(JsGetGlobalObject(&global));

		chakra::FPropertyDescriptor desc;
		desc.Getter = chakra::FunctionTemplate(RootGetter, ExportObject(Object));

		chakra::SetAccessor(global, RootName, desc);
	}

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

		// export
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

									JsValueRef args[] = { DefaultValue, DefaultValue };
									JsValueRef ret = JS_INVALID_REFERENCE;
									JsCheck(JsCallFunction(Packer, args, 2, &ret));
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

		if (!RunInGameThread) return false;

		FContextScope context_scope(context());

		JsValueRef func = GetProxyFunction(Holder, FunctionToCall);
		if (!chakra::IsEmpty(func) && chakra::IsFunction(func))
		{
			JsValueRef global = JS_INVALID_REFERENCE;
			JsCheck(JsGetGlobalObject(&global));

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
		JsCheck(JsGetGlobalObject(&global));
		JsValueRef func = chakra::GetProperty(global, "$uncaughtException");
		if (!chakra::IsEmpty(func) && chakra::IsFunction(func))
		{
			JsValueRef argv[2] = { global };

			argv[1] = chakra::String(Exception);

			JsValueRef returnValue = JS_INVALID_REFERENCE;
			JsCheck(JsCallFunction(func, argv, 2, &returnValue));
		}

		UE_LOG(Javascript, Error, TEXT("%s"), *Exception);
	}

	JsValueRef ReadProperty(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner) override
	{
		return InternalReadProperty(Property, Buffer, Owner);
	}

	template <typename T>
	//JsValueRef ConvertValue(const T& cValue, UProperty* Property, const IPropertyOwner& Owner) = delete;
	JsValueRef ConvertValue(const T& cValue, UProperty* Property, const IPropertyOwner& Owner)
	{
		//static_assert(false, "implement for this type");
		check(false);
	}

	template<typename T>
	JsValueRef InternalReadSealedArray(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
	{
		if (Property->ArrayDim == 1)
		{
			T* ptr = Property->ContainerPtrToValuePtr<T>(Buffer);
			return ConvertValue<T>(*ptr, Property, Owner);
		}

		JsValueRef array = JS_INVALID_REFERENCE;
		JsCheck(JsCreateArray(0, &array));
		for (int i = 0; i < Property->ArrayDim; i++)
		{
			T* ptr = Property->ContainerPtrToValuePtr<T>(Buffer, i);
			chakra::SetIndex(array, i, ConvertValue<T>(*ptr, Property, Owner));
		}

		// sealing process
		JsValueRef sealer = RunScript("", "arr => Object.seal(arr)");
		check(chakra::IsFunction(sealer));

		JsValueRef sealedArray = JS_INVALID_REFERENCE;
		JsValueRef args[] = { chakra::Undefined(), array };
		JsCheck(JsCallFunction(sealer, args, 2, &sealedArray));
		check(chakra::IsArray(sealedArray));

		return sealedArray;
	}

	JsValueRef InternalReadProperty(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
	{
		if (!Buffer)
		{
			chakra::Throw(TEXT("Read property from invalid memory"));
			return chakra::Undefined();
		}

		if (Property->IsA<UNumericProperty>() && !Property->IsA<UByteProperty>())
		{
			auto p = Cast<UNumericProperty>(Property);

			if (p->IsFloatingPoint())
				return InternalReadSealedArray<float>(p, Buffer, Owner);

			check(p->IsInteger());

			if (p->IsA<UByteProperty>() || p->IsA<UUInt16Property>() || p->IsA<UUInt32Property>() || p->IsA<UUInt64Property>() || p->IsA<UUnsizedUIntProperty>())
				return InternalReadSealedArray<uint32>(p, Buffer, Owner);

			return InternalReadSealedArray<int32>(p, Buffer, Owner);
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			return InternalReadSealedArray<bool>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{
			return InternalReadSealedArray<FName>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			return InternalReadSealedArray<FString>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UTextProperty>(Property))
		{
			return InternalReadSealedArray<FText>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			return InternalReadSealedArray<UClass*>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			return InternalReadSealedArray<FStructDummy>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			return InternalReadSealedArray<FScriptArray>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<USoftObjectProperty>(Property))
		{
			return InternalReadSealedArray<FSoftObjectPtr>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			return InternalReadSealedArray<UObject*>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			return InternalReadSealedArray<uint8>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			return InternalReadSealedArray<uint8>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			return InternalReadSealedArray<FScriptSet>(Property, Buffer, Owner);
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			return InternalReadSealedArray<FScriptMap>(Property, Buffer, Owner);
		}
		else
		{
			return chakra::String("<Unsupported type>");
		}
	}

	void ReadOffStruct(JsValueRef v8_obj, UStruct* Struct, uint8* struct_buffer)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptReadOffStruct);
		FScopeCycleCounterUObject StructContext(Struct);

		JsValueRef arr = JS_INVALID_REFERENCE;
		JsCheck(JsGetOwnPropertyNames(v8_obj, &arr));
		if (chakra::IsEmpty(arr)) return;

		int len = chakra::Length(arr);

		for (TFieldIterator<UProperty> PropertyIt(Struct, EFieldIteratorFlags::IncludeSuper); PropertyIt && len; ++PropertyIt)
		{
			UProperty* Property = *PropertyIt;
			FString PropertyName = PropertyNameToString(Property);

			JsValueRef value = chakra::GetProperty(v8_obj, PropertyName);

			if (!chakra::IsEmpty(value) && !chakra::IsUndefined(value))
			{
				len--;
				InternalWriteProperty(Property, struct_buffer, value);
			}
		}
	}

	void WriteProperty(UProperty* Property, uint8* Buffer, JsValueRef Value) override
	{
		InternalWriteProperty(Property, Buffer, Value);
	}

	template <typename T>
	//void FromValue(T* Ptr, UProperty* Property, JsValueRef Value) = delete;
	void FromValue(T* Ptr, UProperty* Property, JsValueRef Value)
	{
		//static_assert(false, "implement for this type");
		check(false);
	}

	template <typename T>
	void InternalWriteSealedArray(UProperty* Property, uint8* Buffer, JsValueRef Value)
	{
		if (chakra::IsArray(Value) && Property->ArrayDim > 1)
		{
			check(chakra::Length(Value) == Property->ArrayDim); // this is fixed size array
			for (int i = 0; i < Property->ArrayDim; i++)
			{
				T* ptr = Property->ContainerPtrToValuePtr<T>(Buffer, i);
				FromValue<T>(ptr, Property, chakra::GetIndex(Value, i));
			}
		}
		else
		{
			//check(!chakra::IsArray(Value)); // TArray?
			check(Property->ArrayDim <= 1);

			T* ptr = Property->ContainerPtrToValuePtr<T>(Buffer);
			FromValue<T>(ptr, Property, Value);
		}
	}

	void InternalWriteProperty(UProperty* Property, uint8* Buffer, JsValueRef Value)
	{
		if (!Buffer)
		{
			chakra::Throw(TEXT("Write property on invalid memory"));
			return;
		}

		if (chakra::IsEmpty(Value) || chakra::IsUndefined(Value)) return;

		if (Property->IsA<UNumericProperty>() && !Property->IsA<UByteProperty>())
		{
			auto p = Cast<UNumericProperty>(Property);
			uint8* ptr = p->ContainerPtrToValuePtr<uint8>(Buffer);

			if (p->IsFloatingPoint())
				return InternalWriteSealedArray<float>(Property, Buffer, Value);

			check(p->IsInteger());

			if (p->IsA<UUInt16Property>() || p->IsA<UUInt32Property>() || p->IsA<UUInt64Property>() || p->IsA<UUnsizedUIntProperty>())
				return InternalWriteSealedArray<uint32>(Property, Buffer, Value);

			return InternalWriteSealedArray<int32>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			return InternalWriteSealedArray<bool>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{
			return InternalWriteSealedArray<FName>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			return InternalWriteSealedArray<FString>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UTextProperty>(Property))
		{
			return InternalWriteSealedArray<FText>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			return InternalWriteSealedArray<UClass*>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			return InternalWriteSealedArray<FStructDummy>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			return InternalWriteSealedArray<FScriptArray>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			return InternalWriteSealedArray<uint8>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			return InternalWriteSealedArray<uint8>(Property, Buffer, Value);
		}
		else if (auto p = Cast<USoftObjectProperty>(Property))
		{
			return InternalWriteSealedArray<FSoftObjectPtr>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			return InternalWriteSealedArray<UObject*>(Property, Buffer, Value);
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			return InternalWriteSealedArray<FScriptSet>(Property, Buffer, Value);
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			return InternalWriteSealedArray<FScriptMap>(Property, Buffer, Value);
		}
	};

	virtual JsValueRef GetGlobalTemplate() override
	{
		return GlobalTemplate.Get();
	}

	void ExportConsole(JsValueRef global_templ)
	{
		JsValueRef ConsoleTemplate = chakra::FunctionTemplate();
		JsValueRef ConsolePrototype = chakra::GetProperty(ConsoleTemplate, "prototype");

		auto add_fn = [&](const char* name, JsNativeFunction fn) {
			chakra::SetProperty(ConsolePrototype, name, chakra::FunctionTemplate(fn));
		};

		// console.log
		add_fn("log", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UE_LOG(Javascript, Log, TEXT("%s"), *chakra::StringFromArgs(arguments, argumentCount, 1));
			return chakra::Undefined();
		});

		// console.warn
		add_fn("warn", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UE_LOG(Javascript, Warning, TEXT("%s"), *chakra::StringFromArgs(arguments, argumentCount, 1));
			return chakra::Undefined();
		});

		// console.info
		add_fn("info", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UE_LOG(Javascript, Display, TEXT("%s"), *chakra::StringFromArgs(arguments, argumentCount, 1));
			return chakra::Undefined();
		});

		// console.error
		add_fn("error", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UE_LOG(Javascript, Error, TEXT("%s"), *chakra::StringFromArgs(arguments, argumentCount, 1));
			return chakra::Undefined();
		});

		// console.assert
		add_fn("assert", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			bool to_assert = argumentCount < 2 || !chakra::BoolFrom(arguments[1]);
			if (to_assert)
			{
				JsValueRef error = JS_INVALID_REFERENCE;
				FString message = FString::Printf(TEXT("Assertion: %s"), *chakra::StringFromArgs(arguments, argumentCount, 2));
				JsCheck(JsCreateError(chakra::String(message), &error));

				JsValueRef stack = chakra::GetProperty(error, "stack");
				JsCheck(JsConvertValueToString(stack, &stack)); // => stack = stack.toString();

				UE_LOG(Javascript, Error, TEXT("%s"), *chakra::StringFromChakra(stack));
			}

			return chakra::Undefined();
		});

		// console.void
		add_fn("void", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			return chakra::Undefined();
		});

		chakra::SetProperty(global_templ, "console", chakra::New(ConsoleTemplate));
	}

	void ExportMisc(JsValueRef global_templ)
	{
		auto fileManagerCwd = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			return chakra::String(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(L".")); // FPaths::ProjectDir()
		};
		chakra::SetProperty(global_templ, "$cwd", chakra::FunctionTemplate(fileManagerCwd));

#if WITH_EDITOR
		auto exec_editor = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			FEditorScriptExecutionGuard Guard;

			if (argumentCount == 2)
			{
				JsValueRef function = arguments[1];
				if (!chakra::IsEmpty(function))
				{
					JsValueRef returnValue = JS_INVALID_REFERENCE;
					JsCheck(JsCallFunction(function, &arguments[0], 1, &returnValue));
				}
			}

			return chakra::Undefined();
		};
		chakra::SetProperty(global_templ, "$execEditor", chakra::FunctionTemplate(exec_editor));

		auto exec_transaction = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 3)
			{
				FString String = chakra::StringFromChakra(arguments[1]);
				FScopedTransaction Transaction(FText::FromString(String));

				JsValueRef function = arguments[2];
				if (!chakra::IsEmpty(function))
				{
					JsValueRef returnValue = JS_INVALID_REFERENCE;
					JsCheck(JsCallFunction(function, &arguments[0], 1, &returnValue));
				}
			}

			return chakra::Undefined();
		};
		chakra::SetProperty(global_templ, "$exec_transaction", chakra::FunctionTemplate(exec_transaction));

		auto exec_profile = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 3)
			{
				for (;;)
				{
					JsValueRef function = arguments[2];
					if (chakra::IsEmpty(function))
					{
						break;
					}

					if (chakra::IsObject(arguments[1]))
					{
						UObject* Object = chakra::UObjectFromChakra(arguments[1]);
						if (Object)
						{
							FScopeCycleCounterUObject ContextScope(Object);
							JsValueRef returnValue = JS_INVALID_REFERENCE;
							JsCheck(JsCallFunction(function, &arguments[0], 1, &returnValue));

							return returnValue;
						}
					}

					JsValueRef returnValue = JS_INVALID_REFERENCE;
					JsCheck(JsCallFunction(function, &arguments[0], 1, &returnValue));
					return returnValue;
				}
			}

			return chakra::Undefined();
		};
		chakra::SetProperty(global_templ, "$profile", chakra::FunctionTemplate(exec_profile));
#endif
	}

	void ExportMemory(JsValueRef global_templ)
	{
		JsValueRef Template = chakra::FunctionTemplate();
		JsValueRef Prototype = chakra::GetProperty(Template, "prototype");

		auto add_fn = [&](const char* name, JsNativeFunction fn) {
			chakra::SetProperty(Prototype, name, chakra::FunctionTemplate(fn));
		};

		add_fn("access", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 2)
			{
				auto Source = Cast<UJavascriptMemoryObject>(chakra::UObjectFromChakra(arguments[1]));

				if (Source)
				{
					JsValueRef ab = JS_INVALID_REFERENCE;
					JsCheck(JsCreateExternalArrayBuffer(Source->GetMemory(), Source->GetSize(), nullptr, nullptr, &ab));
					chakra::SetProperty(ab, "$source", arguments[1]);
					return ab;
				}
			}

			chakra::Throw(TEXT("memory.fork requires JavascriptMemoryObject"));
			return chakra::Undefined();
		});

		add_fn("exec", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 3 && chakra::IsArrayBuffer(arguments[1]) && chakra::IsFunction(arguments[2]))
			{
				JsValueRef arr = arguments[1];
				JsValueRef function = arguments[2];

				GCurrentContents = arr;

				JsValueRef argv[2] = { arguments[0], arr };
				JsValueRef returnValue = JS_INVALID_REFERENCE;
				JsCheck(JsCallFunction(function, argv, 2, &returnValue));

				GCurrentContents = JS_INVALID_REFERENCE;
			}
			else
			{
				GCurrentContents = JS_INVALID_REFERENCE;
			}

			return chakra::GetPrototype(arguments[0]);
		});

		// memory.bind
		add_fn("bind", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UE_LOG(Javascript, Warning, TEXT("memory.bind is deprecated. use memory.exec(ab,fn) instead."));

			if (argumentCount == 2 && chakra::IsArrayBuffer(arguments[1]))
			{
				JsValueRef arr = arguments[1];

				GCurrentContents = arr;
			}
			else
			{
				GCurrentContents = JS_INVALID_REFERENCE;
			}

			return chakra::GetPrototype(arguments[0]);
		});

		// memory.unbind
		add_fn("unbind", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 2 && chakra::IsArrayBuffer(arguments[1]))
			{
				JsValueRef arr = arguments[1];

				GCurrentContents = JS_INVALID_REFERENCE;
			}

			return chakra::GetPrototype(arguments[0]);
		});

		// console.void
		add_fn("write", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			if (argumentCount == 3)
			{
				JsValueRef filename = arguments[1];
				JsValueRef data = arguments[2];

				FArchive* Ar = IFileManager::Get().CreateFileWriter(*chakra::StringFromChakra(filename), 0);
				if (Ar)
				{
					if (chakra::IsArrayBuffer(data))
					{
						ChakraBytePtr dataPtr = nullptr;
						unsigned dataSize = 0;
						JsCheck(JsGetArrayBufferStorage(data, &dataPtr, &dataSize));

						Ar->Serialize(dataPtr, dataSize);
					}

					delete Ar;
				}
			}
			else
			{
				chakra::Throw(TEXT("Two arguments needed"));
			}

			return chakra::GetPrototype(arguments[0]);
		});

		//add_fn("takeSnapshot", [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		//{			
		//	class FileOutputStream : public OutputStream
		//	{
		//	public:
		//		FileOutputStream(FArchive* ar) : ar_(ar) {}

		//		virtual int GetChunkSize() {
		//			return 65536;  // big chunks == faster
		//		}

		//		virtual void EndOfStream() {}

		//		virtual WriteResult WriteAsciiChunk(char* data, int size) {
		//			ar_->Serialize(data, size);					
		//			return ar_->IsError() ? kAbort : kContinue;
		//		}

		//	private:
		//		FArchive* ar_;
		//	};

		//	if (info.Length() == 1)
		//	{
		//		const HeapSnapshot* const snap = info.GetIsolate()->GetHeapProfiler()->TakeHeapSnapshot();
		//		FArchive* Ar = IFileManager::Get().CreateFileWriter(*StringFromV8(info[0]), 0);
		//		if (Ar)
		//		{
		//			FileOutputStream stream(Ar);
		//			snap->Serialize(&stream, HeapSnapshot::kJSON);
		//			delete Ar;
		//		}

		//		// Work around a deficiency in the API.  The HeapSnapshot object is const
		//		// but we cannot call HeapProfiler::DeleteAllHeapSnapshots() because that
		//		// invalidates _all_ snapshots, including those created by other tools.
		//		const_cast<HeapSnapshot*>(snap)->Delete();
		//	}
		//	else
		//	{
		//		I.Throw(TEXT("One argument needed"));
		//	}
		//});

		chakra::SetProperty(global_templ, "memory", chakra::New(Template));
	}

	template <typename Fn>
	static JsValueRef CallFunction(JsValueRef self, UFunction* Function, UObject* Object, Fn&& GetArg)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptFunctionCallToEngine);

		// Allocate buffer(param size) in stack
		uint8* Buffer = (uint8*)FMemory_Alloca(Function->ParmsSize);

		// Arguments should construct and destruct along this scope
		FScopedArguments scoped_arguments(Function, Buffer);

		// Does this function return some parameters by reference?
		bool bHasAnyOutParams = false;

		// Argument index
		int ArgIndex = 0;

		// Intentionally declares iterator outside for-loop scope
		TFieldIterator<UProperty> It(Function);

		int32 NumArgs = 0;

		// Iterate over input parameters
		for (; It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
		{
			UProperty* Prop = *It;

			// Get argument from caller
			JsValueRef arg = GetArg(ArgIndex++);

			// Do we have valid argument?
			if (!chakra::IsEmpty(arg) && !chakra::IsUndefined(arg))
			{
				GetSelf()->WriteProperty(Prop, Buffer, arg);
			}

			// This is 'out ref'!
			if ((It->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
			{
				bHasAnyOutParams = true;
			}
		}

		NumArgs = ArgIndex;

		// Call regular native function.
		FScopeCycleCounterUObject ContextScope(Object);
		FScopeCycleCounterUObject FunctionScope(Function);

		Object->ProcessEvent(Function, Buffer);

		auto FetchProperty = [&](UProperty* Param, int32 ArgIndex) {
			if (auto p = Cast<UStructProperty>(Param))
			{
				// Get argument from caller
				JsValueRef arg = GetArg(ArgIndex);

				// Do we have valid argument?
				if (!chakra::IsEmpty(arg) && !chakra::IsUndefined(arg))
				{
					auto Instance = FStructMemoryInstance::FromChakra(arg);
					if (Instance)
					{
						p->Struct->CopyScriptStruct(Instance->GetMemory(), p->ContainerPtrToValuePtr<uint8>(Buffer));

						return arg;
					}
				}
			}

			return GetSelf()->ReadProperty(Param, Buffer, FNoPropertyOwner());
		};

		// In case of 'out ref'
		if (bHasAnyOutParams)
		{
			ArgIndex = 0;

			// Allocate an object to pass return values within
			JsValueRef OutParameters = JS_INVALID_REFERENCE;
			JsCheck(JsCreateObject(&OutParameters));

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(Function); It; ++It, ArgIndex++)
			{
				UProperty* Param = *It;

				auto PropertyFlags = Param->GetPropertyFlags();

				// pass return parameter as '$'
				if (PropertyFlags & CPF_ReturnParm)
				{
					// value can be null if isolate is in trouble
					JsValueRef value = FetchProperty(Param, NumArgs);
					if (!chakra::IsEmpty(value))
					{
						chakra::SetProperty(OutParameters, "$", value);
					}
				}
				// rejects 'const T&' and pass 'T&' as its name
				else if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					JsValueRef value = FetchProperty(Param, ArgIndex);
					if (!chakra::IsEmpty(value))
					{
						chakra::SetProperty(OutParameters, Param->GetName(), value);
					}
				}
			}

			// We're done
			return OutParameters;
		}
		else
		{
			// Iterate to fill out return parameter (if we have one)
			for (; It; ++It)
			{
				UProperty* Param = *It;
				if (Param->GetPropertyFlags() & CPF_ReturnParm)
				{
					return FetchProperty(Param, NumArgs);
				}
			}
		}

		// No return value available
		return chakra::Undefined();
	}

	void ExportFunction(JsValueRef Template, UFunction* FunctionToExport)
	{
		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			// arguments[0].prototype is Holder
			JsValueRef self = arguments[0];
			JsValueType type1, type2;
			JsCheck(JsGetValueType(self, &type1));
			JsCheck(JsGetValueType(arguments[0], &type2));

			bool external1, external2;
			JsCheck(JsHasExternalData(self, &external1));
			JsCheck(JsHasExternalData(self, &external2));


			// Retrieve "FUNCTION"
			UFunction* Function = reinterpret_cast<UFunction*>(callbackState);

			// Determine 'this'
			UObject* Object = (Function->FunctionFlags & FUNC_Static) ? Function->GetOwnerClass()->ClassDefaultObject : chakra::UObjectFromChakra(self);

			// Check 'this' is valid
			if (!Object->IsValidLowLevelFast())
			{
				chakra::Throw(FString::Printf(TEXT("Invalid instance for calling a function %s"), *Function->GetName()));
				return chakra::Undefined();
			}

			// Call unreal engine function!
			return CallFunction(self, Function, Object, [&](int ArgIndex) {
				// pass an argument if we have
				if (ArgIndex < argumentCount - 1)
				{
					return arguments[ArgIndex + 1];
				}
				// Otherwise, just return undefined.
				else
				{
					return chakra::Undefined();
				}
			});
		};

		FString function_name = FunctionToExport->GetName();
		JsValueRef function = chakra::FunctionTemplate(FunctionBody, FunctionToExport);

		// In case of static function, you can also call this function by 'Class.Method()'.
		if (FunctionToExport->FunctionFlags & FUNC_Static)
		{
			chakra::SetProperty(Template, function_name, function);
		}

		// Register the function to prototype template
		JsValueRef prototypeTmpl = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(prototypeTmpl, function_name, function);
	}

	void ExportBlueprintLibraryFunction(JsValueRef Template, UFunction* FunctionToExport)
	{
		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			// arguments[0].prototype is Holder
			JsValueRef self = arguments[0];

			// Retrieve "FUNCTION"
			UFunction* Function = reinterpret_cast<UFunction*>(callbackState);

			// 'this' should be CDO of owner class
			UObject* Object = Function->GetOwnerClass()->ClassDefaultObject;

			// Call unreal engine function!
			return CallFunction(self, Function, Object, [&](int ArgIndex) {
				// The first argument is bound automatically
				if (ArgIndex == 0)
				{
					return self;
				}
				// The rest arguments are being passed like normal function call.
				else if (ArgIndex < argumentCount)
				{
					return arguments[ArgIndex];
				}
				else
				{
					return chakra::Undefined();
				}
			});
		};

		FString function_name = FunctionToExport->GetName();
		JsValueRef function = chakra::FunctionTemplate(FunctionBody, FunctionToExport);

		// Register the function to prototype template
		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, function_name, function);

	}

	void ExportBlueprintLibraryFactoryFunction(JsValueRef Template, UFunction* FunctionToExport)
	{
		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			JsValueRef self = arguments[0];

			// Retrieve "FUNCTION"
			UFunction* Function = reinterpret_cast<UFunction*>(callbackState);

			// 'this' should be CDO of owner class
			UObject* Object = Function->GetOwnerClass()->ClassDefaultObject;

			// Call unreal engine function!
			return CallFunction(self, Function, Object, [&](int ArgIndex) {
				// pass an argument if we have
				if (ArgIndex + 1 < argumentCount)
				{
					return arguments[ArgIndex + 1];
				}
				// Otherwise, just return undefined.
				else
				{
					return chakra::Undefined();
				}
			});
		};

		FString function_name = FunctionToExport->GetName();
		JsValueRef function = chakra::FunctionTemplate(FunctionBody, FunctionToExport);

		// Register the function to prototype template
		chakra::SetProperty(Template, function_name, function);
	}

	template <typename PropertyAccessors>
	void ExportProperty(JsValueRef Template, UProperty* PropertyToExport, int32 PropertyIndex)
	{
		// Property getter
		auto Getter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			JsValueRef data = reinterpret_cast<JsValueRef>(callbackState);
			UProperty* Property = nullptr;
			JsCheck(JsGetExternalData(data, (void**)&Property));

			return PropertyAccessors::Get(GetSelf(), arguments[0], Property);
		};

		// Property setter
		auto Setter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			JsValueRef data = reinterpret_cast<JsValueRef>(callbackState);
			UProperty* Property = nullptr;
			JsCheck(JsGetExternalData(data, (void**)&Property));

			PropertyAccessors::Set(GetSelf(), arguments[0], Property, arguments[1]);
			return arguments[1];
		};

		chakra::FPropertyDescriptor desc;
		desc.Getter = chakra::FunctionTemplate(Getter, chakra::External(PropertyToExport, nullptr));
		desc.Setter = chakra::FunctionTemplate(Setter, chakra::External(PropertyToExport, nullptr));
		desc.Configurable = false; // don't delete
		desc.Writable = !FV8Config::IsWriteDisabledProperty(PropertyToExport);

		chakra::SetAccessor(Template, PropertyNameToString(PropertyToExport), desc);
	}

	void ExportHelperFunctions(UStruct* ClassToExport, JsValueRef Template)
	{
		// Bind blue print library!
		TArray<UFunction*> Functions;
		BlueprintFunctionLibraryMapping.MultiFind(ClassToExport, Functions);

		for (auto Function : Functions)
		{
			ExportBlueprintLibraryFunction(Template, Function);
		}

		BlueprintFunctionLibraryFactoryMapping.MultiFind(ClassToExport, Functions);
		for (auto Function : Functions)
		{
			ExportBlueprintLibraryFactoryFunction(Template, Function);
		}
	}

	void AddMemberFunction_Class_GetClassObject(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			return GetSelf()->ForceExportObject(ClassToExport);
		};

		chakra::SetProperty(Template, "GetClassObject", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_SetDefaultSubobjectClass(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			const FObjectInitializer* ObjectInitializer = GetSelf()->GetObjectInitializer();

			if (!ObjectInitializer)
			{
				chakra::Throw(TEXT("SetDefaultSubobjectClass must be called within ctor"));
				return chakra::Undefined();
			}

			if (argumentCount < 2)
			{
				chakra::Throw(TEXT("Missing arg"));
				return chakra::Undefined();
			}

			UJavascriptGeneratedClass* Class = static_cast<UJavascriptGeneratedClass*>(ObjectInitializer->GetClass());
			if (!Class->JavascriptContext.IsValid())
			{
				chakra::Throw(TEXT("Fatal"));
				return chakra::Undefined();
			}

			auto Context = Class->JavascriptContext.Pin();
			auto Name = chakra::StringFromChakra(arguments[1]);
			PlaceholderUClass = ClassToExport;
			ObjectInitializer->SetDefaultSubobjectClass<hack_private_key>(*Name);
			PlaceholderUClass = nullptr;

			return chakra::Undefined();
		};

		chakra::SetProperty(Template, "SetDefaultSubobjectClass", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_CreateDefaultSubobject(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			const FObjectInitializer* ObjectInitializer = GetSelf()->GetObjectInitializer();

			if (!ObjectInitializer)
			{
				chakra::Throw(TEXT("CreateDefaultSubobject must be called within ctor"));
				return chakra::Undefined();
			}

			if (argumentCount < 2)
			{
				chakra::Throw(TEXT("Missing arg"));
				return chakra::Undefined();
			}

			UJavascriptGeneratedClass* Class = static_cast<UJavascriptGeneratedClass*>(ObjectInitializer->GetClass());
			if (!Class->JavascriptContext.IsValid())
			{
				chakra::Throw(TEXT("Fatal"));
				return chakra::Undefined();
			}

			auto Context = Class->JavascriptContext.Pin();
			UClass* ReturnType = ClassToExport;
			FString Name = chakra::StringFromChakra(arguments[1]);
			bool bTransient = argumentCount > 2 ? chakra::BoolFrom(arguments[2]) : false;
			bool bIsRequired = argumentCount > 3 ? chakra::BoolFrom(arguments[3]) : true;
			bool bIsAbstract = argumentCount > 4 ? chakra::BoolFrom(arguments[4]) : false;
			auto Object = ObjectInitializer->CreateDefaultSubobject(ObjectInitializer->GetObj(), *Name, ReturnType, ReturnType, bIsRequired, bIsAbstract, bTransient);

			return Context->ExportObject(Object);
		};

		chakra::SetProperty(Template, "CreateDefaultSubobject", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_GetDefaultSubobjectByName(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			FString Name = chakra::StringFromChakra(arguments[1]);

			return GetSelf()->ExportObject(ClassToExport->GetDefaultSubobjectByName(*Name));
		};

		chakra::SetProperty(Template, "GetDefaultSubobjectByName", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_GetDefaultObject(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			return GetSelf()->ExportObject(ClassToExport->GetDefaultObject());
		};

		chakra::SetProperty(Template, "GetDefaultObject", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_Find(JsValueRef Template, UClass* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			if (argumentCount == 3 && chakra::IsString(arguments[2]))
			{
				UObject* Outer = chakra::UObjectFromChakra(arguments[1]);
				UObject* obj = StaticFindObject(ClassToExport, Outer ? Outer : ANY_PACKAGE, *chakra::StringFromChakra(arguments[2]));
				return GetSelf()->ExportObject(obj);
			}
			else
			{
				chakra::Throw(TEXT("Missing resource name to load"));
				return chakra::Undefined();
			}
		};

		chakra::SetProperty(Template, "Find", chakra::FunctionTemplate(fn, ClassToExport));
	}

	void AddMemberFunction_Class_Load(JsValueRef Template, UClass* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			if (argumentCount == 2 && chakra::IsString(arguments[1]))
			{
				UObject* obj = StaticLoadObject(ClassToExport, nullptr, *chakra::StringFromChakra(arguments[1]));
				return GetSelf()->ExportObject(obj);
			}
			else
			{
				chakra::Throw(TEXT("Missing resource name to load"));
				return chakra::Undefined();
			}
		};

		chakra::SetProperty(Template, "Load", chakra::FunctionTemplate(fn, ClassToExport));
	}

	JsValueRef C_Operator(UStruct* StructToExport, JsValueRef Value)
	{
		auto Instance = FStructMemoryInstance::FromChakra(Value);

		// If given value is an instance
		if (Instance)
		{
			UScriptStruct* GivenStruct = Instance->Struct;
			if (Instance->Struct->IsChildOf(StructToExport))
			{
				return Value;
			}
		}
		else if (auto ScriptStruct = Cast<UScriptStruct>(StructToExport))
		{
			if (chakra::IsObject(Value))
			{
				JsValueRef v = Value;
				auto Size = ScriptStruct->GetStructureSize();
				auto Target = (uint8*)(FMemory_Alloca(Size));
				FMemory::Memzero(Target, Size);
				ReadOffStruct(v, ScriptStruct, Target);
				return ExportStructInstance(ScriptStruct, Target, FNoPropertyOwner());
			}
		}

		return chakra::Undefined();
	}

	void AddMemberFunction_Struct_C(JsValueRef Template, UStruct* StructToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UStruct* StructToExport = reinterpret_cast<UStruct*>(callbackState);

			if (argumentCount == 2)
			{
				return GetSelf()->C_Operator(StructToExport, arguments[1]);
			}

			return chakra::Undefined();
		};

		chakra::SetProperty(Template, "C", chakra::FunctionTemplate(fn, StructToExport));
	}

	void AddMemberFunction_JavascriptRef_get(JsValueRef Template)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			JsValueRef self = arguments[0];

			auto Instance = FStructMemoryInstance::FromChakra(self);

			if (Instance->GetMemory())
			{
				FJavascriptRef* Ref = reinterpret_cast<FJavascriptRef*>(Instance->GetMemory());
				if (Ref->Handle.IsValid())
				{
					FPrivateJavascriptRef* Handle = Ref->Handle.Get();
					return Handle->Object.Get();
				}
			}

			return chakra::Undefined();
		};

		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, "get", chakra::FunctionTemplate(fn));
	}

	void AddMemberFunction_Struct_clone(JsValueRef Template, UStruct* StructToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			JsValueRef self = arguments[0];

			auto Instance = FStructMemoryInstance::FromChakra(self);

			if (Instance->GetMemory())
			{
				return GetSelf()->ExportStructInstance(Instance->Struct, Instance->GetMemory(), FNoPropertyOwner());
			}

			return chakra::Undefined();
		};

		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, "clone", chakra::FunctionTemplate(fn));
	}

	template <typename PropertyAccessor>
	void AddMemberFunction_Struct_toJSON(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UStruct* Class = reinterpret_cast<UStruct*>(callbackState);

			JsValueRef self = arguments[0];
			JsValueRef out = JS_INVALID_REFERENCE;
			JsCreateObject(&out);

			// class info
			chakra::SetProperty(out, "__class", chakra::String(Class->GetPathName()));

			if (Class->IsA<UClass>())
			{
				UObject* _self = chakra::UObjectFromChakra(self);
				check(_self);
			}

			for (TFieldIterator<UProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				UProperty* Property = *PropertyIt;

				if (!FV8Config::CanExportProperty(Class, Property))
					continue;

				FString name = PropertyNameToString(Property);
				JsValueRef value = PropertyAccessor::Get(GetSelf(), self, Property);
				if (auto p = Cast<UClassProperty>(Property))
				{
					UClass* Class = chakra::UClassFromChakra(value);

					if (Class)
					{
						UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class);
						if (BPGC)
						{
							UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy);
							value = chakra::String(BP->GetPathName());
						}
						else
						{
							value = chakra::String(Class->GetPathName());
						}
					}
					else
					{
						value = chakra::String("null");
					}

					chakra::SetProperty(out, name, value);
				}
				else
				{
					chakra::SetProperty(out, name, value);
				}
			}

			return out;
		};

		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, "toJSON", chakra::FunctionTemplate(fn, ClassToExport));
	}

	template <typename PropertyAccessor>
	void AddMemberFunction_Struct_RawAccessor(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* Class = reinterpret_cast<UClass*>(callbackState);

			if (argumentCount != 3 || !chakra::IsFunction(arguments[2])) return chakra::Undefined();

			const FName PropertyNameToAccess(*chakra::StringFromChakra(arguments[1]));
			JsValueRef function = arguments[2];

			JsValueRef self = arguments[0];
			auto Instance = PropertyAccessor::This(self);

			for (TFieldIterator<UProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				UProperty* Property = *PropertyIt;

				if (auto p = Cast<UArrayProperty>(Property))
				{
					FScriptArrayHelper_InContainer helper(p, Instance);

					if (FV8Config::CanExportProperty(Class, Property) && MatchPropertyName(Property, PropertyNameToAccess))
					{
						JsValueRef argv[2] = { self };
						JsCheck(JsCreateExternalArrayBuffer(helper.GetRawPtr(), helper.Num(), nullptr, nullptr, &argv[1]));

						JsValueRef returnValue = JS_INVALID_REFERENCE;
						JsCheck(JsCallFunction(function, argv, 2, &returnValue));

						return returnValue;
					}
				}
			}

			return chakra::Undefined();
		};

		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, "$memaccess", chakra::FunctionTemplate(fn, ClassToExport));
	}

	JsValueRef InternalExportClass(UClass* ClassToExport)
	{
		auto ConstructorBody = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			UClass* ClassToExport = reinterpret_cast<UClass*>(callbackState);

			if (isConstructCall)
			{
				JsValueRef self = arguments[0];

				UObject* Associated = nullptr;

				// Called by system (via ExportObject)
				if (argumentCount == 2 && chakra::IsExternal(arguments[1]))
				{
					JsCheck(JsGetExternalData(arguments[1], reinterpret_cast<void**>(&Associated)));

					if (!Associated->IsValidLowLevel())
					{
						Associated = nullptr;
					}
				}

				// Called by user (via 'new' operator)
				if (Associated == nullptr)
				{
					const bool bIsJavascriptClass =
						ClassToExport->GetClass()->IsChildOf(UJavascriptGeneratedClass::StaticClass()) ||
						ClassToExport->GetClass()->IsChildOf(UJavascriptGeneratedClass_Native::StaticClass());

					auto PreCreate = [&]() {
						if (bIsJavascriptClass)
						{
							GetSelf()->ObjectUnderConstructionStack.Push(FPendingClassConstruction(self, ClassToExport));
						}
					};

					// Custom constructors
					if (ClassToExport->IsChildOf(AActor::StaticClass()))
					{
						if (argumentCount == 1)
						{
							chakra::Throw(TEXT("Missing world to spawn"));
							return chakra::Undefined();
						}

						UWorld* World = Cast<UWorld>(chakra::UObjectFromChakra(arguments[1]));
						if (!World)
						{
							chakra::Throw(TEXT("Missing world to spawn"));
							return chakra::Undefined();
						}

						FVector Location(ForceInitToZero);
						FRotator Rotation(ForceInitToZero);

						UPackage* CoreUObjectPackage = UObject::StaticClass()->GetOutermost();
						static UScriptStruct* VectorStruct = FindObjectChecked<UScriptStruct>(CoreUObjectPackage, TEXT("Vector"));
						static UScriptStruct* RotatorStruct = FindObjectChecked<UScriptStruct>(CoreUObjectPackage, TEXT("Rotator"));
						static TStructReader<FVector> VectorReader(VectorStruct);
						static TStructReader<FRotator> RotatorReader(RotatorStruct);

						FActorSpawnParameters SpawnInfo;
						switch (FMath::Min(3, int(argumentCount))) {
						case 3:
							if (!VectorReader.Read(arguments[2], Location)) return chakra::Undefined();
							if (argumentCount == 3) break;
						case 4:
							if (!RotatorReader.Read(arguments[3], Rotation)) return chakra::Undefined();
							if (argumentCount == 4) break;
						case 5:
							SpawnInfo.Name = FName(*chakra::StringFromChakra(arguments[4]));
							if (argumentCount == 5) break;
						case 6:
							SpawnInfo.ObjectFlags = RF_Transient | RF_Transactional;
							break;
						default:
							break;
						}

						PreCreate();
						Associated = World->SpawnActor(ClassToExport, &Location, &Rotation, SpawnInfo);
#if WITH_EDITOR
						if (SpawnInfo.Name != NAME_None)
							(Cast<AActor>(Associated))->SetActorLabel(chakra::StringFromChakra(arguments[4]));
#endif
					}
					else
					{
						UObject* Outer = GetTransientPackage();
						FName Name = NAME_None;
						EObjectFlags ObjectFlags = RF_NoFlags;

						if (argumentCount > 1)
						{
							if (UObject* value = chakra::UObjectFromChakra(arguments[1]))
							{
								Outer = value;
							}
							if (argumentCount > 2)
							{
								Name = FName(*chakra::StringFromChakra(arguments[2]));
							}
							if (argumentCount > 3)
							{
								ObjectFlags = (EObjectFlags)(chakra::IntFrom(arguments[3]));
							}
						}

						PreCreate();
						Associated = NewObject<UObject>(Outer, ClassToExport, Name, ObjectFlags);
					}

					if (bIsJavascriptClass)
					{
						const auto& Last = GetSelf()->ObjectUnderConstructionStack.Last();

						bool bSafeToQuit = Last.bCatched;

						GetSelf()->ObjectUnderConstructionStack.Pop();

						if (bSafeToQuit)
						{
							return chakra::Undefined();
						}
					}

					if (!Associated)
					{
						chakra::Throw(TEXT("Failed to spawn"));
						return chakra::Undefined();
					}
				}

				FPendingClassConstruction(self, ClassToExport).Finalize(GetSelf(), Associated);
				return chakra::Undefined();
			}
			else
			{
				return GetSelf()->C_Operator(ClassToExport, arguments[1]);
			}
		};

		JsValueRef Template = JS_INVALID_REFERENCE;
		JsCheck(JsCreateNamedFunction(chakra::String(ClassToExport->GetName()), ConstructorBody, ClassToExport, &Template));
		//Template->InstanceTemplate()->SetInternalFieldCount(1);
		//Template->SetClassName(I.Keyword(ClassToExport->GetName()));

		AddMemberFunction_Struct_C(Template, ClassToExport);

		// load
		if (!ClassToExport->IsChildOf(AActor::StaticClass()))
		{
			AddMemberFunction_Class_Load(Template, ClassToExport);
		}
		AddMemberFunction_Class_Find(Template, ClassToExport);

		AddMemberFunction_Class_GetClassObject(Template, ClassToExport);
		AddMemberFunction_Class_CreateDefaultSubobject(Template, ClassToExport);
		AddMemberFunction_Class_SetDefaultSubobjectClass(Template, ClassToExport);

		AddMemberFunction_Class_GetDefaultObject(Template, ClassToExport);
		AddMemberFunction_Class_GetDefaultSubobjectByName(Template, ClassToExport);

		AddMemberFunction_Struct_toJSON<FObjectPropertyAccessors>(Template, ClassToExport);
		AddMemberFunction_Struct_RawAccessor<FObjectPropertyAccessors>(Template, ClassToExport);


		FString static_class = "StaticClass";

		// access thru Class.prototype.StaticClass
		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, static_class, chakra::External(ClassToExport, nullptr));
		chakra::SetProperty(Template, static_class, chakra::External(ClassToExport, nullptr));

		for (TFieldIterator<UFunction> FuncIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (FV8Config::CanExportFunction(ClassToExport, Function))
			{
				ExportFunction(Template, Function);
			}
		}

		int32 PropertyIndex = 0;
		for (TFieldIterator<UProperty> PropertyIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt, ++PropertyIndex)
		{
			UProperty* Property = *PropertyIt;
			if (FV8Config::CanExportProperty(ClassToExport, Property))
			{
				ExportProperty<FObjectPropertyAccessors>(Template, Property, PropertyIndex);
			}
		}

		return Template;
	}

	JsValueRef InternalExportStruct(UScriptStruct* StructToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			auto StructToExport = reinterpret_cast<UScriptStruct*>(callbackState);

			if (isConstructCall)
			{
				JsValueRef self = arguments[0];

				TSharedPtr<FStructMemoryInstance> Memory;

				if (argumentCount == 3 && chakra::IsExternal(arguments[1]) && chakra::IsExternal(arguments[2]))
				{
					IPropertyOwner* Owner;
					JsCheck(JsGetExternalData(arguments[2], reinterpret_cast<void**>(&Owner)));

					void* Source;
					JsCheck(JsGetExternalData(arguments[1], &Source));
					Memory = FStructMemoryInstance::Create(StructToExport, *Owner, Source);
				}
				else
				{
					Memory = FStructMemoryInstance::Create(StructToExport, FNoPropertyOwner());
				}

				GetSelf()->RegisterScriptStructInstance(Memory, self);
				chakra::SetProperty(self, "__self", chakra::External(Memory.Get(), nullptr));

				return chakra::Undefined();

				//self->SetAlignedPointerInInternalField(0, Memory.Get());
			}
			else
			{
				return GetSelf()->C_Operator(StructToExport, arguments[1]);
			}
		};

		JsValueRef Template = JS_INVALID_REFERENCE;
		JsCheck(JsCreateNamedFunction(chakra::String(StructToExport->GetName()), fn, StructToExport, &Template));
		//Template->InstanceTemplate()->SetInternalFieldCount(1);
		//Template->SetClassName(I.Keyword(StructToExport->GetName()));

		AddMemberFunction_Struct_C(Template, StructToExport);
		AddMemberFunction_Struct_clone(Template, StructToExport);
		AddMemberFunction_Struct_toJSON<FStructPropertyAccessors>(Template, StructToExport);
		AddMemberFunction_Struct_RawAccessor<FStructPropertyAccessors>(Template, StructToExport);

		if (StructToExport == FJavascriptRef::StaticStruct())
		{
			AddMemberFunction_JavascriptRef_get(Template);
		}

		FString static_class = "StaticClass";

		// access thru Class.prototype.StaticClass
		JsValueRef templateProto = chakra::GetProperty(Template, "prototype");
		chakra::SetProperty(templateProto, static_class, chakra::External(StructToExport, nullptr));
		chakra::SetProperty(Template, static_class, chakra::External(StructToExport, nullptr));

		int32 PropertyIndex = 0;
		for (TFieldIterator<UProperty> PropertyIt(StructToExport, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt, ++PropertyIndex)
		{
			UProperty* Property = *PropertyIt;
			if (FV8Config::CanExportProperty(StructToExport, Property))
			{
				ExportProperty<FStructPropertyAccessors>(Template, Property, PropertyIndex);
			}
		}

		return Template;
	}

	virtual JsValueRef ExportStruct(UScriptStruct* ScriptStruct) override
	{
		auto ExportedFunctionTemplatePtr = ScriptStructToFunctionTemplateMap.Find(ScriptStruct);
		if (ExportedFunctionTemplatePtr == nullptr)
		{
			JsValueRef Template = InternalExportStruct(ScriptStruct);

			UScriptStruct* SuperStruct = Cast<UScriptStruct>(ScriptStruct->GetSuperStruct());
			if (SuperStruct)
			{
				chakra::Inherit(Template, ExportStruct(SuperStruct));
			}

			ExportHelperFunctions(ScriptStruct, Template);

			RegisterStruct(ScriptStructToFunctionTemplateMap, ScriptStruct, Template);

			return Template;
		}
		else
		{
			return ExportedFunctionTemplatePtr->Get();
		}
	}

	JsValueRef ExportEnum(UEnum* Enum)
	{
		int MaxEnumValue = Enum->GetMaxEnumValue();
		JsValueRef arr = JS_INVALID_REFERENCE;
		JsCheck(JsCreateArray(MaxEnumValue, &arr));

		for (decltype(MaxEnumValue) Index = 0; Index < MaxEnumValue; ++Index)
		{
			FString name = Enum->GetNameStringByIndex(Index);
			JsValueRef value = chakra::String(name);
			chakra::SetIndex(arr, Index, chakra::String(name));
			chakra::SetProperty(arr, name, value);
		}

		// public name
		FString enumName = FV8Config::Safeify(Enum->GetName());
		chakra::SetProperty(GetGlobalTemplate(), enumName, arr);

		return arr;
	}

	virtual JsValueRef ExportClass(UClass* Class, bool bAutoRegister = true) override
	{
		auto ExportedFunctionTemplatePtr = ClassToFunctionTemplateMap.Find(Class);
		if (ExportedFunctionTemplatePtr == nullptr)
		{
			JsValueRef Template = InternalExportClass(Class);

			UClass* SuperClass = Class->GetSuperClass();
			if (SuperClass)
			{
				chakra::Inherit(Template, ExportClass(SuperClass));
			}

			ExportHelperFunctions(Class, Template);

			if (bAutoRegister)
			{
				RegisterClass(Class, Template);
			}

			return Template;
		}
		else
		{
			return ExportedFunctionTemplatePtr->Get();
		}
	}

	static void FinalizeScriptStruct(JsRef ref, void* data)
	{
		auto self = GetSelf();
		if (self == nullptr)
			return;

		FStructMemoryInstance* memory = reinterpret_cast<FStructMemoryInstance*>(data);
		self->OnGarbageCollectedByChakra(memory);
	}

	static void FinalizeUObject(JsRef ref, void* data)
	{
		auto self = GetSelf();
		if (self == nullptr)
			return;

		UObject* memory = reinterpret_cast<UObject*>(data);
		self->OnGarbageCollectedByChakra(memory);
	}

	JsValueRef ExportStructInstance(UScriptStruct* Struct, uint8* Buffer, const IPropertyOwner& Owner)
	{
		if (Struct == nullptr || Buffer == nullptr)
			return chakra::Undefined();

		JsValueRef v8_struct = ExportStruct(Struct);
		JsValueRef args[] = {
			chakra::Undefined(),
			chakra::External(Buffer, nullptr),
			chakra::External((void*)&Owner, nullptr)
		};

		JsValueRef obj = chakra::New(v8_struct, args, 3);
		if (chakra::IsEmpty(obj))
			return chakra::Undefined();

		return obj;
	}

	JsValueRef ForceExportObject(UObject* Object)
	{
		if (Object == nullptr)
			return chakra::Undefined();

		auto ObjectPtr = ObjectToObjectMap.Find(Object);
		if (ObjectPtr == nullptr)
		{
			JsValueRef v8_class = ExportClass(Object->GetClass());
			JsValueRef args[] = {
				chakra::Undefined(),
				chakra::External(Object, nullptr)
			};

			return chakra::New(v8_class, args, 2);
		}
		else
		{
			return ObjectPtr->Get();
		}
	}

	JsValueRef ExportObject(UObject* Object, bool bForce = false) override
	{
		if (bForce)
			return ForceExportObject(Object);

		if (Object == nullptr)
			return chakra::Undefined();

		if (UClass* Class = Cast<UClass>(Object))
		{
			return ExportClass(Class);
		}
		else if (UScriptStruct* Struct = Cast<UScriptStruct>(Object))
		{
			return ExportStruct(Struct);
		}
		else if (auto ObjectPtr = ObjectToObjectMap.Find(Object))
		{
			return ObjectPtr->Get();
		}

		if (ObjectUnderConstructionStack.Num() > 0)
		{
			auto& Last = ObjectUnderConstructionStack.Last();
			if (!Last.bCatched)
			{
				if (!Object->HasAnyFlags(RF_ClassDefaultObject) && Object->IsA(Last.Class))
				{
					Last.bCatched = true;
					Last.Finalize(this, Object);
					return Last.Object.Get();
				}
			}
		}

		UClass* Class = Object->GetClass();
		//if (Class->ClassGeneratedBy && Cast<ULevel>(Class->ClassGeneratedBy->GetOuter()))
		//{
		//	return Undefined(isolate_);
		//}

		JsValueRef v8_class = ExportClass(Class);
		JsValueRef args[] = {
			chakra::Undefined(),
			chakra::External(Object, nullptr)
		};

		return chakra::New(v8_class, args, 2);
	}

	template <typename StructType>
	void RegisterStruct(TMap< StructType*, Persistent<JsValueRef> >& TheMap, StructType* Class, JsValueRef Template)
	{
		// public name
		FString name = FV8Config::Safeify(Class->GetName());

		// If we are running in a context, we also register this class to the context directly.
		JsValueRef global = JS_INVALID_REFERENCE;
		JsCheck(JsGetGlobalObject(&global));
		chakra::SetProperty(global, name, Template);

		// Register this class to the global template so that any other contexts which will be created later have this function template.
		chakra::SetProperty(GetGlobalTemplate(), name, Template);

		// Track this class from v8 gc.
		TheMap.Add(Class, Template);
		JsCheck(JsSetObjectBeforeCollectCallback(Template, Class, FinalizeUObject));
	}

	virtual void RegisterClass(UClass* Class, JsValueRef Template) override
	{
		RegisterStruct(ClassToFunctionTemplateMap, Class, Template);
	}

	void RegisterScriptStruct(UScriptStruct* Struct, JsValueRef Template)
	{
		RegisterStruct(ScriptStructToFunctionTemplateMap, Struct, Template);
	}

	void RegisterObject(UObject* UnrealObject, JsValueRef value)
	{
		ObjectToObjectMap.Add(UnrealObject, value);

		// track gc
		JsCheck(JsSetObjectBeforeCollectCallback(value, UnrealObject, FinalizeUObject));
	}

	void RegisterScriptStructInstance(TSharedPtr<FStructMemoryInstance> MemoryObject, JsValueRef value)
	{
		MemoryToObjectMap.Add(MemoryObject, value);

		// track gc
		JsCheck(JsSetObjectBeforeCollectCallback(value, MemoryObject.Get(), FinalizeScriptStruct));
	}

	void OnGarbageCollectedByChakra(FStructMemoryInstance* Memory)
	{
		// We should keep ourselves clean
		MemoryToObjectMap.Remove(Memory->AsShared());
	}

	void OnGarbageCollectedByChakra(UObject* Object)
	{
		if (!Object->IsValidLowLevelFast())
			return;

		if (UClass* klass = Cast<UClass>(Object))
		{
			ClassToFunctionTemplateMap.Remove(klass);
		}

		ObjectToObjectMap.Remove(Object);
	}

	static FJavascriptContextImplementation* GetSelf()
	{
		JsContextRef context = JS_INVALID_REFERENCE;
		JsCheck(JsGetCurrentContext(&context));

		if (context == JS_INVALID_REFERENCE)
			return nullptr;

		FJavascriptContextImplementation* jsContext = nullptr;
		JsCheck(JsGetContextData(context, reinterpret_cast<void**>(&jsContext)));

		return jsContext;
	}
};

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<bool>(const bool& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::Boolean(cValue);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<int32>(const int32& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::Int(cValue);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<uint32>(const uint32& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::Int(cValue);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<float>(const float& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::Double(cValue);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<FString>(const FString& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::String(cValue);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<FText>(const FText& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	FName TableID;
	FString Key;

	if (FTextInspector::GetTableIdAndKey(cValue, TableID, Key))
	{
		JsValueRef tableBuilder = RunScript("", "(table, key) => new FText(table, key)");
		JsValueRef args[] = { chakra::Undefined(), chakra::String(TableID.ToString()), chakra::String(Key) };
		JsValueRef ret = JS_INVALID_REFERENCE;
		JsCheck(JsCallFunction(tableBuilder, args, 3, &ret));

		return ret;
	}

	FString Namespace = FTextInspector::GetNamespace(cValue).Get("");
	Key = FTextInspector::GetKey(cValue).Get("");
	const FString *Source = FTextInspector::GetSourceString(cValue);

	if (!Namespace.IsEmpty())
	{
		JsValueRef namespaceBuilder = RunScript("", "(ns, key, src) => new FText(ns, key, src)");
		JsValueRef args[] = { chakra::Undefined(), chakra::String(Namespace), chakra::String(Key), chakra::String(*Source) };
		JsValueRef ret = JS_INVALID_REFERENCE;
		JsCheck(JsCallFunction(namespaceBuilder, args, 4, &ret));

		return ret;
	}
	else
	{
		JsValueRef stringBuilder = RunScript("", "str => new FText(str)");
		JsValueRef args[] = { chakra::Undefined(), chakra::String(*Source) };
		JsValueRef ret = JS_INVALID_REFERENCE;
		JsCheck(JsCallFunction(stringBuilder, args, 2, &ret));

		return ret;
	}
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<FName>(const FName& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return chakra::String(cValue.ToString());
}

// for enum
template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<uint8>(const uint8& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	UByteProperty* byteProperty = Cast<UByteProperty>(Property);
	if (byteProperty && byteProperty->Enum)
	{
		return chakra::String(byteProperty->Enum->GetNameStringByIndex(cValue));
	}
	else if (UEnumProperty* enumProperty = Cast<UEnumProperty>(Property))
	{
		return chakra::String(enumProperty->GetEnum()->GetNameStringByIndex(cValue));
	}
	else
	{
		return chakra::Int(cValue);
	}
}

template<>
JsValueRef FJavascriptContextImplementation::ConvertValue<FScriptArray>(const FScriptArray& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	check(Property->IsA<UArrayProperty>());

	UArrayProperty* arrayProperty = Cast<UArrayProperty>(Property);
	FScriptArrayHelper helper(arrayProperty, reinterpret_cast<const void*>(&cValue));
	auto len = (uint32_t)(helper.Num());

	JsValueRef arr = JS_INVALID_REFERENCE;
	JsCheck(JsCreateArray(len, &arr));

	auto Inner = arrayProperty->Inner;

	if (Inner->IsA(UStructProperty::StaticClass()))
	{
		uint8* ElementBuffer = (uint8*)FMemory_Alloca(Inner->GetSize());
		for (decltype(len) Index = 0; Index < len; ++Index)
		{
			Inner->InitializeValue(ElementBuffer);
			Inner->CopyCompleteValueFromScriptVM(ElementBuffer, helper.GetRawPtr(Index));
			chakra::SetIndex(arr, Index, ReadProperty(Inner, ElementBuffer, FNoPropertyOwner()));
			Inner->DestroyValue(ElementBuffer);
		}
	}
	else
	{
		for (decltype(len) Index = 0; Index < len; ++Index)
		{
			chakra::SetIndex(arr, Index, ReadProperty(Inner, helper.GetRawPtr(Index), Owner));
		}
	}

	return arr;
}

template<>
JsValueRef FJavascriptContextImplementation::ConvertValue<FScriptSet>(const FScriptSet& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	check(Property->IsA<USetProperty>());
	USetProperty* setProperty = Cast<USetProperty>(Property);
	FScriptSetHelper SetHelper(setProperty, reinterpret_cast<const void*>(&cValue));

	int Num = SetHelper.Num();
	JsValueRef Out = JS_INVALID_REFERENCE;
	JsCheck(JsCreateArray(Num, &Out));

	for (int Index = 0; Index < Num; ++Index)
	{
		auto PairPtr = SetHelper.GetElementPtr(Index);

		chakra::SetIndex(Out, Index, InternalReadProperty(setProperty->ElementProp, SetHelper.GetElementPtr(Index), Owner));
	}

	return Out;
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<UClass*>(UClass* const& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	if (cValue == nullptr)
		return chakra::Null();

	return ExportClass(const_cast<UClass*>(cValue));
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<FSoftObjectPtr>(FSoftObjectPtr const& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	return ExportStructInstance(TBaseStructure<FSoftObjectPath>::Get(), (uint8*)&cValue.ToSoftObjectPath(), Owner);
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<UObject*>(UObject* const& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	UObjectPropertyBase* objProperty = CastChecked<UObjectPropertyBase>(Property);
	return ExportObject(objProperty->GetObjectPropertyValue(&cValue));
}

template <>
JsValueRef FJavascriptContextImplementation::ConvertValue<FScriptMap>(const FScriptMap& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	check(Property->IsA<UMapProperty>());
	UMapProperty* mapProperty = Cast<UMapProperty>(Property);
	FScriptMapHelper MapHelper(mapProperty, reinterpret_cast<const void*>(&cValue));

	JsValueRef Out = JS_INVALID_REFERENCE;
	JsCheck(JsCreateObject(&Out));

	int Num = MapHelper.Num();
	for (int Index = 0; Index < Num; ++Index)
	{
		uint8* PairPtr = MapHelper.GetPairPtr(Index);

		JsValueRef Key = InternalReadProperty(mapProperty->KeyProp, PairPtr + mapProperty->MapLayout.KeyOffset, Owner);
		JsValueRef Value = InternalReadProperty(mapProperty->ValueProp, PairPtr, Owner);

		JsCheck(JsSetIndexedProperty(Out, Key, Value));
	}

	return Out;
}

template<>
JsValueRef FJavascriptContextImplementation::ConvertValue<FStructDummy>(FStructDummy const& cValue, UProperty* Property, const IPropertyOwner& Owner)
{
	check(Property->IsA<UStructProperty>());
	UStructProperty* structProperty = Cast<UStructProperty>(Property);
	if (structProperty->Struct == nullptr)
	{
		UE_LOG(Javascript, Warning, TEXT("Non ScriptStruct found : %s"), *structProperty->Struct->GetName());
		return chakra::Undefined();
	}

	return ExportStructInstance(structProperty->Struct, (uint8*)&cValue, Owner);
}

template<>
void FJavascriptContextImplementation::FromValue<bool>(bool* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UBoolProperty>(Property)->SetPropertyValue(Ptr, chakra::BoolFrom(Value));
}

template<>
void FJavascriptContextImplementation::FromValue<int32>(int32* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UNumericProperty>(Property)->SetIntPropertyValue(Ptr, (int64)chakra::IntFrom(Value));
}

template<>
void FJavascriptContextImplementation::FromValue<uint32>(uint32* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UNumericProperty>(Property)->SetIntPropertyValue(Ptr, (uint64)chakra::IntFrom(Value));
}

template<>
void FJavascriptContextImplementation::FromValue<float>(float* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UNumericProperty>(Property)->SetFloatingPointPropertyValue(Ptr, (double)chakra::DoubleFrom(Value));
}

template<>
void FJavascriptContextImplementation::FromValue<FString>(FString* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UStrProperty>(Property)->SetPropertyValue(Ptr, chakra::StringFromChakra(Value));
}

template<>
void FJavascriptContextImplementation::FromValue<FText>(FText* Ptr, UProperty* Property, JsValueRef Value)
{
	if (chakra::IsString(Value))
	{
		Cast<UTextProperty>(Property)->SetPropertyValue(Ptr, FText::FromString(chakra::StringFromChakra(Value)));
	}
	else if (chakra::IsObject(Value))
	{
		FText Text = FText::GetEmpty();
		if (chakra::HasProperty(Value, "Table"))
		{
			JsValueRef Table = chakra::GetProperty(Value, "Table");
			JsValueRef Key = chakra::GetProperty(Value, "Key");
			Text = FText::FromStringTable(*chakra::StringFromChakra(Table), chakra::StringFromChakra(Key));
		}
		else if (chakra::HasProperty(Value, "Namespace"))
		{
			JsValueRef Source = chakra::GetProperty(Value, "Source");
			JsValueRef Namespace = chakra::GetProperty(Value, "Namespace");
			JsValueRef Key = chakra::GetProperty(Value, "Key");

			if (!FText::FindText(chakra::StringFromChakra(Namespace), chakra::StringFromChakra(Key), Text))
				Text = FText::FromString(chakra::StringFromChakra(Source));
		}
		else if (chakra::HasProperty(Value, "Source"))
		{
			Text = FText::FromString(chakra::StringFromChakra(chakra::GetProperty(Value, "Source")));
		}
		else
		{
			UE_LOG(Javascript, Warning, TEXT("Could not detext text type from object"));
		}

		Cast<UTextProperty>(Property)->SetPropertyValue(Ptr, Text);
	}
}

template<>
void FJavascriptContextImplementation::FromValue<FName>(FName* Ptr, UProperty* Property, JsValueRef Value)
{
	Cast<UNameProperty>(Property)->SetPropertyValue(Ptr, FName(*chakra::StringFromChakra(Value)));
}

template<>
void FJavascriptContextImplementation::FromValue<uint8>(uint8* Ptr, UProperty* Property, JsValueRef Value)
{
	UByteProperty* byteProperty = Cast<UByteProperty>(Property);
	if (byteProperty && byteProperty->Enum)
	{
		FString Str = chakra::StringFromChakra(Value);
		int EnumValue = byteProperty->Enum->GetIndexByName(FName(*Str), EGetByNameFlags::None);
		if (EnumValue == INDEX_NONE)
		{
			chakra::Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *byteProperty->Enum->GetName()));
		}
		else
		{
			byteProperty->SetPropertyValue(Ptr, EnumValue);
		}
	}
	else if (UEnumProperty* enumProperty = Cast<UEnumProperty>(Property))
	{
		FString Str = chakra::StringFromChakra(Value);
		int EnumValue = enumProperty->GetEnum()->GetIndexByName(FName(*Str), EGetByNameFlags::None);
		if (EnumValue == INDEX_NONE)
		{
			chakra::Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *enumProperty->GetEnum()->GetName()));
		}
		else
		{
			enumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, (int64)EnumValue);
		}
	}
	else if (byteProperty)
	{
		byteProperty->SetPropertyValue(Ptr, chakra::IntFrom(Value));
	}
}

template <>
void FJavascriptContextImplementation::FromValue<FScriptArray>(FScriptArray* Ptr, UProperty* Property, JsValueRef Value)
{
	UArrayProperty* arrayProperty = Cast<UArrayProperty>(Property);
	check(arrayProperty);

	if (chakra::IsArray(Value))
	{
		JsValueRef arr = Value;
		int len = chakra::Length(arr);

		FScriptArrayHelper helper(arrayProperty, Ptr);

		// synchronize the length
		int CurSize = helper.Num();
		if (CurSize < len)
		{
			helper.AddValues(len - CurSize);
		}
		else if (CurSize > len)
		{
			helper.RemoveValues(len, CurSize - len);
		}

		for (decltype(len) Index = 0; Index < len; ++Index)
		{
			WriteProperty(arrayProperty->Inner, helper.GetRawPtr(Index), chakra::GetIndex(arr, Index));
		}
	}
	else
	{
		chakra::Throw(TEXT("Should write into array by passing an array instance"));
	}
}

template <>
void FJavascriptContextImplementation::FromValue<FScriptSet>(FScriptSet* Ptr, UProperty* Property, JsValueRef Value)
{
	USetProperty* setProperty = Cast<USetProperty>(Property);
	check(setProperty);

	if (chakra::IsArray(Value))
	{
		JsValueRef arr = Value;
		int len = chakra::Length(arr);

		FScriptSetHelper SetHelper(setProperty, Ptr);

		auto Num = SetHelper.Num();
		for (int Index = 0; Index < Num; ++Index)
		{
			const int32 ElementIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
			uint8* ElementPtr = SetHelper.GetElementPtr(Index);
			InternalWriteProperty(setProperty->ElementProp, ElementPtr, chakra::GetIndex(arr, Index));
		}

		SetHelper.Rehash();
	}
}

template <>
void FJavascriptContextImplementation::FromValue<FScriptMap>(FScriptMap* Ptr, UProperty* Property, JsValueRef Value)
{
	UMapProperty* mapProperty = Cast<UMapProperty>(Property);
	check(mapProperty);

	if (chakra::IsObject(Value))
	{
		FScriptMapHelper MapHelper(mapProperty, Ptr);

		JsValueRef v = Value;
		JsValueRef PropertyNames = JS_INVALID_REFERENCE;
		JsCheck(JsGetOwnPropertyNames(v, &PropertyNames));
		int Num = chakra::Length(PropertyNames);
		for (decltype(Num) Index = 0; Index < Num; ++Index) {
			JsValueRef Key = chakra::GetIndex(PropertyNames, Index);
			JsValueRef Value = chakra::GetProperty(v, chakra::StringFromChakra(Key));

			int ElementIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
			MapHelper.Rehash();

			uint8* PairPtr = MapHelper.GetPairPtr(ElementIndex);
			InternalWriteProperty(mapProperty->KeyProp, PairPtr + mapProperty->MapLayout.KeyOffset, Key);
			InternalWriteProperty(mapProperty->ValueProp, PairPtr, Value);
		}
	}
}

template <>
void FJavascriptContextImplementation::FromValue<FSoftObjectPtr>(FSoftObjectPtr* Ptr, UProperty* Property, JsValueRef Value)
{
	USoftObjectProperty* softObjProperty = Cast<USoftObjectProperty>(Property);
	if (chakra::IsString(Value))
	{
		softObjProperty->SetPropertyValue(Ptr, FSoftObjectPtr(FSoftObjectPath(chakra::StringFromChakra(Value))));
	}
	else if (chakra::IsObject(Value))
	{
		UObject* Instance = chakra::UObjectFromChakra(Value);
		if (Instance)
		{
			softObjProperty->SetPropertyValue(Ptr, FSoftObjectPtr(Instance));
		}
		else
		{
			// maybe path
			JsValueRef AssetPathName = chakra::GetProperty(Value, "AssetPathName");
			JsValueRef SubPathString = chakra::GetProperty(Value, "SubPathString");

			softObjProperty->SetPropertyValue(Ptr, FSoftObjectPtr(FSoftObjectPath(*chakra::StringFromChakra(AssetPathName), chakra::StringFromChakra(SubPathString))));
		}
	}
	else
	{
		// invalid
		softObjProperty->SetObjectPropertyValue(Ptr, nullptr);
	}
}

template <>
void FJavascriptContextImplementation::FromValue<UObject*>(UObject** Ptr, UProperty* Property, JsValueRef Value)
{
	UObjectPropertyBase* objProperty = Cast<UObjectPropertyBase>(Property);
	UObject* obj = chakra::UObjectFromChakra(Value);

	if (obj)
	{
		// object value
		objProperty->SetObjectPropertyValue(Ptr, obj);
	}
	else if (chakra::IsObject(Value))
	{
		// overwrite property
		UObject* target = objProperty->GetObjectPropertyValue(Ptr);
		JsValueRef classValue = chakra::GetProperty(Value, "__class");
		UClass* cls = objProperty->PropertyClass;
		if (chakra::IsString(classValue))
		{
			cls = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, *chakra::StringFromChakra(classValue)));
			check(cls);
		}

		if (target == nullptr)
		{
			target = NewObject<UObject>(GetTransientPackage(), cls);
			objProperty->SetObjectPropertyValue(Ptr, target);
		}

		ReadOffStruct(Value, cls, reinterpret_cast<uint8*>(target));
	}
	else
	{
		// null value
		objProperty->SetObjectPropertyValue(Ptr, obj);
	}
}

template <>
void FJavascriptContextImplementation::FromValue<UClass*>(UClass** Ptr, UProperty* Property, JsValueRef Value)
{
	UClassProperty* classProperty = Cast<UClassProperty>(Property);
	if (chakra::IsString(Value))
	{
		FString UString = chakra::StringFromChakra(Value);
		if (UString == TEXT("null"))
		{
			classProperty->SetPropertyValue(Ptr, nullptr);
		}
		else
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *UString);
			if (UClass* Class = Cast<UClass>(Object))
			{
				classProperty->SetPropertyValue(Ptr, Class);
			}
			else if (UBlueprint* BP = Cast<UBlueprint>(Object))
			{
				auto BPGC = BP->GeneratedClass;
				classProperty->SetPropertyValue(Ptr, BPGC.Get());
			}
			else
			{
				classProperty->SetPropertyValue(Ptr, Object);
			}
		}
	}
	else
	{
		classProperty->SetPropertyValue(Ptr, chakra::UClassFromChakra(Value));
	}
}

template <>
void FJavascriptContextImplementation::FromValue<FStructDummy>(FStructDummy* Ptr, UProperty* Property, JsValueRef Value)
{
	check(Property->IsA<UStructProperty>());
	UStructProperty* structProperty = Cast<UStructProperty>(Property);

	if (UScriptStruct* ScriptStruct = structProperty->Struct)
	{
		auto Instance = FStructMemoryInstance::FromChakra(Value);

		// If given value is an instance
		if (Instance)
		{
			auto GivenStruct = Instance->Struct;

			// Type-checking needed
			if (GivenStruct->IsChildOf(ScriptStruct))
			{
				structProperty->CopyCompleteValue(Ptr, Instance->GetMemory());
			}
			else
			{
				chakra::Throw(FString::Printf(TEXT("Wrong struct type (given:%s), (expected:%s)"), *GivenStruct->GetName(), *ScriptStruct->GetName()));
			}
		}
		else if (ScriptStruct->IsChildOf(FJavascriptFunction::StaticStruct()))
		{
			FJavascriptFunction func;
			if (chakra::IsFunction(Value))
			{
				JsValueRef jsfunc = Value;
				func.Handle = MakeShareable(new FPrivateJavascriptFunction);
				func.Handle->context.Reset(context());
				func.Handle->Function.Reset(jsfunc);
			}
			ScriptStruct->CopyScriptStruct(Ptr, &func);
		}
		else if (ScriptStruct->IsChildOf(FJavascriptRef::StaticStruct()))
		{
			FJavascriptRef ref;

			if (chakra::IsObject(Value))
			{
				JsValueRef jsobj = Value;
				ref.Handle = MakeShareable(new FPrivateJavascriptRef);
				ref.Handle->Object.Reset(jsobj);
			}

			ScriptStruct->CopyScriptStruct(Ptr, &ref);
		}
		// If raw javascript object has been passed,
		else if (chakra::IsObject(Value))
		{
			JsValueRef v8_obj = Value;

			ReadOffStruct(v8_obj, ScriptStruct, reinterpret_cast<uint8*>(Ptr));
		}
		else
		{
			if (ScriptStruct->GetOwnerClass() == nullptr)
				chakra::Throw(FString::Printf(TEXT("Needed struct data : [null] %s"), *ScriptStruct->GetName()));
			else
				chakra::Throw(FString::Printf(TEXT("Needed struct data : %s %s"), *ScriptStruct->GetOwnerClass()->GetName(), *ScriptStruct->GetName()));
		}
	}
	else
	{
		chakra::Throw(TEXT("No ScriptStruct found"));
	}
}

FJavascriptContext* FJavascriptContext::FromChakra(JsContextRef InContext)
{
	if (InContext == JS_INVALID_REFERENCE)
		return nullptr;

	void* ptr = nullptr;
	JsCheck(JsGetContextData(InContext, &ptr));

	return reinterpret_cast<FJavascriptContext*>(ptr);
}

FJavascriptContext* FJavascriptContext::Create(JsRuntimeHandle InRuntime, TArray<FString>& InPaths)
{
	return new FJavascriptContextImplementation(InRuntime, InPaths);
}

// To tell Unreal engine's GC not to destroy these objects!

inline void FJavascriptContextImplementation::AddReferencedObjects(UObject * InThis, FReferenceCollector & Collector)
{
	//FContextScope scope(context());
	RequestV8GarbageCollection(); // just mark

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
			Collector.AddReferencedObject(Object, InThis);
		}
	}

	// All structs
	for (auto It = MemoryToObjectMap.CreateIterator(); It; ++It)
	{
		TSharedPtr<FStructMemoryInstance> StructScript = It.Key();
		if (!StructScript.IsValid() || StructScript->Struct->IsPendingKill())
		{
			It.RemoveCurrent();
		}
		else
		{
			Collector.AddReferencedObject(StructScript->Struct, InThis);
		}
	}

	// All class definitions
	for (auto It = ClassToFunctionTemplateMap.CreateIterator(); It; ++It)
	{
		UClass* Class = It.Key();
		//UE_LOG(Javascript, Log, TEXT("JavascriptIsolate referencing %s / %s %s (gen by %s %s)"), *(Class->GetOuter()->GetName()), *(Class->GetClass()->GetName()), *(Class->GetName()), Class->ClassGeneratedBy ? *(Class->ClassGeneratedBy->GetClass()->GetName()) : TEXT("none"), Class->ClassGeneratedBy ? *(Class->ClassGeneratedBy->GetName()) : TEXT("none"));
		Collector.AddReferencedObject(Class, InThis);
	}

	// All struct definitions
	for (auto It = ScriptStructToFunctionTemplateMap.CreateIterator(); It; ++It)
	{
		Collector.AddReferencedObject(It.Key(), InThis);
	}
}

template <typename CppType>
bool TStructReader<CppType>::Read(JsValueRef Value, CppType& Target) const
{
	auto Instance = FStructMemoryInstance::FromChakra(Value);
	if (Instance && Instance->Struct == ScriptStruct)
	{
		ScriptStruct->CopyScriptStruct(&Target, Instance->GetMemory());
	}
	else if (chakra::IsObject(Value))
	{
		FJavascriptContextImplementation::GetSelf()->ReadOffStruct(Value, ScriptStruct, reinterpret_cast<uint8*>(&Target));
	}
	else
	{
		chakra::Throw(TEXT("couldn't read struct"));
		return false;
	}

	return true;
}

void FPendingClassConstruction::Finalize(FJavascriptContext* Context, UObject* UnrealObject)
{
	static_cast<FJavascriptContextImplementation*>(Context)->RegisterObject(UnrealObject, Object.Get());
	chakra::SetProperty(Object.Get(), "__self", chakra::External(UnrealObject, nullptr));
	//Object->SetAlignedPointerInInternalField(0, UnrealObject);
}

void FJavascriptFunction::Execute()
{
	if (!Handle.IsValid() || chakra::IsEmpty(Handle->Function.Get())) return;

	{
		FPrivateJavascriptFunction* Handle = this->Handle.Get();

		JsValueRef function = Handle->Function.Get();
		if (!chakra::IsEmpty(function))
		{
			JsContextRef context = Handle->context.Get();

			FContextScope context_scope(context);

			JsValueRef returnValue = JS_INVALID_REFERENCE;
			JsErrorCode error = JsCallFunction(function, &function, 1, &returnValue);
			if (error == JsErrorScriptException)
			{
				JsValueRef exception = JS_INVALID_REFERENCE;
				JsCheck(JsGetAndClearException(&exception));

				FJavascriptContext::FromChakra(context)->UncaughtException(FV8Exception::Report(exception));
			}
			else if (error != JsNoError)
			{
				UE_LOG(Javascript, Error, TEXT("exception occured: %d"), (int)error);
			}
		}
	}
}

void FJavascriptFunction::Execute(UScriptStruct* Struct, void* Buffer)
{
	if (!Handle.IsValid() || Handle->Function.IsEmpty()) return;

	{
		FPrivateJavascriptFunction* Handle = this->Handle.Get();

		JsValueRef function = Handle->Function.Get();
		if (!chakra::IsEmpty(function))
		{
			JsContextRef context = Handle->context.Get();

			FContextScope context_scope(context);

			JsValueRef arg = FJavascriptContextImplementation::GetSelf()->ExportStructInstance(Struct, (uint8*)Buffer, FNoPropertyOwner());
			JsValueRef args[] = { function, arg };
			JsValueRef returnValue = JS_INVALID_REFERENCE;
			JsCheck(JsCallFunction(function, args, 2, &returnValue));
		}
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
