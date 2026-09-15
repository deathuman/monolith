// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "MonolithEditorSlateActions.h"
#include "MonolithSettings.h"
#include "MonolithUIRecovery.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorAccessibilityStartupTest,
	"Monolith.Editor.Accessibility.StartupBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorAccessibilityStartupTest::RunTest(const FString& /*Parameters*/)
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);

	IConsoleVariable* AccessibilityCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Accessibility.Enable"));
	TestNotNull(TEXT("Accessibility.Enable exists"), AccessibilityCVar);

	if (Settings && Settings->bEnableEditorAccessibilityBridge && AccessibilityCVar)
	{
		TestEqual(
			TEXT("Opted-in editor startup leaves Unreal native accessibility enabled"),
			AccessibilityCVar->GetInt(),
			1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateSnapshotBoundedTest,
	"Monolith.Editor.Accessibility.SlateSnapshotBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateSnapshotBoundedTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("max_controls"), 10);
	Params->SetNumberField(TEXT("max_depth"), 4);

	const FMonolithActionResult Snapshot = FMonolithEditorSlateActions::HandleGetSlateSnapshot(Params);
	TestTrue(TEXT("Slate snapshot succeeds in editor context"), Snapshot.bSuccess);
	TestTrue(TEXT("Slate snapshot returns a JSON result"), Snapshot.Result.IsValid());
	if (!Snapshot.bSuccess || !Snapshot.Result.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Windows = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
	TestTrue(TEXT("Snapshot exposes windows array"), Snapshot.Result->TryGetArrayField(TEXT("windows"), Windows));
	TestTrue(TEXT("Snapshot exposes controls array"), Snapshot.Result->TryGetArrayField(TEXT("controls"), Controls));

	double ReturnedControls = -1.0;
	TestTrue(TEXT("Snapshot reports returned_controls"), Snapshot.Result->TryGetNumberField(TEXT("returned_controls"), ReturnedControls));
	TestTrue(TEXT("Snapshot honors max_controls bound"), ReturnedControls >= 0.0 && ReturnedControls <= 10.0);
	TestTrue(TEXT("Snapshot reports truncation state"), Snapshot.Result->HasField(TEXT("truncated")));
	TestTrue(TEXT("Snapshot reports focus state"), Snapshot.Result->HasField(TEXT("focused_control_index")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateButtonUniqueGuardTest,
	"Monolith.Editor.Accessibility.SlateButtonUniqueGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateButtonUniqueGuardTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("text"), TEXT("__MONOLITH_NON_EXISTENT_BUTTON__"));

	const FMonolithActionResult Click = FMonolithEditorSlateActions::HandleClickSlateButton(Params);
	TestFalse(TEXT("Unknown button is rejected without mutation"), Click.bSuccess);
	TestTrue(TEXT("Unknown-button error explains unique-match requirement"), Click.ErrorMessage.Contains(TEXT("Expected one visible enabled SButton")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateDismissMenusIdempotentTest,
	"Monolith.Editor.Accessibility.SlateDismissMenusIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateDismissMenusIdempotentTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithActionResult Dismiss = FMonolithEditorSlateActions::HandleDismissSlateMenus(nullptr);
	TestTrue(TEXT("Dismiss-menus action succeeds with or without an open menu"), Dismiss.bSuccess);
	TestTrue(TEXT("Dismiss-menus action returns JSON"), Dismiss.Result.IsValid());
	if (Dismiss.Result.IsValid())
	{
		bool bVisibleAfter = true;
		TestTrue(TEXT("Dismiss-menus reports post state"), Dismiss.Result->TryGetBoolField(TEXT("visible_menu_after"), bVisibleAfter));
		TestFalse(TEXT("No Slate menu remains after dismissal"), bVisibleAfter);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateObservationGuardTest,
	"Monolith.Editor.Accessibility.SlateObservationGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateObservationGuardTest::RunTest(const FString& /*Parameters*/)
{
	const auto First = FMonolithEditorSlateActions::HandleGetSlateSnapshot(nullptr);
	if (!TestTrue(TEXT("Initial snapshot succeeds"), First.bSuccess)) return false;
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("snapshot_id"), First.Result->GetStringField(TEXT("snapshot_id")));
	Params->SetNumberField(TEXT("control_index"), 0);
	Params->SetNumberField(TEXT("window_index"), 0);
	Params->SetStringField(TEXT("operation"), TEXT("click"));
	FMonolithEditorSlateActions::HandleGetSlateSnapshot(nullptr);
	TestFalse(TEXT("New observation invalidates old control identity"), FMonolithEditorSlateActions::HandleInteractSlate(Params).bSuccess);
	TestFalse(TEXT("New observation invalidates old window identity"), FMonolithEditorSlateActions::HandleCaptureSlateWindow(Params).bSuccess);
	const auto Fresh = FMonolithEditorSlateActions::HandleGetSlateSnapshot(nullptr);
	Params->SetStringField(TEXT("snapshot_id"), Fresh.Result->GetStringField(TEXT("snapshot_id")));
	Params->SetNumberField(TEXT("control_index"), -1);
	TestFalse(TEXT("Negative index is rejected"), FMonolithEditorSlateActions::HandleInteractSlate(Params).bSuccess);
	Params->SetNumberField(TEXT("control_index"), 0.5);
	TestFalse(TEXT("Fractional index is rejected"), FMonolithEditorSlateActions::HandleInteractSlate(Params).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateDisabledDetachedTest,
	"Monolith.Editor.Accessibility.DisabledDetachedTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateDisabledDetachedTest::RunTest(const FString& /*Parameters*/)
{
	int32 Clicks = 0;
	TSharedPtr<SButton> First;
	const auto Window = SNew(SWindow).Title(FText::FromString(TEXT("Monolith guard fixture"))).ClientSize(FVector2D(400, 180))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[SAssignNew(First, SButton).Text(FText::FromString(TEXT("__MonolithGuard__"))).OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })]
		+ SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("__MonolithGuard__")))]
	];
	auto& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window);
	Slate.Tick();
	auto Search = MakeShared<FJsonObject>();
	Search->SetStringField(TEXT("text"), TEXT("__MonolithGuard__"));
	Search->SetStringField(TEXT("filter"), TEXT("__MonolithGuard__"));
	Search->SetNumberField(TEXT("max_depth"), 64);
	const auto Duplicate = FMonolithEditorSlateActions::HandleClickSlateButton(Search);
	TestFalse(TEXT("Duplicate labels fail closed"), Duplicate.bSuccess);
	const auto Snapshot = FMonolithEditorSlateActions::HandleGetSlateSnapshot(Search);
	const auto& Controls = Snapshot.Result->GetArrayField(TEXT("controls"));
	if (TestTrue(TEXT("Fixture controls observed"), Controls.Num() >= 2))
	{
		auto Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("snapshot_id"), Snapshot.Result->GetStringField(TEXT("snapshot_id")));
		Target->SetNumberField(TEXT("control_index"), 0);
		Target->SetStringField(TEXT("operation"), TEXT("click"));
		First->SetEnabled(false);
		TestFalse(TEXT("Disabled after observation rejects without dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
		First->SetEnabled(true);
		Window->SetEnabled(false);
		TestFalse(TEXT("Disabled owning window rejects dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
		Window->SetEnabled(true);
		Window->SetVisibility(EVisibility::HitTestInvisible);
		TestFalse(TEXT("Non-hit-testable owning window rejects dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
		Window->SetVisibility(EVisibility::Visible);
		First->SetVisibility(EVisibility::HitTestInvisible);
		TestFalse(TEXT("Hit-test-invisible target rejects without dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
		First->SetVisibility(EVisibility::SelfHitTestInvisible);
		TestFalse(TEXT("Self-hit-test-invisible target rejects without dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
		First->SetVisibility(EVisibility::Visible);
		Window->SetContent(SNullWidget::NullWidget);
		TestFalse(TEXT("Detached target rejects without dispatch"), FMonolithEditorSlateActions::HandleInteractSlate(Target).bSuccess);
	}
	TestEqual(TEXT("No fixture action fired"), Clicks, 0);
	Slate.RequestDestroyWindow(Window);
	Slate.Tick();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorLegacyClickHitTestGuard,
	"Monolith.Editor.Accessibility.LegacyClickHitTestGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorLegacyClickHitTestGuard::RunTest(const FString& /*Parameters*/)
{
	int32 Clicks = 0;
	TSharedPtr<SButton> Button;
	const auto Window = SNew(SWindow).Title(FText::FromString(TEXT("Monolith legacy guard fixture"))).ClientSize(FVector2D(400, 180))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[SAssignNew(Button, SButton).Text(FText::FromString(TEXT("__MonolithLegacyGuard__"))).OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })]
	];
	auto& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window);
	Slate.Tick();
	auto Search = MakeShared<FJsonObject>();
	Search->SetStringField(TEXT("text"), TEXT("__MonolithLegacyGuard__"));
	Search->SetStringField(TEXT("window_title"), TEXT("Monolith legacy guard fixture"));
	for (const EVisibility Visibility : {EVisibility::HitTestInvisible, EVisibility::SelfHitTestInvisible})
	{
		Button->SetVisibility(Visibility);
		TestFalse(TEXT("Legacy click rejects a non-hit-testable target"), FMonolithEditorSlateActions::HandleClickSlateButton(Search).bSuccess);
	}
	Button->SetVisibility(EVisibility::Visible);
	Window->SetEnabled(false);
	TestFalse(TEXT("Legacy click rejects disabled owning window"), FMonolithEditorSlateActions::HandleClickSlateButton(Search).bSuccess);
	Window->SetEnabled(true);
	Window->SetVisibility(EVisibility::HitTestInvisible);
	TestFalse(TEXT("Legacy click rejects non-hit-testable owning window"), FMonolithEditorSlateActions::HandleClickSlateButton(Search).bSuccess);
	Window->SetVisibility(EVisibility::Visible);
	Window->GetContent()->SetVisibility(EVisibility::HitTestInvisible);
	TestFalse(TEXT("Legacy click rejects a blocked ancestor"), FMonolithEditorSlateActions::HandleClickSlateButton(Search).bSuccess);
	TestEqual(TEXT("Rejected targets fired no action"), Clicks, 0);
	Window->GetContent()->SetVisibility(EVisibility::SelfHitTestInvisible);
	TestTrue(TEXT("Hit-testable child remains usable under self-invisible parent"), FMonolithEditorSlateActions::HandleClickSlateButton(Search).bSuccess);
	TestEqual(TEXT("One valid legacy click fired"), Clicks, 1);
	Slate.RequestDestroyWindow(Window);
	Slate.Tick();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorSlateRecoveryGateTest,
	"Monolith.Editor.Accessibility.RecoveryDispatchGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSlateRecoveryGateTest::RunTest(const FString& /*Parameters*/)
{
	const bool bPrevious = SetMonolithUIRecoveryOnly(true);
	const auto Blocked = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("run_python"), MakeShared<FJsonObject>());
	TestFalse(TEXT("General scripting unavailable during modal recovery"), Blocked.bSuccess);
	TestTrue(TEXT("Rejection names recovery restriction"), Blocked.ErrorMessage.Contains(TEXT("Modal recovery")));
	const auto Snapshot = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("get_slate_snapshot"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Recovery observation remains available"), Snapshot.bSuccess);
	SetMonolithUIRecoveryOnly(bPrevious);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
