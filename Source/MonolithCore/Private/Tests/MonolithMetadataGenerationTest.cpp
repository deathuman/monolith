#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithMetadataGenerationTestDetail
{
	static FString MakeTemporaryNamespace()
	{
		return TEXT("__metadata_generation_test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	static FMonolithActionResult NoOpHandler(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	}

	static TSharedPtr<FJsonObject> MakeSchema(const FString& Alias, const FString& ArrayFirst)
	{
		TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Aliases;
		Aliases.Add(MakeShared<FJsonValueString>(Alias));
		Param->SetArrayField(TEXT("aliases"), Aliases);

		TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Add(MakeShared<FJsonValueString>(ArrayFirst));
		Items.Add(MakeShared<FJsonValueString>(TEXT("second")));
		Nested->SetArrayField(TEXT("enum"), Items);
		Param->SetObjectField(TEXT("nested"), Nested);

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetObjectField(TEXT("input"), Param);
		return Schema;
	}

	static FString MakeEmbeddedNullText(const TCHAR* Tail)
	{
		const TCHAR Prefix[] = TEXT("prefix");
		TArray<TCHAR> Characters;
		Characters.Append(Prefix, UE_ARRAY_COUNT(Prefix) - 1);
		Characters.Add(static_cast<TCHAR>(0));
		for (const TCHAR* Character = Tail; Character && *Character; ++Character)
		{
			Characters.Add(*Character);
		}
		return FString::ConstructFromPtrSize(Characters.GetData(), Characters.Num());
	}

	static FString ReadAlias(const FMonolithActionInfo& Info)
	{
		if (!Info.ParamSchema.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* Param = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
		if (!Info.ParamSchema->TryGetObjectField(TEXT("input"), Param) || !Param ||
			!(*Param)->TryGetArrayField(TEXT("aliases"), Aliases) || !Aliases || Aliases->Num() == 0)
		{
			return FString();
		}
		return (*Aliases)[0].IsValid() ? (*Aliases)[0]->AsString() : FString();
	}

	static FMonolithActionResult ReadRegisteredStatus()
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("monolith"), TEXT("status"), nullptr);
	}

	static FMonolithActionResult ReadRegisteredSchema(const FString& Namespace, const FString& Action)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target_namespace"), Namespace);
		Params->SetStringField(TEXT("target_action"), Action);
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("describe"), TEXT("action_schema"), Params);
	}

	static bool ReadStatusIdentity(
		const FMonolithActionResult& Result,
		FString& OutGeneration,
		int32& OutActionCount,
		int32& OutNamespaceCount)
	{
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			return false;
		}
		double ActionCount = 0.0;
		double NamespaceCount = 0.0;
		if (!Result.Result->TryGetStringField(TEXT("metadata_generation"), OutGeneration) ||
			!Result.Result->TryGetNumberField(TEXT("total_actions"), ActionCount) ||
			!Result.Result->TryGetNumberField(TEXT("namespaces"), NamespaceCount))
		{
			return false;
		}
		OutActionCount = static_cast<int32>(ActionCount);
		OutNamespaceCount = static_cast<int32>(NamespaceCount);
		return true;
	}

	static FString ReadDescribedAlias(const FMonolithActionResult& Result)
	{
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Result.Result->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			return FString();
		}
		FMonolithActionInfo Info;
		Info.ParamSchema = *Params;
		return ReadAlias(Info);
	}

	static const FMonolithActionInfo* FindAction(
		const TArray<FMonolithActionInfo>& Actions,
		const FString& Namespace,
		const FString& Action)
	{
		return Actions.FindByPredicate([&](const FMonolithActionInfo& Info)
		{
			return Info.Namespace == Namespace && Info.Action == Action;
		});
	}

	struct FWeakTarget
	{
		explicit FWeakTarget(const TSharedPtr<int32>& InInvocationCount)
			: InvocationCount(InInvocationCount)
		{
		}

		FMonolithActionResult Invoke(const TSharedPtr<FJsonObject>& /*Params*/)
		{
			++*InvocationCount;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		}

		TSharedPtr<int32> InvocationCount;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationCanonicalEncodingTest,
	"Monolith.Core.MetadataGeneration.CanonicalEncoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationCanonicalEncodingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithActionInfo First;
	First.Namespace = TEXT("metadata_fixture");
	First.Action = TEXT("first");
	First.Description = TEXT("Full description");
	First.Category = TEXT("fixture");
	First.ParamSchema = MakeSchema(TEXT("legacy"), TEXT("first"));
	First.bReadOnlyHint = true;
	First.Title = TEXT("Fixture title");

	FMonolithActionInfo Second;
	Second.Namespace = TEXT("metadata_fixture");
	Second.Action = TEXT("second");
	Second.Description = TEXT("Second action");
	Second.ParamSchema = MakeShared<FJsonObject>();

	TMap<FString, FMonolithDispatcherAnnotations> Dispatchers;
	FMonolithDispatcherAnnotations ActiveDispatcher;
	ActiveDispatcher.bIdempotentHint = true;
	ActiveDispatcher.Title = TEXT("Active dispatcher");
	Dispatchers.Add(First.Namespace, ActiveDispatcher);
	FMonolithDispatcherAnnotations RemovedDispatcher;
	RemovedDispatcher.Title = TEXT("Must be ignored while namespace is inactive");
	Dispatchers.Add(TEXT("removed_namespace"), RemovedDispatcher);

	TArray<FMonolithActionInfo> ForwardActions;
	ForwardActions.Add(First);
	ForwardActions.Add(Second);
	TArray<FMonolithActionInfo> ReverseActions;
	ReverseActions.Add(Second);
	ReverseActions.Add(First);
	FString DigestForward;
	FString DigestReverse;
	FString Error;
	TestTrue(TEXT("canonical digest encodes forward action order"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			ForwardActions, Dispatchers, DigestForward, Error));
	TestTrue(TEXT("canonical digest encodes reverse action order"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			ReverseActions, Dispatchers, DigestReverse, Error));
	TestEqual(TEXT("action registration order does not affect structural digest"),
		DigestForward, DigestReverse);

	TSharedPtr<FJsonObject> KeyReorderedSchema = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SameItems;
	SameItems.Add(MakeShared<FJsonValueString>(TEXT("first")));
	SameItems.Add(MakeShared<FJsonValueString>(TEXT("second")));
	Nested->SetArrayField(TEXT("enum"), SameItems);
	Param->SetObjectField(TEXT("nested"), Nested);
	TArray<TSharedPtr<FJsonValue>> Aliases;
	Aliases.Add(MakeShared<FJsonValueString>(TEXT("legacy")));
	Param->SetArrayField(TEXT("aliases"), Aliases);
	KeyReorderedSchema->SetObjectField(TEXT("input"), Param);
	FMonolithActionInfo KeyReorderedFirst = First;
	KeyReorderedFirst.ParamSchema = KeyReorderedSchema;
	TArray<FMonolithActionInfo> KeyReorderedActions;
	KeyReorderedActions.Add(KeyReorderedFirst);
	KeyReorderedActions.Add(Second);
	FString DigestKeysReordered;
	TestTrue(TEXT("object-key-order fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			KeyReorderedActions, Dispatchers, DigestKeysReordered, Error));
	TestEqual(TEXT("object insertion order does not affect structural digest"),
		DigestForward, DigestKeysReordered);

	const FString EmbeddedValueA = MakeEmbeddedNullText(TEXT("tail_a"));
	const FString EmbeddedValueB = MakeEmbeddedNullText(TEXT("tail_b"));
	FMonolithActionInfo EmbeddedValueActionA = Second;
	EmbeddedValueActionA.Action = TEXT("embedded_value");
	EmbeddedValueActionA.ParamSchema = MakeShared<FJsonObject>();
	EmbeddedValueActionA.ParamSchema->SetStringField(TEXT("value"), EmbeddedValueA);
	FMonolithActionInfo EmbeddedValueActionB = EmbeddedValueActionA;
	EmbeddedValueActionB.ParamSchema = MakeShared<FJsonObject>();
	EmbeddedValueActionB.ParamSchema->SetStringField(TEXT("value"), EmbeddedValueB);
	TArray<FMonolithActionInfo> EmbeddedValueActionsA;
	EmbeddedValueActionsA.Add(EmbeddedValueActionA);
	TArray<FMonolithActionInfo> EmbeddedValueActionsB;
	EmbeddedValueActionsB.Add(EmbeddedValueActionB);
	FString DigestEmbeddedValueA;
	FString DigestEmbeddedValueB;
	TestTrue(TEXT("embedded-NUL string value fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			EmbeddedValueActionsA, Dispatchers, DigestEmbeddedValueA, Error));
	TestTrue(TEXT("embedded-NUL string tail fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			EmbeddedValueActionsB, Dispatchers, DigestEmbeddedValueB, Error));
	TestNotEqual(TEXT("string tail after embedded NUL affects structural digest"),
		DigestEmbeddedValueA, DigestEmbeddedValueB);

	const FString EmbeddedKeyA = MakeEmbeddedNullText(TEXT("key_tail_a"));
	const FString EmbeddedKeyB = MakeEmbeddedNullText(TEXT("key_tail_b"));
	FMonolithActionInfo EmbeddedKeyActionA = Second;
	EmbeddedKeyActionA.Action = TEXT("embedded_key");
	EmbeddedKeyActionA.ParamSchema = MakeShared<FJsonObject>();
	EmbeddedKeyActionA.ParamSchema->SetField(FStringView(EmbeddedKeyA),
		MakeShared<FJsonValueString>(TEXT("same_value")));
	FMonolithActionInfo EmbeddedKeyActionB = EmbeddedKeyActionA;
	EmbeddedKeyActionB.ParamSchema = MakeShared<FJsonObject>();
	EmbeddedKeyActionB.ParamSchema->SetField(FStringView(EmbeddedKeyB),
		MakeShared<FJsonValueString>(TEXT("same_value")));
	TArray<FMonolithActionInfo> EmbeddedKeyActionsA;
	EmbeddedKeyActionsA.Add(EmbeddedKeyActionA);
	TArray<FMonolithActionInfo> EmbeddedKeyActionsB;
	EmbeddedKeyActionsB.Add(EmbeddedKeyActionB);
	FString DigestEmbeddedKeyA;
	FString DigestEmbeddedKeyB;
	TestTrue(TEXT("embedded-NUL schema key fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			EmbeddedKeyActionsA, Dispatchers, DigestEmbeddedKeyA, Error));
	TestTrue(TEXT("embedded-NUL schema key tail fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			EmbeddedKeyActionsB, Dispatchers, DigestEmbeddedKeyB, Error));
	TestNotEqual(TEXT("schema key tail after embedded NUL affects structural digest"),
		DigestEmbeddedKeyA, DigestEmbeddedKeyB);

	TSharedPtr<FJsonObject> ReorderedSchema = MakeSchema(TEXT("legacy"), TEXT("second"));
	TSharedPtr<FJsonObject> ArrayParam;
	const TSharedPtr<FJsonObject>* ArrayParamPtr = nullptr;
	ReorderedSchema->TryGetObjectField(TEXT("input"), ArrayParamPtr);
	if (ArrayParamPtr && ArrayParamPtr->IsValid())
	{
		ArrayParam = *ArrayParamPtr;
	}
	TSharedPtr<FJsonObject> ArrayNested;
	const TSharedPtr<FJsonObject>* ArrayNestedPtr = nullptr;
	ArrayParam->TryGetObjectField(TEXT("nested"), ArrayNestedPtr);
	if (ArrayNestedPtr && ArrayNestedPtr->IsValid())
	{
		ArrayNested = *ArrayNestedPtr;
	}
	TArray<TSharedPtr<FJsonValue>> ReversedItems;
	ReversedItems.Add(MakeShared<FJsonValueString>(TEXT("second")));
	ReversedItems.Add(MakeShared<FJsonValueString>(TEXT("first")));
	ArrayNested->SetArrayField(TEXT("enum"), ReversedItems);
	First.ParamSchema = ReorderedSchema;
	TArray<FMonolithActionInfo> ArrayReorderedActions;
	ArrayReorderedActions.Add(First);
	ArrayReorderedActions.Add(Second);
	FString DigestArrayReordered;
	TestTrue(TEXT("array-order fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			ArrayReorderedActions, Dispatchers, DigestArrayReordered, Error));
	TestNotEqual(TEXT("array order remains meaningful"), DigestForward, DigestArrayReordered);

	TArray<FMonolithActionInfo> NoSchemaActions;
	FMonolithActionInfo NoSchemaInfo = Second;
	NoSchemaInfo.ParamSchema.Reset();
	NoSchemaActions.Add(NoSchemaInfo);
	TArray<FMonolithActionInfo> EmptySchemaActions;
	EmptySchemaActions.Add(Second);
	FString NoSchemaDigest;
	FString EmptySchemaDigest;
	TestTrue(TEXT("null schema fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			NoSchemaActions, Dispatchers, NoSchemaDigest, Error));
	TestTrue(TEXT("empty schema fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			EmptySchemaActions, Dispatchers, EmptySchemaDigest, Error));
	TestNotEqual(TEXT("missing schema differs from an empty object schema"),
		NoSchemaDigest, EmptySchemaDigest);

	FMonolithDispatcherAnnotations ChangedDispatcher = ActiveDispatcher;
	ChangedDispatcher.Title = TEXT("Changed active dispatcher");
	Dispatchers.Add(First.Namespace, ChangedDispatcher);
	FString DigestChangedDispatcher;
	TestTrue(TEXT("changed dispatcher annotation fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			ForwardActions, Dispatchers, DigestChangedDispatcher, Error));
	TestNotEqual(TEXT("active dispatcher annotations affect structural digest"),
		DigestForward, DigestChangedDispatcher);

	Dispatchers.Add(TEXT("removed_namespace"), FMonolithDispatcherAnnotations{});
	FString DigestChangedInactive;
	TestTrue(TEXT("inactive dispatcher fixture encodes"),
		FMonolithToolRegistry::ComputeStructuralDigestForTesting(
			ForwardActions, Dispatchers, DigestChangedInactive, Error));
	TestEqual(TEXT("inactive dispatcher annotations do not affect digest"),
		DigestChangedDispatcher, DigestChangedInactive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationSchemaOwnershipTest,
	"Monolith.Core.MetadataGeneration.SchemaOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationSchemaOwnershipTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FMonolithMetadataSnapshot Baseline = Registry.GetMetadataSnapshot();
	const FString Namespace = MakeTemporaryNamespace();
	ON_SCOPE_EXIT
	{
		Registry.UnregisterNamespace(Namespace);
	};

	TSharedPtr<FJsonObject> InputSchema = MakeSchema(TEXT("original_alias"), TEXT("original_first"));
	const TSharedPtr<FJsonObject>* InputParamPtr = nullptr;
	InputSchema->TryGetObjectField(TEXT("input"), InputParamPtr);
	const TSharedPtr<FJsonObject> InputParam = InputParamPtr ? *InputParamPtr : nullptr;
	const TSharedPtr<FJsonObject>* InputNestedPtr = nullptr;
	InputParam->TryGetObjectField(TEXT("nested"), InputNestedPtr);
	const TSharedPtr<FJsonObject> InputNested = InputNestedPtr ? *InputNestedPtr : nullptr;
	const FString EmbeddedSchemaKey = MakeEmbeddedNullText(TEXT("owned_key_tail"));
	InputNested->SetStringField(FStringView(EmbeddedSchemaKey), TEXT("owned key value"));
	Registry.RegisterAction(Namespace, TEXT("owned"), TEXT("owned schema"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), InputSchema);

	TArray<TSharedPtr<FJsonValue>> CallerAliases;
	CallerAliases.Add(MakeShared<FJsonValueString>(TEXT("caller_mutation")));
	InputParam->SetArrayField(TEXT("aliases"), CallerAliases);
	InputNested->SetStringField(TEXT("caller_mutation"), TEXT("after registration"));

	TArray<FMonolithActionInfo> FirstRead = Registry.GetActions(Namespace);
	const FMonolithActionInfo* FirstInfo = FindAction(FirstRead, Namespace, TEXT("owned"));
	TestNotNull(TEXT("GetActions returns the registered action"), FirstInfo);
	if (FirstInfo)
	{
		TestEqual(TEXT("input schema mutation does not reach registry"),
			ReadAlias(*FirstInfo), FString(TEXT("original_alias")));
		const TSharedPtr<FJsonObject>* ReturnedParamPtr = nullptr;
		FirstInfo->ParamSchema->TryGetObjectField(TEXT("input"), ReturnedParamPtr);
		const TSharedPtr<FJsonObject>* ReturnedNestedPtr = nullptr;
		if (ReturnedParamPtr && ReturnedParamPtr->IsValid())
		{
			(*ReturnedParamPtr)->TryGetObjectField(TEXT("nested"), ReturnedNestedPtr);
		}
		TestTrue(TEXT("embedded-NUL schema key survives registration and GetActions clone"),
			ReturnedNestedPtr && ReturnedNestedPtr->IsValid() &&
			(*ReturnedNestedPtr)->HasField(FStringView(EmbeddedSchemaKey)));
		TArray<TSharedPtr<FJsonValue>> ReturnedAliases;
		ReturnedAliases.Add(MakeShared<FJsonValueString>(TEXT("get_actions_mutation")));
		if (ReturnedParamPtr && ReturnedParamPtr->IsValid())
		{
			(*ReturnedParamPtr)->SetArrayField(TEXT("aliases"), ReturnedAliases);
		}
	}

	TArray<FMonolithActionInfo> AllRead = Registry.GetAllActions();
	const FMonolithActionInfo* AllInfo = FindAction(AllRead, Namespace, TEXT("owned"));
	TestNotNull(TEXT("GetAllActions returns the registered action"), AllInfo);
	if (AllInfo)
	{
		const TSharedPtr<FJsonObject>* ReturnedParamPtr = nullptr;
		AllInfo->ParamSchema->TryGetObjectField(TEXT("input"), ReturnedParamPtr);
		const TSharedPtr<FJsonObject>* ReturnedNestedPtr = nullptr;
		if (ReturnedParamPtr && ReturnedParamPtr->IsValid())
		{
			(*ReturnedParamPtr)->TryGetObjectField(TEXT("nested"), ReturnedNestedPtr);
		}
		TestTrue(TEXT("embedded-NUL schema key survives GetAllActions clone"),
			ReturnedNestedPtr && ReturnedNestedPtr->IsValid() &&
			(*ReturnedNestedPtr)->HasField(FStringView(EmbeddedSchemaKey)));
		if (ReturnedParamPtr && ReturnedParamPtr->IsValid())
		{
			(*ReturnedParamPtr)->SetStringField(TEXT("get_all_actions_mutation"), TEXT("after return"));
		}
	}

	const TArray<FMonolithActionInfo> FinalRead = Registry.GetActions(Namespace);
	const FMonolithActionInfo* FinalInfo = FindAction(FinalRead, Namespace, TEXT("owned"));
	TestNotNull(TEXT("registry action remains after returned-schema mutations"), FinalInfo);
	if (FinalInfo)
	{
		TestEqual(TEXT("nested return-schema mutation does not reach registry"),
			ReadAlias(*FinalInfo), FString(TEXT("original_alias")));
		const TSharedPtr<FJsonObject>* Param = nullptr;
		FinalInfo->ParamSchema->TryGetObjectField(TEXT("input"), Param);
		if (Param && Param->IsValid())
		{
			TestFalse(TEXT("nested input mutation is not stored"), (*Param)->HasField(TEXT("caller_mutation")));
			TestFalse(TEXT("GetAllActions mutation is not stored"), (*Param)->HasField(TEXT("get_all_actions_mutation")));
			const TSharedPtr<FJsonObject>* Nested = nullptr;
			(*Param)->TryGetObjectField(TEXT("nested"), Nested);
			TestTrue(TEXT("registry retains embedded-NUL schema key after getter mutations"),
				Nested && Nested->IsValid() && (*Nested)->HasField(FStringView(EmbeddedSchemaKey)));
		}
	}
	Registry.UnregisterNamespace(Namespace);
	const FMonolithMetadataSnapshot Restored = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("schema ownership fixture cleanup restores baseline generation"),
		Baseline.MetadataGeneration, Restored.MetadataGeneration);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationRegistrySnapshotTest,
	"Monolith.Core.MetadataGeneration.RegistrySnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationRegistrySnapshotTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FMonolithMetadataSnapshot Baseline = Registry.GetMetadataSnapshot();
	const FString Namespace = MakeTemporaryNamespace();
	ON_SCOPE_EXIT
	{
		Registry.UnregisterNamespace(Namespace);
	};

	Registry.RegisterAction(Namespace, TEXT("replaceable"), TEXT("initial description"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("alias_a"), TEXT("one")), TEXT("initial category"));
	FMonolithMetadataSnapshot First = Registry.GetMetadataSnapshot();
	TestNotEqual(TEXT("registered action changes generation"), Baseline.MetadataGeneration, First.MetadataGeneration);

	Registry.RegisterAction(Namespace, TEXT("replaceable"), TEXT("initial description"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("alias_b"), TEXT("one")), TEXT("initial category"));
	FMonolithMetadataSnapshot SchemaChanged = Registry.GetMetadataSnapshot();
	TestNotEqual(TEXT("nested schema-only change invalidates generation"),
		First.MetadataGeneration, SchemaChanged.MetadataGeneration);

	Registry.RegisterAction(Namespace, TEXT("replaceable"), TEXT("replacement description"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("alias_b"), TEXT("one")), TEXT("replacement category"));
	FMonolithMetadataSnapshot Replaced = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("same-key replacement keeps action count"), SchemaChanged.TotalActions, Replaced.TotalActions);
	TestNotEqual(TEXT("same-count replacement changes generation"), SchemaChanged.MetadataGeneration, Replaced.MetadataGeneration);

	Registry.SetActionAnnotations(Namespace, TEXT("replaceable"), true, false, true, TEXT("Action title"));
	FMonolithMetadataSnapshot ActionAnnotated = Registry.GetMetadataSnapshot();
	TestNotEqual(TEXT("action annotations change generation"), Replaced.MetadataGeneration, ActionAnnotated.MetadataGeneration);
	const uint64 BeforeNoOpAnnotation = Registry.GetStructuralDigestComputationCountForTesting();
	Registry.SetActionAnnotations(Namespace, TEXT("replaceable"), true, false, true, TEXT("Action title"));
	const FMonolithMetadataSnapshot NoOpAnnotation = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("no-op annotation setter preserves generation"),
		ActionAnnotated.MetadataGeneration, NoOpAnnotation.MetadataGeneration);
	TestEqual(TEXT("no-op annotation setter may recompute the structural digest"),
		BeforeNoOpAnnotation + 1, Registry.GetStructuralDigestComputationCountForTesting());

	FMonolithDispatcherAnnotations Dispatcher;
	Dispatcher.bReadOnlyHint = true;
	Dispatcher.Title = TEXT("Dispatcher title");
	Registry.SetDispatcherAnnotations(Namespace, Dispatcher);
	FMonolithMetadataSnapshot DispatcherAnnotated = Registry.GetMetadataSnapshot();
	TestNotEqual(TEXT("active dispatcher annotations change generation"),
		NoOpAnnotation.MetadataGeneration, DispatcherAnnotated.MetadataGeneration);

	Registry.UnregisterNamespace(Namespace);
	FMonolithMetadataSnapshot Restored = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("namespace cleanup restores baseline generation despite retained dispatcher metadata"),
		Baseline.MetadataGeneration, Restored.MetadataGeneration);
	TestEqual(TEXT("namespace cleanup restores baseline action count"), Baseline.TotalActions, Restored.TotalActions);
	TestEqual(TEXT("namespace cleanup restores baseline namespace count"), Baseline.NamespaceCount, Restored.NamespaceCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationStatusAndSchemaIntegrationTest,
	"Monolith.Core.MetadataGeneration.StatusAndSchemaIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationStatusAndSchemaIntegrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FString BaselineGeneration;
	int32 BaselineActions = 0;
	int32 BaselineNamespaces = 0;
	const FMonolithActionResult BaselineStatus = ReadRegisteredStatus();
	TestTrue(TEXT("registered monolith.status returns baseline identity"),
		ReadStatusIdentity(BaselineStatus, BaselineGeneration, BaselineActions, BaselineNamespaces));
	if (!BaselineStatus.bSuccess || !BaselineStatus.Result.IsValid() || BaselineGeneration.IsEmpty())
	{
		return true;
	}
	AddInfo(FString::Printf(TEXT("baseline status: generation=%s actions=%d namespaces=%d"),
		*BaselineGeneration, BaselineActions, BaselineNamespaces));

	const FString Namespace = MakeTemporaryNamespace();
	ON_SCOPE_EXIT
	{
		Registry.UnregisterNamespace(Namespace);
	};
	Registry.RegisterAction(Namespace, TEXT("schema_fixture"), TEXT("temporary schema fixture"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("first_alias"), TEXT("first")));

	FString RegisteredGeneration;
	int32 RegisteredActions = 0;
	int32 RegisteredNamespaces = 0;
	const FMonolithActionResult RegisteredStatus = ReadRegisteredStatus();
	const FMonolithActionResult RegisteredSchema = ReadRegisteredSchema(Namespace, TEXT("schema_fixture"));
	TestTrue(TEXT("registered monolith.status reads fixture generation"),
		ReadStatusIdentity(RegisteredStatus, RegisteredGeneration, RegisteredActions, RegisteredNamespaces));
	const TArray<TSharedPtr<FJsonValue>>* UnavailableActions = nullptr;
	TestTrue(TEXT("registered status includes unavailable_actions array"),
		RegisteredStatus.Result.IsValid() &&
		RegisteredStatus.Result->TryGetArrayField(TEXT("unavailable_actions"), UnavailableActions) &&
		UnavailableActions != nullptr);
	TestTrue(TEXT("registered describe.action_schema reads fixture schema"), RegisteredSchema.bSuccess);
	TestEqual(TEXT("schema reader returns registered first alias"),
		ReadDescribedAlias(RegisteredSchema), FString(TEXT("first_alias")));
	TestEqual(TEXT("status action count includes one fixture action"), RegisteredActions, BaselineActions + 1);
	TestEqual(TEXT("status namespace count includes one fixture namespace"), RegisteredNamespaces, BaselineNamespaces + 1);
	TestNotEqual(TEXT("registered fixture changes live status generation"),
		BaselineGeneration, RegisteredGeneration);
	AddInfo(FString::Printf(TEXT("registered fixture status: generation=%s actions=%d namespaces=%d schema_alias=%s"),
		*RegisteredGeneration, RegisteredActions, RegisteredNamespaces, *ReadDescribedAlias(RegisteredSchema)));

	Registry.RegisterAction(Namespace, TEXT("schema_fixture"), TEXT("temporary schema fixture"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("changed_alias"), TEXT("first")));
	FString ChangedGeneration;
	int32 ChangedActions = 0;
	int32 ChangedNamespaces = 0;
	const FMonolithActionResult ChangedStatus = ReadRegisteredStatus();
	const FMonolithActionResult ChangedSchema = ReadRegisteredSchema(Namespace, TEXT("schema_fixture"));
	TestTrue(TEXT("registered status remains readable after same-count replacement"),
		ReadStatusIdentity(ChangedStatus, ChangedGeneration, ChangedActions, ChangedNamespaces));
	TestTrue(TEXT("registered schema reader remains readable after replacement"), ChangedSchema.bSuccess);
	TestEqual(TEXT("schema reader exposes changed replacement schema"),
		ReadDescribedAlias(ChangedSchema), FString(TEXT("changed_alias")));
	TestEqual(TEXT("schema-only replacement keeps action count"), RegisteredActions, ChangedActions);
	TestEqual(TEXT("schema-only replacement keeps namespace count"), RegisteredNamespaces, ChangedNamespaces);
	TestNotEqual(TEXT("schema-only replacement changes live status generation"),
		RegisteredGeneration, ChangedGeneration);
	AddInfo(FString::Printf(TEXT("schema replacement status: generation=%s actions=%d namespaces=%d schema_alias=%s"),
		*ChangedGeneration, ChangedActions, ChangedNamespaces, *ReadDescribedAlias(ChangedSchema)));

	Registry.UnregisterNamespace(Namespace);
	FString RestoredGeneration;
	int32 RestoredActions = 0;
	int32 RestoredNamespaces = 0;
	const FMonolithActionResult RestoredStatus = ReadRegisteredStatus();
	TestTrue(TEXT("registered monolith.status reads restored baseline"),
		ReadStatusIdentity(RestoredStatus, RestoredGeneration, RestoredActions, RestoredNamespaces));
	TestEqual(TEXT("cleanup restores baseline generation through status reader"),
		BaselineGeneration, RestoredGeneration);
	TestEqual(TEXT("cleanup restores baseline action count through status reader"),
		BaselineActions, RestoredActions);
	TestEqual(TEXT("cleanup restores baseline namespace count through status reader"),
		BaselineNamespaces, RestoredNamespaces);
	AddInfo(FString::Printf(TEXT("restored status: generation=%s actions=%d namespaces=%d"),
		*RestoredGeneration, RestoredActions, RestoredNamespaces));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationHandlerAvailabilityTest,
	"Monolith.Core.MetadataGeneration.HandlerAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationHandlerAvailabilityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FMonolithMetadataSnapshot Baseline = Registry.GetMetadataSnapshot();
	const FString Namespace = MakeTemporaryNamespace();
	ON_SCOPE_EXIT
	{
		Registry.UnregisterNamespace(Namespace);
	};

	TSharedPtr<int32> InvocationCount = MakeShared<int32>(0);
	FMonolithActionHandler BoundWithoutInvocation = FMonolithActionHandler::CreateLambda(
		[InvocationCount](const TSharedPtr<FJsonObject>& /*Params*/)
		{
			++*InvocationCount;
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		});
	Registry.RegisterAction(Namespace, TEXT("bound"), TEXT("bound handler"), BoundWithoutInvocation);
	Registry.RegisterAction(Namespace, TEXT("unbound"), TEXT("unbound handler"), FMonolithActionHandler{});

	TSharedPtr<int32> WeakInvocationCount = MakeShared<int32>(0);
	TSharedPtr<FWeakTarget> WeakTarget = MakeShared<FWeakTarget>(WeakInvocationCount);
	FMonolithActionHandler ExpiringHandler = FMonolithActionHandler::CreateSP(
		WeakTarget.ToSharedRef(), &FWeakTarget::Invoke);
	Registry.RegisterAction(Namespace, TEXT("expired"), TEXT("expired weak target"), ExpiringHandler);
	const uint64 BindingSamplesBeforeLiveSnapshot = Registry.GetHandlerBindingSampleCountForTesting();
	const FMonolithMetadataSnapshot WeakTargetAlive = Registry.GetMetadataSnapshot();
	const uint64 BindingSamplesAfterLiveSnapshot = Registry.GetHandlerBindingSampleCountForTesting();
	TestFalse(TEXT("live weak target remains available"),
		WeakTargetAlive.UnavailableActions.Contains(Namespace + TEXT(".expired")));
	TestEqual(TEXT("snapshot samples every registered handler"),
		BindingSamplesAfterLiveSnapshot - BindingSamplesBeforeLiveSnapshot,
		static_cast<uint64>(WeakTargetAlive.TotalActions));
	WeakTarget.Reset();

	const uint64 BindingSamplesBeforeExpiredSnapshot = Registry.GetHandlerBindingSampleCountForTesting();
	const FMonolithMetadataSnapshot Snapshot = Registry.GetMetadataSnapshot();
	const uint64 BindingSamplesAfterExpiredSnapshot = Registry.GetHandlerBindingSampleCountForTesting();
	TestTrue(TEXT("unbound delegate is unavailable"),
		Snapshot.UnavailableActions.Contains(Namespace + TEXT(".unbound")));
	TestTrue(TEXT("expired weak delegate is unavailable"),
		Snapshot.UnavailableActions.Contains(Namespace + TEXT(".expired")));
	TestFalse(TEXT("bound delegate is not unavailable"),
		Snapshot.UnavailableActions.Contains(Namespace + TEXT(".bound")));
	TestNotEqual(TEXT("weak target expiry changes generation without a registry setter"),
		WeakTargetAlive.MetadataGeneration, Snapshot.MetadataGeneration);
	TestEqual(TEXT("snapshot does not execute a bound handler"), *InvocationCount, 0);
	TestEqual(TEXT("snapshot does not execute an expired weak handler"), *WeakInvocationCount, 0);
	TestEqual(TEXT("snapshot resamples all handlers after weak expiry"),
		BindingSamplesAfterExpiredSnapshot - BindingSamplesBeforeExpiredSnapshot,
		static_cast<uint64>(Snapshot.TotalActions));
	AddInfo(FString::Printf(TEXT("handler availability observation: actions sampled per snapshot=%d; expired weak action=%s"),
		Snapshot.TotalActions, *FString::Printf(TEXT("%s.expired"), *Namespace)));
	bool bUnavailableActionsSorted = true;
	for (int32 Index = 1; Index < Snapshot.UnavailableActions.Num(); ++Index)
	{
		bUnavailableActionsSorted &= Snapshot.UnavailableActions[Index - 1].Compare(
			Snapshot.UnavailableActions[Index], ESearchCase::CaseSensitive) <= 0;
	}
	TestTrue(TEXT("unavailable actions are sorted"), bUnavailableActionsSorted);

	Registry.UnregisterNamespace(Namespace);
	const FMonolithMetadataSnapshot Restored = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("handler fixture cleanup restores baseline generation"),
		Baseline.MetadataGeneration, Restored.MetadataGeneration);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMetadataGenerationFailureRecoveryTest,
	"Monolith.Core.MetadataGeneration.FailureRecoveryAndMemoization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMetadataGenerationFailureRecoveryTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMetadataGenerationTestDetail;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FMonolithMetadataSnapshot Baseline = Registry.GetMetadataSnapshot();
	const FString Namespace = MakeTemporaryNamespace();
	ON_SCOPE_EXIT
	{
		Registry.UnregisterNamespace(Namespace);
	};

	TSharedPtr<FJsonObject> InvalidSchema = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonValue> InvalidChild;
	InvalidSchema->SetField(TEXT("invalid_child"), InvalidChild);
	Registry.RegisterAction(Namespace, TEXT("recoverable"), TEXT("invalid then valid"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), InvalidSchema);
	const FMonolithMetadataSnapshot InvalidJson = Registry.GetMetadataSnapshot();
	TestTrue(TEXT("invalid JSON has no usable generation"), InvalidJson.MetadataGeneration.IsEmpty());
	TestTrue(TEXT("invalid JSON reports a diagnostic instead of dropping the field"),
		InvalidJson.MetadataGenerationError.Contains(TEXT("null value pointer")));
	FMonolithActionResult InvalidStatus = ReadRegisteredStatus();
	FString InvalidStatusGeneration;
	FString InvalidStatusError;
	TestTrue(TEXT("status preserves empty generation and metadata diagnostic"),
		InvalidStatus.bSuccess && InvalidStatus.Result.IsValid() &&
		InvalidStatus.Result->TryGetStringField(TEXT("metadata_generation"), InvalidStatusGeneration) &&
		InvalidStatus.Result->TryGetStringField(TEXT("metadata_generation_error"), InvalidStatusError) &&
		InvalidStatusGeneration.IsEmpty() && !InvalidStatusError.IsEmpty());

	TSharedPtr<FJsonObject> NonFiniteSchema = MakeShared<FJsonObject>();
	NonFiniteSchema->SetNumberField(TEXT("invalid_number"), std::numeric_limits<double>::quiet_NaN());
	Registry.RegisterAction(Namespace, TEXT("recoverable"), TEXT("invalid then valid"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), NonFiniteSchema);
	const FMonolithMetadataSnapshot NonFinite = Registry.GetMetadataSnapshot();
	TestTrue(TEXT("non-finite metadata has no usable generation"), NonFinite.MetadataGeneration.IsEmpty());
	TestTrue(TEXT("non-finite metadata reports a diagnostic"),
		NonFinite.MetadataGenerationError.Contains(TEXT("non-finite number")));

	Registry.RegisterAction(Namespace, TEXT("recoverable"), TEXT("recovered metadata"),
		FMonolithActionHandler::CreateStatic(&NoOpHandler), MakeSchema(TEXT("valid_alias"), TEXT("one")));
	const FMonolithMetadataSnapshot Recovered = Registry.GetMetadataSnapshot();
	TestTrue(TEXT("valid replacement recovers generation"),
		Recovered.MetadataGeneration.StartsWith(TEXT("v1:")) && Recovered.MetadataGeneration.Len() == 43);
	TestTrue(TEXT("recovered metadata clears the diagnostic"), Recovered.MetadataGenerationError.IsEmpty());
	bool bLowerHex = true;
	for (int32 Index = 3; Index < Recovered.MetadataGeneration.Len(); ++Index)
	{
		const TCHAR Ch = Recovered.MetadataGeneration[Index];
		bLowerHex &= (Ch >= TEXT('0') && Ch <= TEXT('9')) || (Ch >= TEXT('a') && Ch <= TEXT('f'));
	}
	TestTrue(TEXT("generation suffix is lowercase hexadecimal"), bLowerHex);

	const uint64 AfterRecovery = Registry.GetStructuralDigestComputationCountForTesting();
	const uint64 BindingBeforeUnchanged = Registry.GetHandlerBindingSampleCountForTesting();
	const FMonolithMetadataSnapshot Unchanged = Registry.GetMetadataSnapshot();
	const uint64 BindingAfterUnchanged = Registry.GetHandlerBindingSampleCountForTesting();
	TestEqual(TEXT("unchanged metadata reuses the same generation"),
		Recovered.MetadataGeneration, Unchanged.MetadataGeneration);
	TestEqual(TEXT("unchanged snapshot does not re-encode schemas"),
		AfterRecovery, Registry.GetStructuralDigestComputationCountForTesting());
	TestEqual(TEXT("unchanged snapshot still samples every registered handler"),
		BindingAfterUnchanged - BindingBeforeUnchanged, static_cast<uint64>(Unchanged.TotalActions));
	AddInfo(FString::Printf(TEXT("snapshot cost observation: structural attempts=%llu; unchanged handler samples=%llu; actions=%d"),
		static_cast<unsigned long long>(AfterRecovery),
		static_cast<unsigned long long>(BindingAfterUnchanged - BindingBeforeUnchanged),
		Unchanged.TotalActions));

	Registry.SetActionAnnotations(Namespace, TEXT("recoverable"), false, false, false, TEXT("memo invalidation"));
	Registry.GetMetadataSnapshot();
	TestEqual(TEXT("metadata mutation causes one structural recomputation"),
		AfterRecovery + 1, Registry.GetStructuralDigestComputationCountForTesting());

	Registry.UnregisterNamespace(Namespace);
	const FMonolithMetadataSnapshot Restored = Registry.GetMetadataSnapshot();
	TestEqual(TEXT("recovery fixture cleanup restores baseline generation"),
		Baseline.MetadataGeneration, Restored.MetadataGeneration);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
