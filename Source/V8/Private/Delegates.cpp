#include "Delegates.h"
#include "V8PCH.h"
#include "JavascriptDelegate.h"
#include "Translator.h"
#include "Helpers.h"
#include "JavascriptStats.h"
#include "UObject/GCObject.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

class FJavascriptDelegate : public FGCObject, public TSharedFromThis<FJavascriptDelegate>
{
public:
	FWeakObjectPtr WeakObject;
	UProperty* Property;
	Persistent<JsContextRef> context_;
	TMap<int32, Persistent<JsValueRef>> functions;
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
		context_.Reset(context);

		JsValueRef out = JS_INVALID_REFERENCE;
		JsCheck(JsCreateObject(&out));

		auto add = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			for (;;)
			{
				auto payload = reinterpret_cast<FJavascriptDelegate*>(callbackState);
				if (argumentCount == 2)
				{
					JsValueRef func = arguments[1];
					if (!chakra::IsEmpty(func))
					{
						payload->Add(func);
						break;
					}
				}

				UE_LOG(Javascript, Log, TEXT("Invalid argument for delegate"));
				break;
			}

			return chakra::Undefined();
		};

		auto remove = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			for (;;)
			{
				auto payload = reinterpret_cast<FJavascriptDelegate*>(callbackState);
				if (argumentCount == 2)
				{
					JsValueRef func = arguments[1];
					if (!chakra::IsEmpty(func))
					{
						payload->Remove(func);
						break;
					}
				}

				UE_LOG(Javascript, Log, TEXT("Invalid argument for delegate"));
				break;
			}

			return chakra::Undefined();
		};

		auto clear = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto payload = reinterpret_cast<FJavascriptDelegate*>(callbackState);
			payload->Clear();
			return chakra::Undefined();
		};

		auto toJSON = [](JsValueRef callee, bool isConstructCall, JsValueRef *arguments, unsigned short argumentCount, void *callbackState) {
			auto payload = reinterpret_cast<FJavascriptDelegate*>(callbackState);

			uint32_t Index = 0;
			JsValueRef arr = JS_INVALID_REFERENCE;
			JsCheck(JsCreateArray(payload->DelegateObjects.Num(), &arr));
			const bool bIsMulticastDelegate = payload->Property->IsA(UMulticastDelegateProperty::StaticClass());

			for (auto DelegateObject : payload->DelegateObjects)
			{
				auto JavascriptFunction = payload->functions.Find(DelegateObject->UniqueId);
				if (JavascriptFunction)
				{
					JsValueRef function = JavascriptFunction->Get();
					if (!bIsMulticastDelegate)
					{
						return function;
					}

					chakra::SetIndex(arr, Index++, function);
				}
			}

			if (!bIsMulticastDelegate)
			{
				return chakra::Null();
			}
			else
			{
				return arr;
			}
		};

		JsValueRef data = chakra::External(this, nullptr);

		chakra::SetProperty(out, "Add", chakra::FunctionTemplate(add, this));
		chakra::SetProperty(out, "Remove", chakra::FunctionTemplate(remove, this));
		chakra::SetProperty(out, "Clear", chakra::FunctionTemplate(clear, this));
		chakra::SetProperty(out, "toJSON", chakra::FunctionTemplate(toJSON, this));

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

	void Add(JsValueRef function)
	{
		auto DelegateObject = NewObject<UJavascriptDelegate>();

		DelegateObject->UniqueId = NextUniqueId++;

		Bind(DelegateObject, function);
	}

	UJavascriptDelegate* FindJavascriptDelegateByFunction(JsValueRef function)
	{
		bool bWasSuccessful = false;
		for (auto it = functions.CreateIterator(); it; ++it)
		{
			if (it.Value().Get() == function)
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

	void Remove(JsValueRef function)
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

	void Bind(UJavascriptDelegate* DelegateObject, JsValueRef function)
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
			JsValueRef func = it->Get();
			if (!chakra::IsEmpty(func))
			{
				FContextScope Scope(context_.Get());
				JsValueRef Global = JS_INVALID_REFERENCE;
				JsCheck(JsGetGlobalObject(&Global));

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

	JsValueRef CreateDelegate(UObject* Object, UProperty* Property)
	{
		//@HACK
		CollectGarbageDelegates();

		TSharedPtr<FJavascriptDelegate> payload = MakeShareable(new FJavascriptDelegate(Object, Property));
		JsContextRef context = JS_INVALID_REFERENCE;
		JsCheck(JsGetCurrentContext(&context));
		JsValueRef created = payload->Initialize(context);

		Delegates.Add(payload);

		return created;
	}

	virtual JsValueRef GetProxy(JsValueRef This, UObject* Object, UProperty* Property) override
	{
		FString cache_id = FString::Printf(TEXT("$internal_%s"), *(Property->GetName()));
		JsValueRef cached = chakra::GetProperty(This, cache_id);
		if (chakra::IsEmpty(cached) || chakra::IsUndefined(cached))
		{
			JsValueRef created = CreateDelegate(Object, Property);

			chakra::SetProperty(This, cache_id, created);
			return created;
		}
		else
		{
			return cached;
		}
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