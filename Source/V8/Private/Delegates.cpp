
#include "Delegates.h"
#include "V8PCH.h"
#include "JavascriptDelegate.h"
#include "Translator.h"
#include "JavascriptStats.h"
#include "UObject/GCObject.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

class FJavascriptDelegate : public FGCObject, public TSharedFromThis<FJavascriptDelegate>
{
public:
	FWeakObjectPtr WeakObject;
	UProperty* Property;
	Persistent<JsValueRef> context_;
	TMap<int32, JsFunctionRef> functions;
	Persistent<JsValueRef> WrappedObject;
	int32 NextUniqueId{ 0 };
	bool bAbandoned{ false };

	bool IsValid() const
	{
		return WeakObject.IsValid();
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(DelegateObjects);
	}

	FJavascriptDelegate(UObject* InObject, UProperty* InProperty)
		: WeakObject(InObject), Property(InProperty)
	{}

	~FJavascriptDelegate()
	{
		Purge();
	}

	void Purge()
	{
		if (!bAbandoned)
		{
			bAbandoned = true;

			WrappedObject.Reset();

			ClearDelegateObjects();

			context_.Reset();
		}
	}

	JsValueRef Initialize(JsContextRef context)
	{
		JsValueRef out = JS_INVALID_REFERENCE;
		JsCreateObject(&out);

		JsNativeFunction add = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			FJavascriptDelegate* payload = nullptr;
			JsGetExternalData(callbackState, (void**)&payload);

			for (;;)
			{
				if (argumentCount == 2)
				{
					JsFunctionRef func = arguments[1];
					JsValueType Type = JsValueType::JsUndefined;
					if (JsGetValueType(func, &Type) == JsNoError && Type == JsValueType::JsFunction)
					{
						payload->Add(func);
						break;
					}
				}

				UE_LOG(Javascript, Log, TEXT("Invalid argument for delegate"));
				break;
			}

			JsValueRef undefined = JS_INVALID_REFERENCE;
			JsGetUndefinedValue(&undefined);
			return undefined;
		};

		auto remove = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			FJavascriptDelegate* payload = nullptr;
			JsGetExternalData(callbackState, (void**)&payload);
			for (;;)
			{
				if (argumentCount == 2)
				{
					JsFunctionRef func = arguments[1];
					JsValueType Type = JsValueType::JsUndefined;
					if (JsGetValueType(func, &Type) == JsNoError && Type == JsValueType::JsFunction)
					{
						payload->Remove(func);
						break;
					}
				}

				UE_LOG(Javascript, Log, TEXT("Invalid argument for delegate"));
				break;
			}

			JsValueRef undefined = JS_INVALID_REFERENCE;
			JsGetUndefinedValue(&undefined);
			return undefined;
		};

		auto clear = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			FJavascriptDelegate* payload = nullptr;
			JsGetExternalData(callbackState, (void**)&payload);
			payload->Clear();			

			JsValueRef undefined = JS_INVALID_REFERENCE;
			JsGetUndefinedValue(&undefined);
			return undefined;
		};

		auto toJSON = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			FJavascriptDelegate* payload = nullptr;
			JsGetExternalData(callbackState, (void**)&payload);
			const bool bIsMulticastDelegate = payload->Property->IsA(UMulticastDelegateProperty::StaticClass());

			uint32_t Index = 0;
			JsValueRef arr = JS_INVALID_REFERENCE;
			JsCreateArray(payload->DelegateObjects.Num(), &arr);

			for (auto DelegateObject : payload->DelegateObjects)
			{
				auto JavascriptFunction = payload->functions.Find(DelegateObject->UniqueId);
				if (JavascriptFunction)
				{
					if (!bIsMulticastDelegate)
						return *JavascriptFunction;
					
					//arr->Set(Index++, function);
					JsValueRef index = JS_INVALID_REFERENCE;
					JsIntToNumber(Index++, &index);
					JsSetIndexedProperty(arr, index, JavascriptFunction);
				}
			}

			if (!bIsMulticastDelegate)
			{
				JsValueRef nullValue = JS_INVALID_REFERENCE;
				JsGetNullValue(&nullValue);
				return nullValue;
			}

			return arr;
		};

		JsPropertyIdRef addProp = JS_INVALID_REFERENCE;
		JsFunctionRef addFunc = JS_INVALID_REFERENCE;
		JsCreateFunction(add, this, &addFunc);
		JsCreatePropertyId("Add", FCStringAnsi::Strlen("Add"), &addProp);
		JsSetProperty(out, addProp, addFunc, true);

		JsPropertyIdRef removeProp = JS_INVALID_REFERENCE;
		JsFunctionRef removeFunc = JS_INVALID_REFERENCE;
		JsCreateFunction(remove, this, &removeFunc);
		JsCreatePropertyId("Remove", FCStringAnsi::Strlen("Remove"), &removeProp);
		JsSetProperty(out, removeProp, removeFunc, true);

		JsPropertyIdRef clearProp = JS_INVALID_REFERENCE;
		JsFunctionRef clearFunc = JS_INVALID_REFERENCE;
		JsCreateFunction(clear, this, &clearFunc);
		JsCreatePropertyId("Clear", FCStringAnsi::Strlen("Clear"), &clearProp);
		JsSetProperty(out, clearProp, clearFunc, true);

		JsPropertyIdRef toJSONProp = JS_INVALID_REFERENCE;
		JsFunctionRef toJSONFunc = JS_INVALID_REFERENCE;
		JsCreateFunction(toJSON, this, &toJSONFunc);
		JsCreatePropertyId("toJSON", FCStringAnsi::Strlen("toJSON"), &toJSONProp);
		JsSetProperty(out, toJSONProp, toJSONFunc, true);

		WrappedObject.Reset(out);
		return out;
	}

	TArray<UJavascriptDelegate*> DelegateObjects;

	void ClearDelegateObjects()
	{
		for (auto obj : DelegateObjects)
		{
			obj->RemoveFromRoot();
		}
		DelegateObjects.Empty();
		functions.Empty();
	}

	void Add(JsFunctionRef function)
	{
		auto DelegateObject = NewObject<UJavascriptDelegate>();

		DelegateObject->UniqueId = NextUniqueId++;

		Bind(DelegateObject, function);
	}

	UJavascriptDelegate* FindJavascriptDelegateByFunction(JsFunctionRef function)
	{
		//HandleScope handle_scope(isolate_);

		bool bWasSuccessful = false;
		for (auto it = functions.CreateIterator(); it; ++it)
		{
			if (it.Value() == function)
			{
				for (auto obj : DelegateObjects)
				{
					if (obj->UniqueId == it.Key())
					{
						return obj;
					}
				}
			}
		}

		return nullptr;
	}

	void Remove(JsFunctionRef function)
	{
		auto obj = FindJavascriptDelegateByFunction(function);

		if (obj)
		{
			Unbind(obj);
		}
		else
		{
			UE_LOG(Javascript, Log, TEXT("No match for removing delegate"));
		}
	}

	void Clear()
	{
		while (DelegateObjects.Num())
		{
			Unbind(DelegateObjects[0]);
		}
	}

	void Bind(UJavascriptDelegate* DelegateObject, JsFunctionRef function)
	{
		static FName NAME_Fire("Fire");

		if (WeakObject.IsValid())
		{
			if (auto p = Cast<UMulticastDelegateProperty>(Property))
			{
				FScriptDelegate Delegate;
				Delegate.BindUFunction(DelegateObject, NAME_Fire);

				auto Target = p->GetPropertyValuePtr_InContainer(WeakObject.Get());
				Target->Add(Delegate);
			}
			else if (auto p = Cast<UDelegateProperty>(Property))
			{
				auto Target = p->GetPropertyValuePtr_InContainer(WeakObject.Get());
				Target->BindUFunction(DelegateObject, NAME_Fire);
			}
		}

		DelegateObject->JavascriptDelegate = AsShared();
		DelegateObjects.Add(DelegateObject);

		functions.Add( DelegateObject->UniqueId, function );
	}

	void Unbind(UJavascriptDelegate* DelegateObject)
	{
		static FName NAME_Fire("Fire");

		if (WeakObject.IsValid())
		{
			if (auto p = Cast<UMulticastDelegateProperty>(Property))
			{
				FScriptDelegate Delegate;
				Delegate.BindUFunction(DelegateObject, NAME_Fire);

				auto Target = p->GetPropertyValuePtr_InContainer(WeakObject.Get());
				Target->Remove(Delegate);
			}
			else if (auto p = Cast<UDelegateProperty>(Property))
			{
				auto Target = p->GetPropertyValuePtr_InContainer(WeakObject.Get());
				Target->Clear();
			}
		}

		DelegateObject->JavascriptDelegate.Reset();
		DelegateObjects.Remove(DelegateObject);

		if (!bAbandoned)
		{
			functions.Remove(DelegateObject->UniqueId);
		}
	}

	UFunction* GetSignatureFunction()
	{
		if (auto p = Cast<UMulticastDelegateProperty>(Property))
		{
			return p->SignatureFunction;
		}
		else if (auto p = Cast<UDelegateProperty>(Property))
		{
			return p->SignatureFunction;
		}
		else
		{
			return nullptr;
		}
	}

	void Fire(void* Parms, UJavascriptDelegate* Delegate)
	{
		SCOPE_CYCLE_COUNTER(STAT_JavascriptDelegate);

		if (!Delegate->IsValidLowLevelFast())
		{
			return;
		}

		auto Buffer = reinterpret_cast<uint8*>(Parms);		

		auto it = functions.Find(Delegate->UniqueId);
		if (WeakObject.IsValid() && it)
		{
			//Isolate::Scope isolate_scope(isolate_);
			//HandleScope handle_scope(isolate_);

			JsFunctionRef func = *it;
			if (func != JS_INVALID_REFERENCE)
			{
				FContextScope Scope(context_.Get());
				JsValueRef Global = JS_INVALID_REFERENCE;
				JsGetGlobalObject(&Global);

				chakra::CallJavascriptFunction(context_.Get(), Global, GetSignatureFunction(), func, Parms);
			}
		}
	}
};

struct FDelegateManager : chakra::IDelegateManager
{
	FDelegateManager()
	{}

	virtual ~FDelegateManager()
	{
		PurgeAllDelegates();
	}

	virtual void Destroy() override
	{
		delete this;
	}

	TSet<TSharedPtr<FJavascriptDelegate>> Delegates;

	void CollectGarbageDelegates()
	{
		for (auto it = Delegates.CreateIterator(); it; ++it)
		{
			auto d = *it;
			if (!d->IsValid())
			{
				it.RemoveCurrent();
			}
		}
	}

	void PurgeAllDelegates()
	{
		Delegates.Empty();
	}

	JsValueRef CreateDelegate(JsContextRef Context, UObject* Object, UProperty* Property)
	{
		//@HACK
		CollectGarbageDelegates();

		TSharedPtr<FJavascriptDelegate> payload = MakeShareable(new FJavascriptDelegate(Object, Property));
		auto created = payload->Initialize(Context);

		Delegates.Add(payload);

		return created;
	}

	virtual JsValueRef GetProxy(JsValueRef This, UObject* Object, UProperty* Property) override
	{
		JsValueRef cached = JS_INVALID_REFERENCE;
		JsPropertyIdRef cachedProp = JS_INVALID_REFERENCE;
		FTCHARToUTF8 cachedId (*FString::Printf(TEXT("$internal_%s"), *(Property->GetName())));
		JsCreatePropertyId(cachedId.Get(), cachedId.Length(), &cachedProp);
		JsGetProperty(This, cachedProp, &cached);

		JsValueType Type = JsValueType::JsUndefined;
		if (cached == JS_INVALID_REFERENCE || JsGetValueType(This, &Type) != JsNoError || Type == JsUndefined)
		{
			JsContextRef Context = JS_INVALID_REFERENCE;
			JsErrorCode err = JsGetContextOfObject(This, &Context);
			check(err == JsNoError);

			auto created = CreateDelegate(Context, Object, Property);
			JsSetProperty(This, cachedProp, created, true);
			return created;
		}

		return cached;
	}
};

namespace chakra
{
	IDelegateManager* IDelegateManager::Create()
	{
		return new FDelegateManager();
	}
}

void UJavascriptDelegate::Fire()
{}

void UJavascriptDelegate::ProcessEvent(UFunction* Function, void* Parms)
{
	if (JavascriptDelegate.IsValid())
	{
		JavascriptDelegate.Pin()->Fire(Parms, this);
	}
}

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS