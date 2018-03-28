#include "V8PCH.h"

PRAGMA_DISABLE_SHADOW_VARIABLE_WARNINGS

#if !PLATFORM_LINUX
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptContext_Private.h"
#include "SocketSubSystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "NetworkMessage.h"
#include "Misc/DefaultValueHelper.h"

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#endif
#include <thread>
#if PLATFORM_WINDOWS
#include "HideWindowsPlatformTypes.h"
#endif

#define DEBUG_V8_DEBUGGER 1

#include "ScopeLock.h"
#include "Translator.h"
#include "Helpers.h"
#include "ChakraDebug.h"
#endif

#if V8_MAJOR_VERSION == 5 && V8_MINOR_VERSION < 5
#	define V8_Debug_ProcessDebugMessages(isolate) Debug::ProcessDebugMessages()
#	define V8_Debug_SetMessageHandler(isolate,fn) Debug::SetMessageHandler(fn)
#else
#	define V8_Debug_ProcessDebugMessages(isolate) Debug::ProcessDebugMessages(isolate)
#	define V8_Debug_SetMessageHandler(isolate,fn) Debug::SetDebugEventListener(isolate,fn)
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

class FDebugger : public IJavascriptDebugger, public FTickableEditorObject
{
public:
	//struct FClientData : public Debug::ClientData
	//{
	//	FClientData(FDebugger* InDebugger, FSocket* InSocket)
	//	: Debugger(InDebugger), Socket_(InSocket)
	//	{
	//		Debugger->Busy.Increment();
	//	}

	//	~FClientData()
	//	{
	//		Debugger->Busy.Decrement();
	//	}

	//	FDebugger* Debugger;
	//	FSocket* Socket_;

	//	void Send(const char* msg)
	//	{
	//		SendTo(Socket_, msg);
	//	}
	//};

	std::thread thread;
	JsRuntimeHandle runtime_;
	Persistent<JsContextRef> context_;

	TArray<FString> commands_;
	FCriticalSection commandsLock_;

	TArray<std::function<void()>> asyncBreakCallbacks;

	bool breakState = false;

	static Persistent<JsValueRef> jsonStringifier;
	static Persistent<JsValueRef> jsonParser;

	JsContextRef context() const { return context_.Get(); }

	virtual void Tick(float DeltaTime) override
	{
		FContextScope debug_scope(context());

		ProcessDebugMessages();
	}

	virtual bool IsTickable() const override
	{
		return true;
	}

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FV8Debugger, STATGROUP_Tickables);
	}	

	FDebugger(int32 InPort, JsContextRef context)
	{
		context_.Reset(context);
		JsCheck(JsGetRuntime(context, &runtime_));

		StopRequested.Reset();
		StopAck.Reset();

		Install();

		thread = std::thread([this, InPort]{Main(InPort); });				
	}

	static FString DumpJSON(JsValueRef value,int32 depth = 1) 
	{
		if (depth <= 0) return TEXT("<deep>");

		if (chakra::IsEmpty(value)) return TEXT("(empty)");
		if (!chakra::IsObject(value))
		{
			if (chakra::IsString(value))
				return FString::Printf(TEXT("\"%s\""), *chakra::StringFromChakra(value));

			JsCheck(JsConvertValueToString(value, &value));
			return chakra::StringFromChakra(value);
		}

		JsValueRef Obj = value;
		TArray<FString> Names = chakra::PropertyNames(Obj);
		TArray<FString> values;
		for (const FString& Name : Names)
		{
			JsValueRef Value = chakra::GetProperty(Obj, Name);
			values.Add(FString::Printf(TEXT("\"%s\":%s"), *Name, *DumpJSON(Value,depth-1)));
		}
		return FString::Printf(TEXT("{%s}"), *FString::Join(values,TEXT(",")));
	}

#define VAR_STR(value, name) FString name = chakra::StringFromChakra(chakra::GetProperty(value, #name));
#define VAR_INT(value, name) int name = chakra::IntFrom(chakra::GetProperty(value, #name));
#define VAR_BOOL(value, name) bool name = chakra::BoolFrom(chakra::GetProperty(value, #name));

	void Install()
	{
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsCheck(JsGetRuntime(context(), &runtime));

		JsValueRef stringifier = JS_INVALID_REFERENCE;
		JsCheck(JsRunScript(TEXT("JSON.stringify"), 0, TEXT(""), &stringifier));
		jsonStringifier.Reset(stringifier);

		JsValueRef parser = JS_INVALID_REFERENCE;
		JsCheck(JsRunScript(TEXT("JSON.parse"), 0, TEXT(""), &parser));
		jsonParser.Reset(parser);

		JsCheck(JsDiagStartDebugging(runtime, HandleDebugEvents, this));

#if DEBUG_V8_DEBUGGER
		//Debug::SetDebugEventListener([](const v8::Debug::EventDetails& event_details) {
		//	static const TCHAR* EventTypes[] = { 
		//		TEXT("Break"), 
		//		TEXT("Exception"), 
		//		TEXT("NewFunction"), 
		//		TEXT("BeforeCompile"), 
		//		TEXT("AfterCompile"), 
		//		TEXT("CompileError"), 
		//		TEXT("PromiseEvent"), 
		//		TEXT("AsyncTaskEvent") 
		//	};
		//	UE_LOG(Javascript, Log, TEXT("DebugEvent Event(%s) State(%s) Data(%s)"), 
		//		EventTypes[event_details.GetEvent()],
		//		*DumpJSON(event_details.GetExecutionState()),
		//		*DumpJSON(event_details.GetEventData())
		//		);
		//});
#endif
	}

	void Uninstall()
	{		
		FContextScope context_scope(context());
		JsRuntimeHandle runtime = JS_INVALID_RUNTIME_HANDLE;
		JsCheck(JsGetRuntime(context(), &runtime));

		FDebugger* self = nullptr;
		JsCheck(JsDiagStopDebugging(runtime, reinterpret_cast<void**>(&self)));
		check(self == this);
	}

	static void HandleDebugEvents(JsDiagDebugEvent debugEvent, JsValueRef eventData, void* callbackState)
	{
		FDebugger* self = reinterpret_cast<FDebugger*>(callbackState);
		switch (debugEvent)
		{
		case JsDiagDebugEvent::JsDiagDebugEventSourceCompile:
			{
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventSourceCompile"));
			VAR_STR(eventData, scriptType);
			VAR_INT(eventData, lineCount);
			VAR_INT(eventData, sourceLength);
			VAR_INT(eventData, scriptId);
			VAR_INT(eventData, parentScriptId);
			}
			break;

		case JsDiagDebugEvent::JsDiagDebugEventBreakpoint:
			{
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventBreakpoint"));
			VAR_INT(eventData, breakpointId);
			VAR_INT(eventData, scriptId);
			VAR_INT(eventData, line);
			VAR_INT(eventData, column);
			VAR_INT(eventData, sourceLength);
			VAR_STR(eventData, sourceText);
			}
			break;

		case JsDiagDebugEvent::JsDiagDebugEventStepComplete:
			{
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventStepComplete"));
			VAR_INT(eventData, scriptId);
			VAR_INT(eventData, line);
			VAR_INT(eventData, column);
			VAR_INT(eventData, sourceLength);
			VAR_STR(eventData, sourceText);
			}
			break;

		case JsDiagDebugEvent::JsDiagDebugEventDebuggerStatement:
			{
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventDebuggerStatement"));
			VAR_INT(eventData, scriptId);
			VAR_INT(eventData, line);
			VAR_INT(eventData, column);
			VAR_INT(eventData, sourceLength);
			VAR_STR(eventData, sourceText);
			}
			break;

		case JsDiagDebugEvent::JsDiagDebugEventRuntimeException:
			{
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventRuntimeException"));
			VAR_INT(eventData, scriptId);
			VAR_INT(eventData, line);
			VAR_INT(eventData, column);
			VAR_INT(eventData, sourceLength);
			VAR_STR(eventData, sourceText);
			VAR_BOOL(eventData, uncaught);
			JsValueRef exception = chakra::GetProperty(eventData, "exception");

			VAR_STR(exception, name);
			VAR_STR(exception, type);
			VAR_STR(exception, className);
			VAR_STR(exception, display);
			VAR_INT(exception, propertyAttributes);
			VAR_INT(exception, handle);
			}
			break;

		case JsDiagDebugEvent::JsDiagDebugEventAsyncBreak:
			UE_LOG(Javascript, Log, TEXT("JsDiagDebugEventAsyncBreak"));
			self->breakState = true;

			TArray<std::function<void()>> callbacks = self->asyncBreakCallbacks;
			self->asyncBreakCallbacks.Empty();
			for (auto& cb : callbacks)
				cb();

			break;
		}

		//UE_LOG(Javascript, Log, TEXT("V8 message : %s %s"), message.IsEvent() ? TEXT("(event)") : TEXT(""), message.IsResponse() ? TEXT("(response)") : TEXT("") );			

		if (chakra::IsEmpty(eventData))
		{
			UE_LOG(Javascript, Error, TEXT("Not a json"));
		}
		else
		{
			//JsValueRef json = JS_INVALID_REFERENCE;
			//JsValueRef args[] = { chakra::Undefined(), eventData };
			//JsCheck(JsCallFunction(jsonStringifier.Get(), args, 2, &json));

			//FString jsonStr = chakra::StringFromChakra(json);
			FString jsonStr = DumpJSON(eventData, 2);
			UE_LOG(Javascript, Log, TEXT("Debug message: %s"), *jsonStr);

			//Broadcast(TCHAR_TO_UTF8(*jsonStr));
		}
	}

	~FDebugger()
	{
		Uninstall();

		context_.Reset();
	}

	virtual void Destroy() override
	{
		Stop();

		delete this;
	}

	void Stop()
	{
		StopRequested.Set(true);

		if (WorkingOn)
		{
			WorkingOn->Close();
		}

		while (!StopAck.GetValue())
		{
			ProcessDebugMessages();
		}

		thread.join();
	}

	void ProcessDebugMessages()
	{
		if (commands_.Num() == 0)
			return;

		commandsLock_.Lock();
		TArray<FString> commands = commands_;
		commands_.Empty();
		commandsLock_.Unlock();

		for (const FString& cmd : commands)
		{
			JsValueRef cmdValue = JS_INVALID_REFERENCE;
			JsCheck(JsRunScript(*FString::Printf(TEXT("(() => (%s))()"), *cmd), 0, TEXT(""), &cmdValue));

			VAR_STR(cmdValue, command);
			JsValueRef argument = chakra::GetProperty(cmdValue, "arguments");
			if (command == "evaluate")
			{
				VAR_STR(argument, expression);
				RequestAsyncBreak([expression]() {
					JsValueRef ret = JS_INVALID_REFERENCE;
					JsValueRef result = JS_INVALID_REFERENCE;

					//JsErrorCode evalErr = JsRunScript(*expression, 0, TEXT(""), &result));

					JsErrorCode evalErr = JsDiagEvaluate(chakra::String(expression), 0, JsParseScriptAttributeNone, false, &result);
					if (evalErr == JsNoError)
					{
						///     {
						///         "name" : "this",
						///         "type" : "object",
						///         "className" : "Object",
						///         "display" : "{...}",
						///         "propertyAttributes" : 1,
						///         "handle" : 18
						///     }

						JsValueRef remoteObject = JS_INVALID_REFERENCE;
						JsCheck(JsCreateObject(&remoteObject));

						chakra::SetProperty(remoteObject, "type", chakra::GetProperty(result, "type"));
						chakra::SetProperty(remoteObject, "className", chakra::GetProperty(result, "className"));
						chakra::SetProperty(remoteObject, "description", chakra::GetProperty(result, "display"));
						chakra::SetProperty(remoteObject, "objectId", chakra::GetProperty(result, "handle"));

						JsCheck(JsCreateObject(&result));
						chakra::SetProperty(result, "result", remoteObject);
					}
					else if (evalErr == JsErrorScriptException)
					{

						///     {
						///         "name" : "a.b.c",
						///         "type" : "object",
						///         "className" : "Error",
						///         "display" : "'a' is undefined",
						///         "propertyAttributes" : 1,
						///         "handle" : 18
						///     }

						JsValueRef exceptionDetails = JS_INVALID_REFERENCE;
						JsCheck(JsCreateObject(&exceptionDetails));

						chakra::SetProperty(exceptionDetails, "exceptionId", chakra::GetProperty(result, "handle"));

						JsCheck(JsCreateObject(&result));
						chakra::SetProperty(result, "exceptionDetails", exceptionDetails);
					}
					else
					{
						check(false);
					}

					JsValueRef strResult = JS_INVALID_REFERENCE;
					JsValueRef args[] = { chakra::Undefined(), result };
					JsCheck(JsCallFunction(jsonStringifier.Get(), args, 2, &strResult));

					Broadcast(TCHAR_TO_UTF8(*chakra::StringFromChakra(strResult)));
				});
			}
			else
			{
				UE_LOG(Javascript, Warning, TEXT("unhandled debug command %s"), *cmd);
			}
		}
	}

	FSocket* WorkingOn{ nullptr };
	
	bool Main(int32 Port)
	{
		auto SocketSubsystem = ISocketSubsystem::Get();
		if (!SocketSubsystem) return ReportError("No socket subsystem");

		auto Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("V8 debugger"));
		if (!Socket) return ReportError("Failed to create socket");

		auto Cleanup_Socket = [&]{Socket->Close(); SocketSubsystem->DestroySocket(Socket); };
		
		// listen on any IP address
		auto ListenAddr = SocketSubsystem->GetLocalBindAddr(*GLog);

		ListenAddr->SetPort(Port);
		Socket->SetReuseAddr();

		// bind to the address
		if (!Socket->Bind(*ListenAddr)) return ReportError("Failed to bind socket", Cleanup_Socket );

		if (!Socket->Listen(16)) return ReportError("Failed to listen", Cleanup_Socket );
				
		int32 port = Socket->GetPortNo();
		check((Port == 0 && port != 0) || port == Port);

		ListenAddr->SetPort(port);		

		while (!StopRequested.GetValue())
		{
			bool bReadReady = false;

			// check for incoming connections
			if (Socket->HasPendingConnection(bReadReady) && bReadReady)
			{
				auto ClientSocket = Socket->Accept(TEXT("Remote V8 connection"));

				if (!ClientSocket) continue;				

				RegisterDebuggerSocket(ClientSocket);

				UE_LOG(Javascript, Log, TEXT("V8 Debugger session start"));

				WorkingOn = ClientSocket;

				Logic(ClientSocket);

				WorkingOn = nullptr;

				UE_LOG(Javascript, Log, TEXT("V8 Debugger session stop"));

				while (Busy.GetValue() != 0)
				{
					FPlatformProcess::Sleep(0.25f);
				}

				UnregisterDebuggerSocket(ClientSocket);
				
				ClientSocket->Close();
				SocketSubsystem->DestroySocket(ClientSocket);
			}
			else
			{
				FPlatformProcess::Sleep(0.25f);
			}		
		}

		Cleanup_Socket();		

		StopAck.Set(true);

		return true;
	}

	bool Logic(FSocket* InSocket)
	{
		FSimpleAbstractSocket_FSocket Socket(InSocket);

		auto send = [&](const char* msg) {
			Socket.Send(reinterpret_cast<const uint8*>(msg), strlen(msg));
		};

		auto read_line = [&](FString& line) {
			const auto buffer_size = 1024;
			ANSICHAR buffer[buffer_size];
			for (int pos = 0; pos < buffer_size; pos++)			
			{
				char ch;
				if (!Socket.Receive(reinterpret_cast<uint8*>(&ch), 1)) return false;

				if (pos && buffer[pos - 1] == '\r' && ch == '\n')
				{
					buffer[pos - 1] = '\0';
					line = UTF8_TO_TCHAR(buffer);
					return true;
				}
				else if (ch == '\n')
				{
					buffer[pos] = '\0';
					line = UTF8_TO_TCHAR(buffer);
					return true;
				}
				buffer[pos] = ch;				
			}
			return false;
		};

		auto command = [&](const FString& request) {
			FScopeLock Lock(&commandsLock_);
			commands_.Add(request);
			//Debug::SendCommand(isolate_, (uint16_t*)*request, request.Len(), new FClientData(this,InSocket));
		};

		auto process = [&](const FString& content) {
#if DEBUG_V8_DEBUGGER
			UE_LOG(Javascript, Log, TEXT("Received: %s"), *content);
#endif
			command(content);
			return content.Find(TEXT("\"type\":\"request\",\"command\":\"disconnect\"}")) == INDEX_NONE;
		};

		auto bye = [&] {
			command(TEXT("{\"seq\":1,\"type:\":\"request\",\"command\":\"disconnect\"}"));
		};

		send("Type: connect\r\n");
		send("V8-Version: ");
		send(TCHAR_TO_ANSI(*FString::Printf(TEXT("8.6.0"))));
		send("\r\n");
		send("Protocol-Version: 1\r\n");
		send("Embedding-Host: UnrealEngine\r\n");
		send("Content-Length: 0\r\n");
		send("\r\n");		

		FString json_text;
		int32 content_length = -1;

		while (!StopRequested.GetValue())
		{
			FString line, left, right;
			if (!read_line(line)) break;			

			if (line.Split(TEXT(":"), &left, &right))
			{				
				if (left == TEXT("Content-Length"))
				{
					if (!FDefaultValueHelper::ParseInt(right, content_length)) return ReportError("invalid content length");
				}
			}
			else
			{	
				if (content_length <= 0) return ReportError("invalid content length < 0");

				char* buffer = new char[content_length+1];
				if (!buffer) break;

				if (!Socket.Receive(reinterpret_cast<uint8*>(buffer), content_length)) break;

				buffer[content_length] = '\0';
				if (!process(buffer)) return ReportError("failed to process");				
			}			
		}

		bye();

		return true;
	}	

	bool ReportError(const char* Error)
	{
		UE_LOG(Javascript, Error, TEXT("%s"), UTF8_TO_TCHAR(Error));

		// always false
		return false;
	}

	template <typename Fn>
	bool ReportError(const char* Error, Fn&& fn)
	{		
		fn();
		return ReportError(Error);
	}

	virtual bool IsBreak() override
	{
		return breakState;
	}

	void RequestAsyncBreak(std::function<void()> callback)
	{
		JsCheck(JsDiagRequestAsyncBreak(runtime_));
		asyncBreakCallbacks.Add(callback);
	}

	FThreadSafeCounter StopAck;
	FThreadSafeCounter StopRequested;	
	FThreadSafeCounter Busy;
};

Persistent<JsValueRef> FDebugger::jsonStringifier;
Persistent<JsValueRef> FDebugger::jsonParser;

IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort, JsContextRef InContext)
{
	return new FDebugger(InPort,InContext);
}
#else
IJavascriptDebugger* IJavascriptDebugger::Create(int32 InPort, JsContextRef InContext)
{
	return nullptr;
} 
#endif

PRAGMA_ENABLE_SHADOW_VARIABLE_WARNINGS
