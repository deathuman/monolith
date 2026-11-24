#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"
#include "MonolithJsonUtils.h"
#include "Containers/StringConv.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpResultCallback.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithHttpTransportTests
{
	struct FCapturedResponse
	{
		bool bHandled = false;
		TUniquePtr<FHttpServerResponse> Response;
	};

	static FHttpServerRequest MakeRequest(const FString& Origin, const FString& Body = FString())
	{
		FHttpServerRequest Request;
		if (!Origin.IsEmpty())
		{
			Request.Headers.Add(TEXT("Origin"), {Origin});
		}

		if (!Body.IsEmpty())
		{
			FTCHARToUTF8 Utf8Body(*Body);
			Request.Body.Append(reinterpret_cast<const uint8*>(Utf8Body.Get()), Utf8Body.Length());
		}
		return Request;
	}

	static FString BodyAsString(const TArray<uint8>& Body)
	{
		if (Body.IsEmpty())
		{
			return FString();
		}

		FUTF8ToTCHAR Utf8Body(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
		return FString(Utf8Body.Length(), Utf8Body.Get());
	}

	static const FString* FindHeader(const FHttpServerResponse& Response, const TCHAR* Name)
	{
		const TArray<FString>* Values = Response.Headers.Find(Name);
		return Values != nullptr && Values->Num() > 0 ? &(*Values)[0] : nullptr;
	}

	static TSharedPtr<FJsonObject> ReadToolPayload(const FHttpServerResponse& Response)
	{
		const TSharedPtr<FJsonObject> Envelope = FMonolithJsonUtils::Parse(BodyAsString(Response.Body));
		const TSharedPtr<FJsonObject>* Result = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Envelope.IsValid() || !Envelope->TryGetObjectField(TEXT("result"), Result)
			|| !Result || !Result->IsValid() || !(*Result)->TryGetArrayField(TEXT("content"), Content)
			|| !Content || Content->Num() == 0 || !(*Content)[0].IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Item = nullptr;
		FString Text;
		if (!(*Content)[0]->TryGetObject(Item) || !Item || !Item->IsValid()
			|| !(*Item)->TryGetStringField(TEXT("text"), Text))
		{
			return nullptr;
		}
		return FMonolithJsonUtils::Parse(Text);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithHttpTransportPolicyTest,
	"Monolith.Core.HttpTransport.Policy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpTransportPolicyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithHttpTransportTests;

	// This instance remains stopped. Direct handler calls exercise response
	// policy without binding routes or touching the process-wide HTTP listeners.
	FMonolithHttpServer Server;
	auto Dispatch = [&Server](EHttpServerRequestVerbs Verb, const FHttpServerRequest& Request)
	{
		FCapturedResponse Captured;
		FHttpResultCallback OnComplete = [&Captured](TUniquePtr<FHttpServerResponse>&& Response)
		{
			Captured.Response = MoveTemp(Response);
		};

		switch (Verb)
		{
		case EHttpServerRequestVerbs::VERB_GET:
			Captured.bHandled = Server.HandleGetMcp(Request, OnComplete);
			break;
		case EHttpServerRequestVerbs::VERB_POST:
			Captured.bHandled = Server.HandlePostMcp(Request, OnComplete);
			break;
		case EHttpServerRequestVerbs::VERB_DELETE:
			Captured.bHandled = Server.HandleDeleteMcp(Request, OnComplete);
			break;
		case EHttpServerRequestVerbs::VERB_OPTIONS:
			Captured.bHandled = Server.HandleOptions(Request, OnComplete);
			break;
		default:
			break;
		}
		return Captured;
	};

	const FString AllowedOrigin = TEXT("http://localhost:3000");
	FCapturedResponse GetAllowed = Dispatch(EHttpServerRequestVerbs::VERB_GET, MakeRequest(AllowedOrigin));
	TestTrue(TEXT("GET handler accepts the routed request"), GetAllowed.bHandled);
	TestNotNull(TEXT("GET response is captured"), GetAllowed.Response.Get());
	if (GetAllowed.Response.IsValid())
	{
		const FHttpServerResponse& Response = *GetAllowed.Response;
		TestEqual(TEXT("standalone GET returns 405"), Response.Code, EHttpServerResponseCodes::BadMethod);
		TestEqual(TEXT("405 response body is empty"), Response.Body.Num(), 0);
		TestEqual(TEXT("Allow lists only supported methods"), FindHeader(Response, TEXT("Allow")) != nullptr ? *FindHeader(Response, TEXT("Allow")) : FString(), FString(TEXT("POST, OPTIONS, DELETE")));
		TestNull(TEXT("405 response has no SSE content type"), Response.Headers.Find(TEXT("Content-Type")));
		TestNull(TEXT("405 response has no keep-alive header"), Response.Headers.Find(TEXT("Connection")));
		TestNull(TEXT("405 response has no SSE cache header"), Response.Headers.Find(TEXT("Cache-Control")));
		TestEqual(TEXT("allowlisted origin is echoed"), FindHeader(Response, TEXT("Access-Control-Allow-Origin")) != nullptr ? *FindHeader(Response, TEXT("Access-Control-Allow-Origin")) : FString(), AllowedOrigin);
		TestNotNull(TEXT("normal CORS methods header remains present"), Response.Headers.Find(TEXT("Access-Control-Allow-Methods")));
		TestNotNull(TEXT("normal CORS headers header remains present"), Response.Headers.Find(TEXT("Access-Control-Allow-Headers")));
		TestNotNull(TEXT("normal CORS Vary header remains present"), Response.Headers.Find(TEXT("Vary")));
	}

	FCapturedResponse GetBlocked = Dispatch(EHttpServerRequestVerbs::VERB_GET, MakeRequest(TEXT("https://example.invalid")));
	TestTrue(TEXT("blocked-origin GET handler accepts the routed request"), GetBlocked.bHandled);
	TestNotNull(TEXT("blocked-origin GET response is captured"), GetBlocked.Response.Get());
	if (GetBlocked.Response.IsValid())
	{
		TestNull(TEXT("blocked origin is not echoed"), GetBlocked.Response->Headers.Find(TEXT("Access-Control-Allow-Origin")));
		TestNotNull(TEXT("blocked origin still receives Vary"), GetBlocked.Response->Headers.Find(TEXT("Vary")));
	}

	FCapturedResponse Initialize = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}")));
	TestTrue(TEXT("POST initialize is handled"), Initialize.bHandled);
	TestNotNull(TEXT("POST initialize response is captured"), Initialize.Response.Get());
	if (Initialize.Response.IsValid())
	{
		TestEqual(TEXT("POST initialize remains HTTP 200"), Initialize.Response->Code, EHttpServerResponseCodes::Ok);
		TestTrue(TEXT("POST initialize returns the negotiated protocol"), BodyAsString(Initialize.Response->Body).Contains(TEXT("\"protocolVersion\":\"2025-03-26\"")));
	}

	FCapturedResponse ToolsList = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}")));
	TestTrue(TEXT("POST tools/list is handled"), ToolsList.bHandled);
	TestNotNull(TEXT("POST tools/list response is captured"), ToolsList.Response.Get());
	if (ToolsList.Response.IsValid())
	{
		const FString Body = BodyAsString(ToolsList.Response->Body);
		TestEqual(TEXT("POST tools/list remains HTTP 200"), ToolsList.Response->Code, EHttpServerResponseCodes::Ok);
		TestTrue(TEXT("tools/list still discovers monolith tools"), Body.Contains(TEXT("monolith_discover")) && Body.Contains(TEXT("monolith_status")));
	}

	FCapturedResponse Discover = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"monolith_discover\",\"arguments\":{\"namespace\":\"monolith\"}}}")));
	TestTrue(TEXT("POST monolith_discover is handled"), Discover.bHandled);
	TestNotNull(TEXT("POST monolith_discover response is captured"), Discover.Response.Get());
	if (Discover.Response.IsValid())
	{
		const TSharedPtr<FJsonObject> Payload = ReadToolPayload(*Discover.Response);
		TestEqual(TEXT("POST monolith_discover remains HTTP 200"), Discover.Response->Code, EHttpServerResponseCodes::Ok);
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("monolith_discover returns its action list inside the MCP content payload"),
			Payload.IsValid() && Payload->TryGetArrayField(TEXT("actions"), Actions) && Actions && Actions->Num() > 0);
	}

	FCapturedResponse Status = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"monolith_status\",\"arguments\":{}}}")));
	TestTrue(TEXT("POST monolith_status is handled"), Status.bHandled);
	TestNotNull(TEXT("POST monolith_status response is captured"), Status.Response.Get());
	if (Status.Response.IsValid())
	{
		const TSharedPtr<FJsonObject> Payload = ReadToolPayload(*Status.Response);
		TestEqual(TEXT("POST monolith_status remains HTTP 200"), Status.Response->Code, EHttpServerResponseCodes::Ok);
		bool bServerRunning = false;
		TestTrue(TEXT("monolith_status still returns server status inside the MCP content payload"),
			Payload.IsValid() && Payload->TryGetBoolField(TEXT("server_running"), bServerRunning));
	}

	FCapturedResponse InvalidParams = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{}}")));
	TestNotNull(TEXT("invalid tools/call params response is captured"), InvalidParams.Response.Get());
	if (InvalidParams.Response.IsValid())
	{
		const TSharedPtr<FJsonObject> Envelope = FMonolithJsonUtils::Parse(BodyAsString(InvalidParams.Response->Body));
		const TSharedPtr<FJsonObject>* Error = nullptr;
		double ErrorCode = 0.0;
		TestTrue(TEXT("missing tool name returns a JSON-RPC error object"),
			Envelope.IsValid() && Envelope->TryGetObjectField(TEXT("error"), Error) && Error &&
			(*Error)->TryGetNumberField(TEXT("code"), ErrorCode));
		TestEqual(TEXT("missing tool name keeps JSON-RPC invalid params code"),
			ErrorCode, static_cast<double>(FMonolithJsonUtils::ErrInvalidParams));
	}

	FCapturedResponse StructuredError = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"monolith_missing_for_transport_test\",\"arguments\":{}}}")));
	TestNotNull(TEXT("unknown action response is captured"), StructuredError.Response.Get());
	if (StructuredError.Response.IsValid())
	{
		const TSharedPtr<FJsonObject> Envelope = FMonolithJsonUtils::Parse(BodyAsString(StructuredError.Response->Body));
		const TSharedPtr<FJsonObject>* Result = nullptr;
		const TSharedPtr<FJsonObject>* StructuredContent = nullptr;
		const TSharedPtr<FJsonObject>* Error = nullptr;
		double ErrorCode = 0.0;
		bool bIsError = false;
		TestTrue(TEXT("unknown action remains a structured MCP tool error"),
			Envelope.IsValid() && Envelope->TryGetObjectField(TEXT("result"), Result) && Result &&
			(*Result)->TryGetBoolField(TEXT("isError"), bIsError) && bIsError &&
			(*Result)->TryGetObjectField(TEXT("structuredContent"), StructuredContent) && StructuredContent &&
			(*StructuredContent)->TryGetObjectField(TEXT("error"), Error) && Error &&
			(*Error)->TryGetNumberField(TEXT("code"), ErrorCode));
		TestEqual(TEXT("unknown action retains method-not-found code in MCP error data"),
			ErrorCode, static_cast<double>(FMonolithJsonUtils::ErrMethodNotFound));
		TestTrue(TEXT("structured MCP error retains its data payload"), Error && (*Error)->HasField(TEXT("data")));
	}

	FCapturedResponse Notification = Dispatch(
		EHttpServerRequestVerbs::VERB_POST,
		MakeRequest(FString(), TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")));
	TestTrue(TEXT("POST initialized notification is handled"), Notification.bHandled);
	TestNotNull(TEXT("POST notification response is captured"), Notification.Response.Get());
	if (Notification.Response.IsValid())
	{
		TestEqual(TEXT("POST notification remains HTTP 202"), Notification.Response->Code, EHttpServerResponseCodes::Accepted);
		TestEqual(TEXT("POST notification response body remains empty"), Notification.Response->Body.Num(), 0);
	}

	FCapturedResponse Options = Dispatch(EHttpServerRequestVerbs::VERB_OPTIONS, MakeRequest(AllowedOrigin));
	TestTrue(TEXT("OPTIONS handler remains available"), Options.bHandled);
	TestNotNull(TEXT("OPTIONS response is captured"), Options.Response.Get());
	if (Options.Response.IsValid())
	{
		TestEqual(TEXT("OPTIONS behavior remains 204"), Options.Response->Code, EHttpServerResponseCodes::NoContent);
		TestEqual(TEXT("OPTIONS response remains empty"), Options.Response->Body.Num(), 0);
		TestEqual(TEXT("OPTIONS still echoes allowed origin"), FindHeader(*Options.Response, TEXT("Access-Control-Allow-Origin")) != nullptr ? *FindHeader(*Options.Response, TEXT("Access-Control-Allow-Origin")) : FString(), AllowedOrigin);
	}

	FCapturedResponse Delete = Dispatch(EHttpServerRequestVerbs::VERB_DELETE, MakeRequest(AllowedOrigin));
	TestTrue(TEXT("DELETE handler remains available"), Delete.bHandled);
	TestNotNull(TEXT("DELETE response is captured"), Delete.Response.Get());
	if (Delete.Response.IsValid())
	{
		TestEqual(TEXT("DELETE behavior remains 204"), Delete.Response->Code, EHttpServerResponseCodes::NoContent);
		TestEqual(TEXT("DELETE response remains empty"), Delete.Response->Body.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
