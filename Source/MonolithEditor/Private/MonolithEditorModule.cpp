#include "MonolithEditorModule.h"
#include "MonolithEditorActions.h"
#include "MonolithEditorMapActions.h"
#include "MonolithEditorSlateActions.h"
#include "MonolithPieObjectActions.h"
#include "MonolithPieInputActions.h"
#include "MonolithPieTimeseries.h"
#include "MonolithStatActions.h"
#include "MonolithSettingsCustomization.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "PropertyEditorModule.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/IConsoleManager.h"

// PART C — passive modal watcher.
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Layout/Children.h"

#define LOCTEXT_NAMESPACE "FMonolithEditorModule"

namespace
{
	void EnableNativeEditorAccessibilityBridge(const UMonolithSettings& Settings)
	{
		if (!Settings.bEnableEditorAccessibilityBridge)
		{
			return;
		}

		IConsoleVariable* AccessibilityCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Accessibility.Enable"));
		if (!AccessibilityCVar)
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith — native editor accessibility requested, but Accessibility.Enable is unavailable."));
			return;
		}

		// Project-setting priority makes the Monolith opt-in deterministic while
		// still allowing command-line/code/console overrides to take precedence.
		if (AccessibilityCVar->GetInt() == 0)
		{
			AccessibilityCVar->Set(1, ECVF_SetByProjectSetting);
		}

		if (AccessibilityCVar->GetInt() != 0)
		{
			UE_LOG(LogMonolith, Display,
				TEXT("Monolith — native editor accessibility bridge active (Accessibility.Enable=1)."));
		}
		else
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith — native editor accessibility was requested, but a higher-priority Accessibility.Enable override kept it disabled."));
		}
	}

	// Recursively walk a Slate widget subtree, appending the text of every STextBlock
	// found to OutText (newline-joined). Best-effort and depth-bounded so a pathological
	// tree can't stall the first modal-loop tick.
	void HarvestTextBlocks(const TSharedPtr<SWidget>& Widget, FString& OutText, int32 Depth)
	{
		if (!Widget.IsValid() || Depth > 12)
		{
			return;
		}
		// STextBlock is a SLeafWidget — identify by widget type name (no RTTI dependency).
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			const FText Text = StaticCastSharedPtr<STextBlock>(Widget)->GetText();
			if (!Text.IsEmpty())
			{
				if (!OutText.IsEmpty()) { OutText.Append(TEXT(" | ")); }
				OutText.Append(Text.ToString());
			}
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			const int32 Num = Children->Num();
			for (int32 Index = 0; Index < Num; ++Index)
			{
				HarvestTextBlocks(Children->GetChildAt(Index), OutText, Depth + 1);
			}
		}
	}
}

void FMonolithEditorModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings->bEnableEditor) return;

	// Enable the engine's existing Slate -> platform accessibility provider before
	// normal Monolith editor registration. External UIA/assistive clients can query
	// the editor as soon as startup completes without a late runtime console toggle.
	EnableNativeEditorAccessibilityBridge(*Settings);

	LogCapture = new FMonolithLogCapture();
	GLog->AddOutputDevice(LogCapture);

	FMonolithEditorActions::RegisterActions(LogCapture);
	FMonolithEditorMapActions::RegisterActions(FMonolithToolRegistry::Get());  // F8: create_empty_map + get_module_status
	FMonolithEditorSlateActions::RegisterActions(FMonolithToolRegistry::Get());
	// Gap 8: live-PIE object property read + function call (editor namespace).
	FMonolithPieObjectActions::RegisterActions(FMonolithToolRegistry::Get());
	// Gap 9: time-series PIE sampling with scripted provocation. Implemented in
	// MonolithEditor (it owns the async PIE-smoke session machinery) but registered
	// under the "animation" namespace string — the registry is namespace-string-keyed,
	// not module-keyed (see UnregisterNamespace note in ShutdownModule).
	FMonolithPieTimeseries::RegisterActions(FMonolithToolRegistry::Get());
	// Gap 4: deterministic PIE input/control driving (set control rotation with hold,
	// inject Enhanced Input action, free-fly spectator possess). The held-rotation /
	// repeated-input re-apply state is dropped on PIE end via the hook below.
	FMonolithPieInputActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithPieInputActions::RegisterPieEndHook();
	// Gap 10: programmatic stat-group counter/cycle readout (#if STATS gated).
	FMonolithStatActions::RegisterActions(FMonolithToolRegistry::Get());

	// Register settings detail customization
	FPropertyEditorModule& PropModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropModule.RegisterCustomClassLayout(
		UMonolithSettings::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMonolithSettingsCustomization::MakeInstance)
	);

	const int32 EditorActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("editor")).Num();
	UE_LOG(LogMonolith, Log, TEXT("Monolith — Editor module loaded (%d editor actions)"), EditorActionCount);

	// PART C — classify slow tasks before the nested loop, defer genuine modal
	// harvesting until Slate has registered the incoming window, and emit paired
	// MODAL_OPEN/MODAL_CLOSE records with the upstream window identity.
#if WITH_EDITOR
	if (FSlateApplication::IsInitialized())
	{
		ModalLoopTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddRaw(
			this, &FMonolithEditorModule::OnModalLoopTick);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		PreSlateModalHandle = FCoreDelegates::PreSlateModalWithContext.AddRaw(this, &FMonolithEditorModule::OnPreSlateModal);
		PostSlateModalHandle = FCoreDelegates::PostSlateModalWithContext.AddRaw(this, &FMonolithEditorModule::OnPostSlateModal);
#else
		PreSlateModalHandle = FCoreDelegates::PreSlateModal.AddRaw(this, &FMonolithEditorModule::OnPreSlateModal);
		PostSlateModalHandle = FCoreDelegates::PostSlateModal.AddRaw(this, &FMonolithEditorModule::OnPostSlateModal);
#endif
	}
#endif
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
void FMonolithEditorModule::OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context)
{
	const int64 Id = static_cast<int64>(Context.WindowIdentifier);
	const bool bSlowTask = Context.bIsSlowTaskWindow.Get(false);
	FOpenModalRecord Record;
	Record.SlowTask = bSlowTask ? TEXT("true") : TEXT("false");
	Record.OpenedAt = FDateTime::Now();
	OpenModals.Add(Id, Record);

	if (bSlowTask)
	{
		UE_LOG(LogMonolith, Display,
			TEXT("SLOW_TASK_OPEN ts='%s' blocking=false action='wait_for_completion' — transient progress window; no dismissal is required."),
			*Record.OpenedAt.ToString(TEXT("%Y-%m-%dT%H:%M:%S")));
		return;
	}

	// Do not inspect Slate here: the incoming genuine modal is not reliably on the
	// active-modal stack until the first modal-loop tick.
	PendingModalIdentifiers.Add(Id);
}
#else
void FMonolithEditorModule::OnPreSlateModal()
{
	// UE 5.7 has no slow-task classification. Defer capture until the first modal
	// loop tick so the actual modal window is on Slate's active-modal stack.
	bBlockingModalLogPending = true;
}
#endif

void FMonolithEditorModule::OnModalLoopTick(float DeltaTime)
{
	FMonolithEditorSlateActions::TickModalRecovery(DeltaTime);
	int64 Id = 0;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (PendingModalIdentifiers.Num() == 0)
	{
		return;
	}
	Id = PendingModalIdentifiers[0];
	PendingModalIdentifiers.RemoveAt(0);
#else
	if (!bBlockingModalLogPending)
	{
		return;
	}
	bBlockingModalLogPending = false;
#endif

	FString Title;
	FString Text;
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		TSharedPtr<SWindow> Window = Slate.GetActiveModalWindow();
		if (Window.IsValid())
		{
			Title = Window->GetTitle().ToString();
			HarvestTextBlocks(Window->GetContent(), Text, 0);
		}
	}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (FOpenModalRecord* Record = OpenModals.Find(Id))
	{
		Record->Title = Title;
	}
#endif
	UE_LOG(LogMonolith, Warning,
		TEXT("MODAL_OPEN ts='%s' id=%lld slow_task=false title='%s' text='%s' — blocking modal loop; deferred Slate actions admit restricted recovery requests, while legacy synchronous actions remain blocked until dismissal."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")), Id, *Title, *Text);
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
void FMonolithEditorModule::OnPostSlateModal(const FCoreDelegates::FModalWindowContext& Context)
{
	const int64 Id = static_cast<int64>(Context.WindowIdentifier);
	FString Title;
	FString SlowTask = TEXT("unknown");
	FString OpenAge = TEXT("unknown");
	if (const FOpenModalRecord* Record = OpenModals.Find(Id))
	{
		Title = Record->Title;
		SlowTask = Record->SlowTask;
		OpenAge = FString::Printf(TEXT("%.1f"), (FDateTime::Now() - Record->OpenedAt).GetTotalSeconds());
		OpenModals.Remove(Id);
	}

	UE_LOG(LogMonolith, Display,
		TEXT("MODAL_CLOSE ts='%s' id=%lld slow_task=%s title='%s' open_age_s=%s — modal dismissed; game thread resumed."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")), Id, *SlowTask, *Title, *OpenAge);
}
#else
void FMonolithEditorModule::OnPostSlateModal()
{
	UE_LOG(LogMonolith, Display,
		TEXT("MODAL_CLOSE ts='%s' id=0 slow_task=unknown title='' open_age_s=unknown — modal dismissed; game thread resumed."),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S")));
}
#endif

void FMonolithEditorModule::ShutdownModule()
{
	FMonolithEditorSlateActions::Shutdown();
#if WITH_EDITOR
	if (PreSlateModalHandle.IsValid())
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		FCoreDelegates::PreSlateModalWithContext.Remove(PreSlateModalHandle);
#else
		FCoreDelegates::PreSlateModal.Remove(PreSlateModalHandle);
#endif
		PreSlateModalHandle.Reset();
	}
	if (PostSlateModalHandle.IsValid())
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		FCoreDelegates::PostSlateModalWithContext.Remove(PostSlateModalHandle);
#else
		FCoreDelegates::PostSlateModal.Remove(PostSlateModalHandle);
#endif
		PostSlateModalHandle.Reset();
	}
	if (ModalLoopTickHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalLoopTickHandle);
		ModalLoopTickHandle.Reset();
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	OpenModals.Empty();
	PendingModalIdentifiers.Empty();
#else
	bBlockingModalLogPending = false;
#endif
#endif

	// Gap 4: drop the PIE-end hook + any residual held-rotation / repeated-input / spectator state.
	FMonolithPieInputActions::UnregisterPieEndHook();

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("editor"));

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropModule.UnregisterCustomClassLayout(UMonolithSettings::StaticClass()->GetFName());
	}

	if (LogCapture)
	{
		GLog->RemoveOutputDevice(LogCapture);
		delete LogCapture;
		LogCapture = nullptr;
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithEditorModule, MonolithEditor)
