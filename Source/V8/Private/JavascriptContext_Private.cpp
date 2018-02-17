
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
#include "ScopedTransaction.h"

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

// HACK FOR ACCESS PRIVATE MEMBERS
class hack_private_key {};
static UClass* PlaceholderUClass;

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

	struct FJsModuleMessage
	{
		FJsModuleMessage() : ReferencingModule(JS_INVALID_REFERENCE), Specifier(JS_INVALID_REFERENCE) {}
		FJsModuleMessage(JsModuleRecord ReferencingModule, JsValueRef Specifier)
			: ReferencingModule(ReferencingModule)
			, Specifier(Specifier)
		{}

		JsModuleRecord ReferencingModule;
		JsValueRef Specifier;
	};
}

class FJavascriptContextImplementation : public FJavascriptContext
{
	friend class UJavascriptContext;

	TArray<const FObjectInitializer*> ObjectInitializerStack;

	IDelegateManager* Delegates;

	Persistent<JsValueRef> GlobalTemplate;

	// Allocator instance should be set for V8's ArrayBuffer's
	//FMallocArrayBufferAllocator AllocatorInstance;

	FTickerDelegate TickDelegate;
	FDelegateHandle TickHandle;
	TArray<JsValueRef> PromiseTasks;
	TQueue<FJsModuleMessage> PendingModules;


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

	int ModuleSourceContext = 0;
	TMap<JsModuleRecord, FString> ModuleDirectories;
	TMap<JsSourceContext, FString> ScriptDirectories;
	TMap<FString, JsModuleRecord> Modules;
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

	void EnqueuePromiseTask(JsValueRef Task)
	{
		JsCheck(JsAddRef(Task, nullptr));
		PromiseTasks.Add(Task);
	}

	FJavascriptContextImplementation(JsRuntimeHandle InRuntime, TArray<FString>& InPaths)
		: Paths(InPaths)
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

		ExposeGlobals();

		Paths = IV8::Get().GetGlobalScriptSearchPaths();
	}

	~FJavascriptContextImplementation()
	{
		PurgeModules();

		ReleaseAllPersistentHandles();

		ResetAsDebugContext();
		DestroyInspector();

		Delegates->Destroy();
		Delegates = nullptr;

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
		ModuleDirectories.Empty();
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
				JsCheck(JsGetOwnPropertyNames(FuncMap, &Keys));

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

	JsErrorCode InitializeModuleInfo(JsValueRef Specifier, JsModuleRecord ModuleRecord)
	{
		JsErrorCode err = JsNoError;
		err = JsSetModuleHostInfo(ModuleRecord, JsModuleHostInfo_FetchImportedModuleCallback, &FJavascriptContextImplementation::FetchImportedModuleCallback);
		if (err != JsNoError) return err;

		err = JsSetModuleHostInfo(ModuleRecord, JsModuleHostInfo_FetchImportedModuleFromScriptCallback, &FJavascriptContextImplementation::FetchImportedModuleFromScriptCallback);
		if (err != JsNoError) return err;

		err = JsSetModuleHostInfo(ModuleRecord, JsModuleHostInfo_NotifyModuleReadyCallback, &FJavascriptContextImplementation::NotifyModuleReadyCallback);
		if (err != JsNoError) return err;

		err = JsSetModuleHostInfo(ModuleRecord, JsModuleHostInfo_HostDefined, Specifier);
		return err;
	}

	// callback when module imported
	static JsErrorCode FetchImportedModuleCallback(JsModuleRecord referencingModule, JsValueRef specifier, JsModuleRecord* dependentModuleRecord)
	{
		return GetSelf()->FetchImportedModuleImpl(referencingModule, specifier, dependentModuleRecord);
	}

	JsErrorCode FetchImportedModuleImpl(JsModuleRecord referencingModule, JsValueRef specifier, JsModuleRecord* dependentModuleRecord)
	{
		if (ModuleDirectories.Contains(referencingModule))
			return FetchImpotedModule(referencingModule, specifier, dependentModuleRecord, ModuleDirectories[referencingModule]);

		return FetchImpotedModule(referencingModule, specifier, dependentModuleRecord, TEXT(""));
	}

	// callback when module imported from script
	static JsErrorCode FetchImportedModuleFromScriptCallback(JsSourceContext dwReferencingSourceContext, JsValueRef specifier, JsModuleRecord* dependentModuleRecord)
	{
		return GetSelf()->FetchImportedModuleImpl(nullptr, specifier, dependentModuleRecord);
	}

	JsErrorCode FetchImportedModuleFromScript(JsSourceContext dwReferencingSourceContext, JsValueRef specifier, JsModuleRecord* dependentModuleRecord)
	{
		if (ScriptDirectories.Contains(dwReferencingSourceContext))
			return FetchImpotedModule(nullptr, specifier, dependentModuleRecord, ScriptDirectories[dwReferencingSourceContext]);

		return FetchImpotedModule(nullptr, specifier, dependentModuleRecord, TEXT(""));
	}

	static JsErrorCode NotifyModuleReadyCallback(JsModuleRecord referencingModule, JsValueRef exceptionVar)
	{
		return GetSelf()->NotifyModuleReady(referencingModule, exceptionVar);
	}

	JsErrorCode NotifyModuleReady(JsModuleRecord referencingModule, JsValueRef exceptionVar)
	{
		if (exceptionVar != JS_INVALID_REFERENCE)
		{
			JsCheck(JsSetException(exceptionVar));

			JsValueRef specifier = JS_INVALID_REFERENCE;
			JsCheck(JsGetModuleHostInfo(referencingModule, JsModuleHostInfo_HostDefined, &specifier));

			FString filename;
			if (specifier != JS_INVALID_REFERENCE)
				filename = chakra::StringFromChakra(specifier);

			JsValueRef strErr = JS_INVALID_REFERENCE;
			JsCheck(JsConvertValueToString(exceptionVar, &strErr));

			UE_LOG(Javascript, Error, TEXT("NotifyModuleReady exception! %s, %s"), *filename, *chakra::StringFromChakra(strErr));

			JsValueRef dummyException = JS_INVALID_REFERENCE;
			JsCheck(JsGetAndClearException(&dummyException)); // just consume the exception
			dummyException;
		}
		else
		{
			PendingModules.Enqueue(FJsModuleMessage(referencingModule, JS_INVALID_REFERENCE));
		}

		return JsNoError;
	}

	JsErrorCode FetchImpotedModule(JsModuleRecord referencingModule, JsValueRef specifier, JsModuleRecord* dependentModuleRecord, const FString& baseDir)
	{
		FString strSpecifier = chakra::StringFromChakra(specifier);
		FString fullPath = baseDir + strSpecifier;
		fullPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*(baseDir / strSpecifier));

		JsModuleRecord* it = Modules.Find(fullPath);
		if (it != nullptr)
		{
			*dependentModuleRecord = *it;
			return JsNoError;
		}

		JsModuleRecord newModule = JS_INVALID_REFERENCE;
		JsErrorCode err = JsInitializeModuleRecord(referencingModule, specifier, &newModule);
		if (err != JsNoError)
			return err;

		ModuleDirectories.Add(newModule, FPaths::GetPath(fullPath));
		Modules.Add(fullPath, newModule);
		InitializeModuleInfo(specifier, newModule);
		*dependentModuleRecord = newModule;
		PendingModules.Enqueue(FJsModuleMessage(newModule, specifier));

		return JsNoError;
	}

	void ExposeRequire()
	{
		FContextScope scope(context());

		JsModuleRecord RootModule = JS_INVALID_REFERENCE;
		JsCheck(JsInitializeModuleRecord(nullptr, nullptr, &RootModule));
		JsCheck(InitializeModuleInfo(nullptr, RootModule));
		return;

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
					returnValue = *it;
					return true;
				}

				FString Text;
				if (FFileHelper::LoadFileToString(Text, *script_path))
				{
					Text = FString::Printf(TEXT("(function (global, __filename, __dirname) { var module = { exports : {}, filename : __filename }, exports = module.exports; (function () { %s\n })()\n;return module.exports;}(this,'%s', '%s'));"), *Text, *script_path, *FPaths::GetPath(script_path));
					JsValueRef exports = Self->RunScript(full_path, Text, false);
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
					JsValueRef exports = Self->RunScript(full_path, Text, false);
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
					returnValue = *it;
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
					JsValueRef exports = Self->RunScript(full_path, Text, false);
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

			//auto current_script_path = FPaths::GetPath(chakra::StringFromChakra(StackTrace::CurrentStackTrace(isolate, 1, StackTrace::kScriptName)->GetFrame(0)->GetScriptName()));
			//current_script_path = URLToLocalPath(current_script_path);

			//if (!(required_module[0] == '.' && inner2(current_script_path)))
			//{
			//	for (const auto& path : load_module_paths(current_script_path))
			//	{
			//		if (inner2(path)) break;
			//		if (inner2(path / TEXT("node_modules"))) break;
			//	}

			//	for (const auto& path : Self->Paths)
			//	{
			//		if (inner2(path)) break;
			//		if (inner2(path / TEXT("node_modules"))) break;
			//	}
			//}

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
		JsCheck(JsGetGlobalObject(&global));

		chakra::SetProperty(global, "require", chakra::FunctionTemplate(fn, this));
		chakra::SetProperty(global, "purge_modules", chakra::FunctionTemplate(fn2, this));

		auto getter = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto Self = reinterpret_cast<FJavascriptContextImplementation*>(callbackState);
			FContextScope scope(Self->context());

			JsValueRef out = JS_INVALID_REFERENCE;
			JsCheck(JsCreateObject(&out));

			for (auto it = Self->Modules.CreateConstIterator(); it; ++it)
			{
				const FString& name = it.Key();
				const JsValueRef& module = it.Value();

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
		{
			TArray<JsValueRef> tasksCopy(const_cast<const TArray<JsValueRef>&>(PromiseTasks));
			PromiseTasks.Empty();

			FContextScope scope(context());
			JsValueRef global = JS_INVALID_REFERENCE, dummy = JS_INVALID_REFERENCE;
			JsCheck(JsGetGlobalObject(&global));

			for (JsValueRef task : tasksCopy)
			{
				JsCheck(JsCallFunction(task, &global, 1, &dummy));
				JsCheck(JsRelease(task, nullptr));
			}
		}

		// wait for dynamic import
		FlushModuleTasks(nullptr);

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
		//FString Text = FString::Printf(TEXT("(function (global,__filename,__dirname) { %s\n;}(this,'%s','%s'));"), *Script, *ScriptPath, *FPaths::GetPath(ScriptPath));
		//return RunScript(ScriptPath, Text, 0);
		return RunScript(ScriptPath, Script, true);
	}

	void Public_RunFile(const FString& Filename)
	{
		RunFile(Filename);
	}

	FString Public_RunScript(const FString& Script, bool bOutput = true)
	{
		FContextScope context_scope(context());

		JsValueRef ret = RunScript(TEXT("(inline)"), Script, false);
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
		// @todo: using 'ForTesting' function
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsCheck(JsGetRuntime(context(), &runtime));
		JsCheck(JsCollectGarbage(runtime));
	}

	// Should be guarded with proper handle scope
	JsValueRef RunScript(const FString& Filename, const FString& Script, bool bModule)
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

		if (bModule)
		{
			JsModuleRecord rootModule = JS_INVALID_REFERENCE;
			JsValueRef specifier = chakra::String(Filename);
			JsCheck(JsInitializeModuleRecord(nullptr, specifier, &rootModule));
			JsCheck(InitializeModuleInfo(specifier, rootModule));

			FString shim = FString::Printf(TEXT(R"raw(
import main from '%s';
let global = (new Function('return this;'))();
global.__export = main;
)raw"), *Filename);

			FTCHARToUTF8 script(*shim);
			JsValueRef exception = JS_INVALID_REFERENCE;
			JsErrorCode err = JsParseModuleSource(rootModule, 0, (BYTE*)script.Get(), script.Length(), JsParseModuleSourceFlags_DataIsUTF8, &exception);
			if (err == JsErrorScriptCompile)
			{
				UncaughtException("Invalid command");
				return JS_INVALID_REFERENCE;
			}

			if (err != JsNoError)
			{
				checkf(false, TEXT("failed to execute script! %d"), (int)err);
				return JS_INVALID_REFERENCE;
			}

			FlushModuleTasks(&returnValue);
		}
		else
		{
			JsValueRef script = chakra::String(Script);
			JsErrorCode err = JsRun(script, 0, chakra::String(LocalPathToURL(Path)), JsParseScriptAttributeNone, &returnValue);
			if (err == JsErrorScriptException)
			{
				JsValueRef exception = JS_INVALID_REFERENCE;
				JsCheck(JsGetAndClearException(&exception));

				FString strErr = chakra::StringFromChakra(chakra::GetProperty(exception, "stack"));
				UncaughtException(strErr);
				return JS_INVALID_REFERENCE;
			}

			if (err == JsErrorScriptCompile)
			{
				UncaughtException("Invalid command");
				return JS_INVALID_REFERENCE;
			}

			if (err != JsNoError)
			{
				checkf(false, TEXT("failed to execute script! %d"), (int)err);
				return JS_INVALID_REFERENCE;
			}
		}

		return returnValue;
	}

	void FlushModuleTasks(JsValueRef* returnValue)
	{
		if (returnValue != nullptr)
			*returnValue = JS_INVALID_REFERENCE;

		FJsModuleMessage module;
		while (PendingModules.Dequeue(module))
		{
			if (module.Specifier == JS_INVALID_REFERENCE)
			{
				JsValueRef result = JS_INVALID_REFERENCE;
				JsErrorCode err = JsModuleEvaluation(module.ReferencingModule, &result);
				if (err != JsNoError)
				{
					if (err == JsErrorScriptException)
					{
						JsValueRef exception = JS_INVALID_REFERENCE;
						JsCheck(JsGetAndClearException(&exception));

						JsValueRef strException = JS_INVALID_REFERENCE;
						JsCheck(JsConvertValueToString(chakra::GetProperty(exception, "stack"), &strException));

						UncaughtException(chakra::StringFromChakra(strException));
					}
					else
					{
						UncaughtException(FString::Printf(TEXT("Module parse failed: %d"), int(err)));
					}
				}

				JsValueRef global = JS_INVALID_REFERENCE;
				JsCheck(JsGetGlobalObject(&global));

				JsValueRef mainFunction = chakra::GetProperty(global, "__export");
				JsValueRef deleteResult;
				JsCheck(JsDeleteProperty(global, chakra::PropertyID("__export"), true, &deleteResult));

				if (chakra::IsFunction(mainFunction) && returnValue != nullptr)
				{
					JsErrorCode execErr = JsCallFunction(mainFunction, &global, 1, returnValue);

					if (execErr == JsErrorScriptException)
					{
						JsValueRef exception = JS_INVALID_REFERENCE;
						JsCheck(JsGetAndClearException(&exception));

						FString strErr = chakra::StringFromChakra(chakra::GetProperty(exception, "stack"));
						UncaughtException(strErr);
						return;
					}

					if (execErr != JsNoError)
					{
						checkf(false, TEXT("failed to execute script! %d"), (int)execErr);
						return;
					}
				}
			}
			else
			{
				check(module.ReferencingModule == JS_INVALID_REFERENCE || ModuleDirectories.Contains(module.ReferencingModule));
				FString Filename = chakra::StringFromChakra(module.Specifier);
				if (module.ReferencingModule != JS_INVALID_REFERENCE)
					Filename = FPaths::ConvertRelativePathToFull(ModuleDirectories[module.ReferencingModule], Filename);

				// check if directory module
				FString physicalPath = Filename;
				if (IFileManager::Get().DirectoryExists(*physicalPath))
					physicalPath = physicalPath / TEXT("index.js");

				if (!physicalPath.EndsWith(".js"))
					physicalPath.Append(".js");

				FString Script = ReadScriptFile(physicalPath);
				FString ScriptPath = GetScriptFileFullPath(Filename);
				JsModuleRecord record = JS_INVALID_REFERENCE;
				if (Modules.Contains(ScriptPath))
					record = Modules[ScriptPath];

				check(record != JS_INVALID_REFERENCE);
				FTCHARToUTF8 script(*Script);
				JsValueRef exception = JS_INVALID_REFERENCE;
				JsParseModuleSource(record, ModuleSourceContext++, (BYTE*)script.Get(), script.Length(), JsParseModuleSourceFlags_DataIsUTF8, &exception);
			}
		}
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
		JsValueRef Packer = RunScript(TEXT(""), TEXT("JSON.stringify"), false);

		auto guard_pre = [&] { w.push("try { "); };
		auto guard_post = [&] { w.push(" } catch (e) {};\n"); };

		// export
		w.push("export default () => {\n");

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

		w.push("};");

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

	JsValueRef InternalReadProperty(UProperty* Property, uint8* Buffer, const IPropertyOwner& Owner)
	{
		if (!Buffer)
		{
			chakra::Throw(TEXT("Read property from invalid memory"));
			return chakra::Undefined();
		}

		if (auto p = Cast<UIntProperty>(Property))
		{
			return chakra::Int(p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UFloatProperty>(Property))
		{
			return chakra::Double(p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
            return chakra::Boolean(p->GetPropertyValue_InContainer(Buffer));
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{
			FName name = p->GetPropertyValue_InContainer(Buffer);
			return chakra::String(name.ToString());
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			const FString& Data = p->GetPropertyValue_InContainer(Buffer);
			return chakra::String(Data);
		}
		else if (auto p = Cast<UTextProperty>(Property))
		{
			const FText& Data = p->GetPropertyValue_InContainer(Buffer);
			return chakra::String(Data.ToString());
		}		
		else if (auto p = Cast<UClassProperty>(Property))
		{			
			UClass* Class = Cast<UClass>(p->GetPropertyValue_InContainer(Buffer));

			if (Class)
			{
				return ExportClass(Class);
			}
			else
			{
				return chakra::Null();
			}
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			if (auto ScriptStruct = Cast<UScriptStruct>(p->Struct))
			{	
				return ExportStructInstance(ScriptStruct, p->ContainerPtrToValuePtr<uint8>(Buffer), Owner);
			}			
			else
			{
				UE_LOG(Javascript, Warning, TEXT("Non ScriptStruct found : %s"), *p->Struct->GetName());
				
				return chakra::Undefined();
			}			
		}		
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			FScriptArrayHelper_InContainer helper(p, Buffer);
			auto len = (uint32_t)(helper.Num());

			JsValueRef arr = JS_INVALID_REFERENCE;
			JsCheck(JsCreateArray(len, &arr));

			auto Inner = p->Inner;

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
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			return ExportObject(p->GetObjectPropertyValue_InContainer(Buffer));
		}				
		else if (auto p = Cast<UByteProperty>(Property))
		{
			auto Value = p->GetPropertyValue_InContainer(Buffer);

			if (p->Enum)
			{							
				return chakra::String(p->Enum->GetNameStringByIndex(Value));
			}			
			else
			{
				return chakra::Int(Value);
			}
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			int32 Value = p->GetUnderlyingProperty()->GetSignedIntPropertyValue(p->ContainerPtrToValuePtr<uint8>(Buffer));
			return chakra::String(p->GetEnum()->GetNameStringByIndex(Value));
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			FScriptSetHelper_InContainer SetHelper(p, Buffer);

			int Num = SetHelper.Num();
			JsValueRef Out = JS_INVALID_REFERENCE;
			JsCheck(JsCreateArray(Num, &Out));

			for (int Index = 0; Index < Num; ++Index)
			{
				auto PairPtr = SetHelper.GetElementPtr(Index);

				chakra::SetIndex(Out, Index, InternalReadProperty(p->ElementProp, SetHelper.GetElementPtr(Index), Owner));
			}

			return Out;
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			FScriptMapHelper_InContainer MapHelper(p, Buffer);

			JsValueRef Out = JS_INVALID_REFERENCE;
			JsCheck(JsCreateObject(&Out));

			int Num = MapHelper.Num();
			for (int Index = 0; Index < Num; ++Index)
			{
				uint8* PairPtr = MapHelper.GetPairPtr(Index);

				JsValueRef Key = InternalReadProperty(p->KeyProp, PairPtr + p->MapLayout.KeyOffset, Owner);
				JsValueRef Value = InternalReadProperty(p->ValueProp, PairPtr, Owner);

				chakra::SetProperty(Out, chakra::StringFromChakra(Key), Value);
			}

			return Out;
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

	void InternalWriteProperty(UProperty* Property, uint8* Buffer, JsValueRef Value)
	{
		if (!Buffer)
		{
			chakra::Throw(TEXT("Write property on invalid memory"));
			return;
		}

		if (chakra::IsEmpty(Value) || chakra::IsUndefined(Value)) return;

		if (auto p = Cast<UIntProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, chakra::IntFrom(Value));
		}
		else if (auto p = Cast<UFloatProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, chakra::DoubleFrom(Value));
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, chakra::BoolFrom(Value));
		}
		else if (auto p = Cast<UNameProperty>(Property))
		{			
			p->SetPropertyValue_InContainer(Buffer, FName(*chakra::StringFromChakra(Value)));
		}
		else if (auto p = Cast<UStrProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, chakra::StringFromChakra(Value));
		}		
		else if (auto p = Cast<UTextProperty>(Property))
		{
			p->SetPropertyValue_InContainer(Buffer, FText::FromString(chakra::StringFromChakra(Value)));
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			if (chakra::IsString(Value))
			{
				FString UString = chakra::StringFromChakra(Value);
				if (UString == TEXT("null"))
				{
					p->SetPropertyValue_InContainer(Buffer, nullptr);
				}
				else
				{
					auto Object = StaticLoadObject(UObject::StaticClass(), nullptr, *UString);					
					if (auto Class = Cast<UClass>(Object))
					{
						p->SetPropertyValue_InContainer(Buffer, Class);
					}
					else if (auto BP = Cast<UBlueprint>(Object))
					{
						auto BPGC = BP->GeneratedClass;
						p->SetPropertyValue_InContainer(Buffer, BPGC);
					}
					else
					{
						p->SetPropertyValue_InContainer(Buffer, Object);
					}
				}
			}
			else
			{
				p->SetPropertyValue_InContainer(Buffer, chakra::UClassFromChakra(Value));
			}
		}
		else if (auto p = Cast<UStructProperty>(Property))
		{
			if (auto ScriptStruct = Cast<UScriptStruct>(p->Struct))
			{
				auto Instance = FStructMemoryInstance::FromChakra(Value);

				// If given value is an instance
				if (Instance)
				{
					auto GivenStruct = Instance->Struct;

					// Type-checking needed
					if (GivenStruct->IsChildOf(ScriptStruct))
					{
						p->CopyCompleteValue(p->ContainerPtrToValuePtr<void>(Buffer), Instance->GetMemory());
					}
					else
					{				
						chakra::Throw(FString::Printf(TEXT("Wrong struct type (given:%s), (expected:%s)"), *GivenStruct->GetName(), *p->Struct->GetName()));
					}
				}
				else if (p->Struct->IsChildOf(FJavascriptFunction::StaticStruct()))
				{
					auto struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);
					FJavascriptFunction func;
					if (chakra::IsFunction(Value))
					{
						JsValueRef jsfunc = Value;
						func.Handle = MakeShareable(new FPrivateJavascriptFunction);
						func.Handle->context.Reset(context());
						func.Handle->Function.Reset(jsfunc);
					}
					p->Struct->CopyScriptStruct(struct_buffer, &func);
				}
				else if (p->Struct->IsChildOf(FJavascriptRef::StaticStruct()))
				{
					auto struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);
					FJavascriptRef ref;

					if (chakra::IsObject(Value))
					{
						JsValueRef jsobj = Value;
						ref.Handle = MakeShareable(new FPrivateJavascriptRef);
						ref.Handle->Object.Reset(jsobj);
					}
					
					p->Struct->CopyScriptStruct(struct_buffer, &ref);
				}
				// If raw javascript object has been passed,
				else if (chakra::IsObject(Value))
				{
					JsValueRef v8_obj = Value;
					uint8* struct_buffer = p->ContainerPtrToValuePtr<uint8>(Buffer);

					UScriptStruct* Struct = p->Struct;

					ReadOffStruct(v8_obj, Struct, struct_buffer);
				}
				else
				{
					if (nullptr == p->Struct->GetOwnerClass())
						chakra::Throw(FString::Printf(TEXT("Needed struct data : [null] %s"), *p->Struct->GetName()));
					else
						chakra::Throw(FString::Printf(TEXT("Needed struct data : %s %s"), *p->Struct->GetOwnerClass()->GetName(), *p->Struct->GetName()));
				}
			}
			else
			{
				chakra::Throw(FString::Printf(TEXT("No ScriptStruct found : %s"), *p->Struct->GetName()));
			}					
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			if (chakra::IsArray(Value))
			{
				JsValueRef arr = Value;
				int len = chakra::Length(arr);

				FScriptArrayHelper_InContainer helper(p, Buffer);

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
					WriteProperty(p->Inner, helper.GetRawPtr(Index), chakra::GetIndex(arr, Index));
				}
			}
			else
			{
				chakra::Throw(TEXT("Should write into array by passing an array instance"));
			}
		}
		else if (auto p = Cast<UByteProperty>(Property))
		{
			if (p->Enum)
			{
				FString Str = chakra::StringFromChakra(Value);
				int EnumValue = p->Enum->GetIndexByName(FName(*Str), EGetByNameFlags::None);
				if (EnumValue == INDEX_NONE)
				{
					chakra::Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *p->Enum->GetName()));
				}
				else
				{
					p->SetPropertyValue_InContainer(Buffer, EnumValue);
				}
			}			
			else
			{				
				p->SetPropertyValue_InContainer(Buffer, chakra::IntFrom(Value));
			}
		}
		else if (auto p = Cast<UEnumProperty>(Property))
		{
			FString Str = chakra::StringFromChakra(Value);
			auto EnumValue = p->GetEnum()->GetIndexByName(FName(*Str), EGetByNameFlags::None);
			if (EnumValue == INDEX_NONE)
			{
				chakra::Throw(FString::Printf(TEXT("Enum Text %s for Enum %s failed to resolve to any value"), *Str, *p->GetName()));
			}
			else
			{
				uint8* PropData = p->ContainerPtrToValuePtr<uint8>(Buffer);
				p->GetUnderlyingProperty()->SetIntPropertyValue(PropData, (int64)EnumValue);
			}
		}
		else if (auto p = Cast<UObjectPropertyBase>(Property))
		{
			p->SetObjectPropertyValue_InContainer(Buffer, chakra::UObjectFromChakra(Value));
		}
		else if (auto p = Cast<USetProperty>(Property))
		{
			if (chakra::IsArray(Value))
			{
				JsValueRef arr = Value;
				int len = chakra::Length(arr);

				FScriptSetHelper_InContainer SetHelper(p, Buffer);

				auto Num = SetHelper.Num();
				for (int Index = 0; Index < Num; ++Index)
				{
					const int32 ElementIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
					uint8* ElementPtr = SetHelper.GetElementPtr(Index);
					InternalWriteProperty(p->ElementProp, ElementPtr, chakra::GetIndex(arr, Index));
				}

				SetHelper.Rehash();
			}
		}
		else if (auto p = Cast<UMapProperty>(Property))
		{
			if (chakra::IsObject(Value))
			{
				FScriptMapHelper_InContainer MapHelper(p, Buffer);

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
					InternalWriteProperty(p->KeyProp, PairPtr + p->MapLayout.KeyOffset, Key);
					InternalWriteProperty(p->ValueProp, PairPtr, Value);
				}
			}
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
				if (ArgIndex < argumentCount-1)
				{
					return arguments[ArgIndex+1];
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
			JsValueRef self = chakra::GetPrototype(arguments[0]);

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
		chakra::SetProperty(Template, function_name, function);
	}

	void ExportBlueprintLibraryFactoryFunction(JsValueRef Template, UFunction* FunctionToExport)
	{
		// Exposed function body (it doesn't capture anything)
		auto FunctionBody = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState)
		{
			JsValueRef self = chakra::GetPrototype(arguments[0]);

			// Retrieve "FUNCTION"
			UFunction* Function = reinterpret_cast<UFunction*>(callbackState);

			// 'this' should be CDO of owner class
			UObject* Object = Function->GetOwnerClass()->ClassDefaultObject;

			// Call unreal engine function!
			return CallFunction(self, Function, Object, [&](int ArgIndex) {
				// pass an argument if we have
				if (ArgIndex < argumentCount-1)
				{
					return arguments[ArgIndex+1];
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

		chakra::SetProperty(Template, "get", chakra::FunctionTemplate(fn));
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

		chakra::SetProperty(Template, "clone", chakra::FunctionTemplate(fn));
	}

	template <typename PropertyAccessor>
	void AddMemberFunction_Struct_toJSON(JsValueRef Template, UStruct* ClassToExport)
	{
		auto fn = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			UClass* Class = reinterpret_cast<UClass*>(callbackState);

			JsValueRef self = arguments[0];
			JsValueRef out = JS_INVALID_REFERENCE;
			JsCreateObject(&out);

			auto Object_toJSON = [&](JsValueRef value)
			{
				UObject* Object = chakra::UObjectFromChakra(value);
				if (Object == nullptr)
				{
					return chakra::Null();
				}
				else
				{
					return chakra::String(Object->GetPathName());
				}
			};

			for (TFieldIterator<UProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
			{
				UProperty* Property = *PropertyIt;

				if (FV8Config::CanExportProperty(Class, Property))
				{
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
					else if (auto p = Cast<UObjectPropertyBase>(Property))
					{
						chakra::SetProperty(out, name, Object_toJSON(value));
					}
					else if (auto p = Cast<UArrayProperty>(Property))
					{
						if (auto q = Cast<UObjectPropertyBase>(p->Inner))
						{
							JsValueRef arr = value;
							int len = chakra::Length(arr);
							
							JsValueRef out_arr = JS_INVALID_REFERENCE;
							JsCheck(JsCreateArray(len, &out_arr));
							chakra::SetProperty(out, name, out_arr);

							for (decltype(len) Index = 0; Index < len; ++Index)
							{
								JsValueRef v = chakra::GetIndex(arr, Index);
								chakra::SetIndex(out_arr, Index, Object_toJSON(v));
							}
						}
						else
						{
							chakra::SetProperty(out, name, value);
						}
					}
					else
					{
						chakra::SetProperty(out, name, value);
					}
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

					if (FV8Config::CanExportProperty(Class, Property) && MatchPropertyName(Property,PropertyNameToAccess))
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
		FStructMemoryInstance* memory = reinterpret_cast<FStructMemoryInstance*>(data);
		GetSelf()->OnGarbageCollectedByChakra(memory);
	}

	static void FinalizeUObject(JsRef ref, void* data)
	{
		UObject* memory = reinterpret_cast<UObject*>(data);
		GetSelf()->OnGarbageCollectedByChakra(memory);
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

		return chakra::New(v8_struct, args, 3);
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

		auto ObjectPtr = ObjectToObjectMap.Find(Object);
		if (ObjectPtr == nullptr)
		{
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

			JsValueRef value = JS_INVALID_REFERENCE;
			
			if (UClass* Class = Cast<UClass>(Object))
			{
				value = ExportClass(Class);
			}
			else if (UScriptStruct* Struct = Cast<UScriptStruct>(Object))
			{
				value = ExportStruct(Struct);
			}
			else
			{
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

				value = chakra::New(v8_class, args, 2);
			}

			return value;
		}
		else
		{
			return ObjectPtr->Get();
		}
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
	FContextScope scope(context());
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
			JsCheck(JsCallFunction(function, &function, 1, &returnValue));
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
