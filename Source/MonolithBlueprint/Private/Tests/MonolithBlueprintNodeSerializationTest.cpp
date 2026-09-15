// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithBlueprintNodeSerializationTest.cpp
//
// Contract test for the shared K2 node serializer identity fields
// (MonolithBlueprintInternal::SerializeNode).
//
// WHY IT EXISTS:
//   Downstream semantic consumers key nodes by NodeGuid and resolve members by
//   qualified owner class paths. Before this contract the serializer emitted
//   only transient UObject names (K2Node_CallFunction_0) and short class names
//   (BPC_Foo_C), forcing consumers to parse titles or chase objects.
//
// WHAT IT LOCKS (all additive; legacy fields must remain untouched):
//   1. Every serialized node carries node_guid from NodeGuid.ToString(). An
//      unassigned (invalid) NodeGuid serializes as the all-zero GUID string —
//      never omitted, never fabricated.
//   2. External function calls emit function_class_path (qualified /Script/
//      path) next to the legacy short function_class, plus is_self_context.
//   3. Self-variable references report variable_name + is_self_context=true;
//      local-variable scopes surface member_scope.
//   4. CreateEvent (UK2Node_CreateDelegate) exposes the bound function name,
//      its scope class path, and self-context derived from the object pin.
//   5. Repeated serialization of the same node produces byte-identical JSON —
//      the stable-identity precondition for downstream diffing.
//
// The disposable fixture Blueprint lives under /Game/Tests/Monolith/ and is
// trashed at the end. No project or engine asset dependency.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

#include "MonolithBlueprintInternal.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CreateDelegate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace MonolithBlueprintNodeSerializationTest
{
	static const TCHAR* const TestBpPath = TEXT("/Game/Tests/Monolith/BP_NodeIdentityTest");

	static FString JsonToString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		if (!Obj.IsValid())
		{
			return Out;
		}
		const auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintNodeIdentitySerializationTest,
	"Monolith.Blueprint.NodeSerialization.IdentityFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintNodeIdentitySerializationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithBlueprintNodeSerializationTest;

	if (!GEditor)
	{
		AddInfo(TEXT("Skipped — no GEditor (non-editor context)"));
		return true;
	}

	// --- 1) Disposable Blueprint fixture ---------------------------------------
	// Cleanup is declared before the package so EVERY failure path below it can
	// trash both objects, including a failed CreatePackage.
	UPackage* Pkg = nullptr;
	UBlueprint* BP = nullptr;
	auto Cleanup = [&Pkg, &BP]()
	{
		if (BP)
		{
			BP->ClearFlags(RF_Standalone | RF_Public);
			BP->RemoveFromRoot();
			BP->MarkAsGarbage();
		}
		if (Pkg)
		{
			Pkg->SetDirtyFlag(false);
			Pkg->ClearFlags(RF_Standalone);
			Pkg->RemoveFromRoot();
			Pkg->MarkAsGarbage();
		}
	};

	Pkg = CreatePackage(TestBpPath);
	if (!TestNotNull(TEXT("Test package created"), Pkg))
	{
		Cleanup();
		return false;
	}

	FString AssetName;
	int32 LastSlash = INDEX_NONE;
	FString(TestBpPath).FindLastChar('/', LastSlash);
	AssetName = FString(TestBpPath).Mid(LastSlash + 1);

	BP = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(), Pkg, FName(*AssetName),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), NAME_None);
	if (!TestNotNull(TEXT("Fixture Blueprint created"), BP))
	{
		Cleanup();
		return false;
	}

	UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("Event graph found"), EventGraph))
	{
		Cleanup();
		return false;
	}

	FEdGraphPinType IntVarType;
	IntVarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	FBlueprintEditorUtils::AddMemberVariable(BP, FName(TEXT("TestVar")), IntVarType);

	// --- 2) External native function call --------------------------------------
	UK2Node_CallFunction* CallNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*EventGraph);
		CallNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("CallFunction node spawned"), CallNode))
	{
		Cleanup();
		return false;
	}
	CallNode->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
	CallNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> CallObj = MonolithBlueprintInternal::SerializeNode(CallNode);
	TestTrue(TEXT("Call node carries node_guid"), !CallObj->GetStringField(TEXT("node_guid")).IsEmpty());
	TestEqual(TEXT("Call node id stays the transient name"),
		CallObj->GetStringField(TEXT("id")), CallNode->GetName());
	TestEqual(TEXT("Legacy function field preserved"),
		CallObj->GetStringField(TEXT("function")), FString(TEXT("PrintString")));
	TestEqual(TEXT("Legacy short function_class preserved"),
		CallObj->GetStringField(TEXT("function_class")), FString(TEXT("KismetSystemLibrary")));
	TestEqual(TEXT("Qualified function_class_path"),
		CallObj->GetStringField(TEXT("function_class_path")), FString(TEXT("/Script/Engine.KismetSystemLibrary")));
	TestFalse(TEXT("External call is not self-context"),
		CallObj->GetBoolField(TEXT("is_self_context")));
	TestFalse(TEXT("External call has no member_scope"), CallObj->HasField(TEXT("member_scope")));
	TestTrue(TEXT("Legacy pins array still present"), CallObj->HasField(TEXT("pins")));

	// --- 3) Determinism ---------------------------------------------------------
	TestEqual(TEXT("Repeated serialization is byte-identical"),
		JsonToString(MonolithBlueprintInternal::SerializeNode(CallNode)), JsonToString(CallObj));

	// --- 4) Self variable reference --------------------------------------------
	UK2Node_VariableGet* VarGetNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*EventGraph);
		VarGetNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("VariableGet node spawned"), VarGetNode))
	{
		Cleanup();
		return false;
	}
	VarGetNode->VariableReference.SetSelfMember(FName(TEXT("TestVar")));
	VarGetNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> VarObj = MonolithBlueprintInternal::SerializeNode(VarGetNode);
	TestEqual(TEXT("variable_name emitted"),
		VarObj->GetStringField(TEXT("variable_name")), FString(TEXT("TestVar")));
	TestTrue(TEXT("Self variable is self-context"),
		VarObj->GetBoolField(TEXT("is_self_context")));
	TestFalse(TEXT("Self variable has no variable_class_path"),
		VarObj->HasField(TEXT("variable_class_path")));

	// --- 5) Local-scoped variable reference ------------------------------------
	UK2Node_VariableGet* LocalGetNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*EventGraph);
		LocalGetNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("Local VariableGet node spawned"), LocalGetNode))
	{
		Cleanup();
		return false;
	}
	LocalGetNode->AllocateDefaultPins();
	LocalGetNode->VariableReference.SetLocalMember(
		FName(TEXT("MonolithLocalFixture")), EventGraph->GetName(), FGuid::NewGuid());

	TSharedPtr<FJsonObject> LocalObj = MonolithBlueprintInternal::SerializeNode(LocalGetNode);
	TestTrue(TEXT("Local reference is not self-context"),
		!LocalObj->GetBoolField(TEXT("is_self_context")));
	TestEqual(TEXT("member_scope carries the local scope"),
		LocalObj->GetStringField(TEXT("member_scope")), EventGraph->GetName());

	// --- 6) Event dispatcher node (UK2Node_BaseMCDelegate) ---------------------
	// Synthetic transient reference: the serializer contract under test is what
	// the branch emits for a given FMemberReference, not dispatcher authoring.
	UK2Node_AddDelegate* McDelNode = NewObject<UK2Node_AddDelegate>(GetTransientPackage(), NAME_None, RF_Transient);
	McDelNode->DelegateReference.SetExternalMember(
		FName(TEXT("OnFixture__DelegateSignature")), AActor::StaticClass());

	TSharedPtr<FJsonObject> McDelObj = MonolithBlueprintInternal::SerializeNode(McDelNode);
	TestEqual(TEXT("Dispatcher property name preserved"),
		McDelObj->GetStringField(TEXT("delegate_property_name")), FString(TEXT("OnFixture__DelegateSignature")));
	TestFalse(TEXT("Legacy self_context bool still emitted and false"),
		McDelObj->GetBoolField(TEXT("self_context")));
	TestFalse(TEXT("Uniform is_self_context false for external dispatcher"),
		McDelObj->GetBoolField(TEXT("is_self_context")));
	TestEqual(TEXT("Qualified delegate_owner_class_path"),
		McDelObj->GetStringField(TEXT("delegate_owner_class_path")), FString(TEXT("/Script/Engine.Actor")));
	McDelNode->MarkAsGarbage();

	// --- 7) CreateEvent (UK2Node_CreateDelegate) -------------------------------
	UK2Node_CreateDelegate* CreateDelNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_CreateDelegate> Creator(*EventGraph);
		CreateDelNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("CreateDelegate node spawned"), CreateDelNode))
	{
		Cleanup();
		return false;
	}
	CreateDelNode->SetFunction(FName(TEXT("MonolithFixtureHandler")));
	CreateDelNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> DelObj = MonolithBlueprintInternal::SerializeNode(CreateDelNode);
	TestEqual(TEXT("CreateDelegate bound function emitted"),
		DelObj->GetStringField(TEXT("function")), FString(TEXT("MonolithFixtureHandler")));
	TestTrue(TEXT("Unlinked object pin means self-context"),
		DelObj->GetBoolField(TEXT("is_self_context")));
	TestEqual(TEXT("Self-scope class path is this Blueprint's generated class"),
		DelObj->GetStringField(TEXT("function_class_path")),
		BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());

	// Linked-to-Self variant: still self-context, same scope path.
	UK2Node_Self* SelfNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_Self> Creator(*EventGraph);
		SelfNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("Self node spawned"), SelfNode))
	{
		Cleanup();
		return false;
	}
	SelfNode->AllocateDefaultPins();
	if (UEdGraphPin* ObjectPin = CreateDelNode->GetObjectInPin())
	{
		if (UEdGraphPin* SelfPin = SelfNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Output))
		{
			ObjectPin->MakeLinkTo(SelfPin);
		}
	}
	TSharedPtr<FJsonObject> DelLinkedObj = MonolithBlueprintInternal::SerializeNode(CreateDelNode);
	TestTrue(TEXT("Self-linked object pin stays self-context"),
		DelLinkedObj->GetBoolField(TEXT("is_self_context")));
	TestEqual(TEXT("Self-linked scope class path unchanged"),
		DelLinkedObj->GetStringField(TEXT("function_class_path")),
		BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());

	// --- 8) Cast node target identity ------------------------------------------
	UK2Node_DynamicCast* CastNode = nullptr;
	{
		FGraphNodeCreator<UK2Node_DynamicCast> Creator(*EventGraph);
		CastNode = Creator.CreateNode(/*bSelectNewNode=*/false);
		Creator.Finalize();
	}
	if (!TestNotNull(TEXT("DynamicCast node spawned"), CastNode))
	{
		Cleanup();
		return false;
	}
	CastNode->TargetType = AActor::StaticClass();
	CastNode->AllocateDefaultPins();

	TSharedPtr<FJsonObject> CastObj = MonolithBlueprintInternal::SerializeNode(CastNode);
	TestEqual(TEXT("cast_class_path emitted"),
		CastObj->GetStringField(TEXT("cast_class_path")), FString(TEXT("/Script/Engine.Actor")));

	// --- 9) Invalid (unassigned) NodeGuid --------------------------------------
	UEdGraphNode* BareNode = NewObject<UEdGraphNode>(GetTransientPackage(), NAME_None, RF_Transient);
	const FGuid InvalidGuid = BareNode->NodeGuid;
	TestFalse(TEXT("Sanity: fixture node guid is genuinely invalid"), InvalidGuid.IsValid());
	TSharedPtr<FJsonObject> BareObj = MonolithBlueprintInternal::SerializeNode(BareNode);
	TestEqual(TEXT("Invalid NodeGuid serializes as the all-zero GUID string"),
		BareObj->GetStringField(TEXT("node_guid")), InvalidGuid.ToString());
	BareNode->MarkAsGarbage();

	// --- 10) Cleanup --------------------------------------------------------------
	Cleanup();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
