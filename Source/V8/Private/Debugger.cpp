#include "V8PCH.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#if !PLATFORM_LINUX
#include "ChakraDebugService.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptContext_Private.h"
#include "SocketSubSystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "NetworkMessage.h"
#include "Misc/DefaultValueHelper.h"

#define DEBUG_V8_DEBUGGER 1

#include "ScopeLock.h"
#include "Translator.h"
#include "Helpers.h"
#include "ChakraDebug.h"
#endif

#if WITH_EDITOR && !PLATFORM_LINUX
#include "TickableEditorObject.h"

namespace 
{
	//@HACK : to be multi-threaded and support multiple sockets
	FSocket* MainSocket{ nullptr };

	void RegisterDebuggerSocket(FSocket* Socket)
	{
		MainSocket = Socket;
	}

	void UnregisterDebuggerSocket(FSocket* Socket)
	{
		MainSocket = nullptr;
	}

	void SendTo(FSocket* Socket_, const char* msg)
	{
		FSimpleAbstractSocket_FSocket Socket(Socket_);

		auto send = [&](const char* msg) {
			Socket.Send(reinterpret_cast<const uint8*>(msg), strlen(msg));
		};

		send(TCHAR_TO_UTF8(*FString::Printf(TEXT("Content-Length: %d\r\n"), strlen(msg))));
		send("\r\n");
		send(msg);

#if DEBUG_V8_DEBUGGER
		UE_LOG(Javascript, Log, TEXT("Reply : %s"), UTF8_TO_TCHAR(msg));
#endif
	}

	void Broadcast(const char* msg)
	{
		if (MainSocket)
		{
			SendTo(MainSocket,msg);
		}
#if DEBUG_V8_DEBUGGER
		UE_LOG(Javascript, Log, TEXT("Broadcast : %s"), UTF8_TO_TCHAR(msg));
#endif
	}
}

class FJsDebugProtocolHandler
{
private:
	JsDebugProtocolHandler ProtocolHandler{ nullptr };

public:
	FJsDebugProtocolHandler(JsRuntimeHandle runtime)
	{
		JsDebugProtocolHandler protocolHandler;

		JsErrorCode result = JsDebugProtocolHandlerCreate(runtime, &protocolHandler);

		if (ensure(result == JsNoError))
		{
			ProtocolHandler = protocolHandler;
		}
	}

	~FJsDebugProtocolHandler()
	{
		Destroy();
	}

	JsErrorCode Connect(bool breakOnNextLine, JsDebugProtocolHandlerSendResponseCallback callback, void* callbackState)
	{
		JsErrorCode result = JsDebugProtocolHandlerConnect(ProtocolHandler, breakOnNextLine, callback, callbackState);

		return result;
	}

	JsErrorCode Destroy()
	{
		JsErrorCode result = JsNoError;

		if (ProtocolHandler != nullptr)
		{
			result = JsDebugProtocolHandlerDestroy(ProtocolHandler);

			if (result == JsNoError)
			{
				ProtocolHandler = nullptr;
			}
		}

		return result;
	}

	JsErrorCode Disconnect()
	{
		JsErrorCode result = JsDebugProtocolHandlerDisconnect(ProtocolHandler);

		return result;
	}

	JsDebugProtocolHandler GetHandle()
	{
		return ProtocolHandler;
	}

	JsErrorCode WaitForDebugger()
	{
		JsErrorCode result = JsDebugProtocolHandlerWaitForDebugger(ProtocolHandler);

		return result;
	}

	JsErrorCode ProcessCommandQueue()
	{
		return JsDebugProtocolHandlerProcessCommandQueue(ProtocolHandler);
	}

	JsErrorCode SetCommandQueueCallback(JsDebugProtocolHandlerCommandQueueCallback callback, void* callbackState)
	{
		return JsDebugProtocolHandlerSetCommandQueueCallback(ProtocolHandler, callback, callbackState);
	}
};

class FDebugger : public IJavascriptDebugger, public FTickableEditorObject
{
	const FString RuntimeName = "unreal.js";
	JsDebugService Service{ nullptr };
	TSharedPtr<FJsDebugProtocolHandler> ProtocolHandler;

	// used to initialize
	int Port = 0;
	bool bInitialized = false;
	JsRuntimeHandle Runtime{ nullptr };

public:
	virtual void Tick(float DeltaTime) override
	{
		//if (!bInitialized)
		//{
		//	Install();
		//	bInitialized = true;
		//}
		//FContextScope debug_scope(context());

		//ProcessDebugMessages();
	}

	virtual bool IsTickable() const override
	{
		return true;
	}

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FV8Debugger, STATGROUP_Tickables);
	}	

	FDebugger(int32 InPort, JsRuntimeHandle InRuntime)
		: bInitialized(false)
		, Port(InPort)
		, Service(nullptr)
		, Runtime(InRuntime)
	{
		check(InRuntime != JS_INVALID_RUNTIME_HANDLE);
		Install(false);
	}

	~FDebugger()
	{
		Destroy();
	}

	void Destroy() override
	{
		Uninstall();
	}

	void Install(bool bWaitForDebugger)
	{
		// create service
		JsDebugService service;
		JsCheck(JsDebugServiceCreate(&service));
		Service = service;

		// protocol handler
		ProtocolHandler = MakeShared<FJsDebugProtocolHandler>(Runtime);
		JsCheck(JsDebugServiceRegisterHandler(Service, TCHAR_TO_UTF8(*RuntimeName), ProtocolHandler->GetHandle(), bWaitForDebugger));

		// listen
		JsCheck(JsDebugServiceListen(Service, Port));
		UE_LOG(Javascript, Log, TEXT("Debugger listening on ws://127.0.0.1:%d/%s"), Port, *RuntimeName);

		if (bWaitForDebugger)
			JsCheck(ProtocolHandler->WaitForDebugger());
	}

	void Uninstall()
	{
		if (Service)
		{
			JsCheck(JsDebugServiceClose(Service));
			JsCheck(JsDebugServiceUnregisterHandler(Service, TCHAR_TO_UTF8(*RuntimeName)));
			JsCheck(JsDebugServiceDestroy(Service));
			Service = nullptr;
		}

		if (ProtocolHandler.IsValid())
		{
			ProtocolHandler->Destroy();
			ProtocolHandler.Reset();
		}
	}

};

IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort, JsContextRef InContext)
{
	JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
	JsCheck(JsGetRuntime(InContext, &runtime));

	return new FDebugger(InPort, runtime);
}
#else
IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort, JsContextRef InContext)
{
	return nullptr;
} 
#endif

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
