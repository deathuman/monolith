#include "MonolithEditorSlateActions.h"
#include "MonolithParamSchema.h"
#include "MonolithUIRecovery.h"
#include "HttpServerModule.h"
#include "Containers/Ticker.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
	constexpr int32 DefaultMaxControls = 120;
	constexpr int32 DefaultMaxDepth = 32;
	constexpr int32 AbsoluteWalkLimit = 10000;
	constexpr int32 MaximumRequestedDepth = 64;
	struct FObservedControl
	{
		TWeakPtr<SWidget> Widget;
		FSlateRect Bounds;
		FString Text;
		int32 WindowIndex;
		FString Value;
	};
	FString SnapshotId;
	TArray<FObservedControl> ObservedControls;
	TArray<TWeakPtr<SWindow>> ObservedWindows;
	struct FSlateOperation
	{
		FString Id, Operation, Value;
		FObservedControl Target;
		TWeakPtr<SWindow> Window;
		double QueuedAt = 0, Deadline = 0, FinishedAt = 0;
		FString Phase = TEXT("queued");
		FString Error;
		bool bTimedOut = false;
	};
	TArray<TSharedPtr<FSlateOperation>> Operations;
	TSharedPtr<FSlateOperation> PendingOperation;
	int32 RunningUIActions = 0;
	bool bPumpingRecovery = false;
	FTSTicker::FDelegateHandle UITicker;
	void DrainUIOperation();
#if WITH_DEV_AUTOMATION_TESTS
	bool bTestSlateUnavailable = false;
	bool bTestAllowModalRecoveryTickDuringAction = false;
	// Lets the focused automation test exercise the pump without a live listener.
	TFunction<void(float)> ModalRecoveryHttpTickOverride;
#endif
	bool IsSlateAvailableForOperation()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (bTestSlateUnavailable) return false;
#endif
		return FSlateApplication::IsInitialized();
	}
	void PositionPointerForClick(FSlateApplication& Slate, const FSlateRect& Bounds)
	{
		const FVector2D Previous = Slate.GetCursorPos();
		const FVector2D Position((Bounds.Left + Bounds.Right) * 0.5f, (Bounds.Top + Bounds.Bottom) * 0.5f);
		Slate.SetCursorPos(Position);
		const FPointerEvent Move(FSlateApplication::CursorPointerIndex, Position, Previous, TSet<FKey>(), EKeys::Invalid, 0, FModifierKeysState());
		Slate.ProcessMouseMoveEvent(Move, true);
	}

	FString GetEditableValue(const TSharedPtr<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("SEditableText")) return StaticCastSharedPtr<SEditableText>(Widget)->GetText().ToString();
		if (Widget->GetType() == TEXT("SEditableTextBox") || Widget->GetType() == TEXT("SSearchBox"))
			return StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString();
		return FString();
	}

	bool IsReachableEnabled(const TSharedRef<SWidget>& Root, const TSharedPtr<SWidget>& Target, int32& Budget, int32 Depth = 0)
	{
		if (--Budget < 0 || Depth > MaximumRequestedDepth || !Root->IsEnabled() || !Root->GetVisibility().IsVisible()) return false;
		if (Root == Target) return Root->GetVisibility().IsHitTestVisible();
		if (!Root->GetVisibility().AreChildrenHitTestVisible()) return false;
		if (FChildren* Children = Root->GetChildren())
			for (int32 Index = 0; Index < Children->Num(); ++Index)
				if (IsReachableEnabled(Children->GetChildAt(Index), Target, Budget, Depth + 1)) return true;
		return false;
	}

	TSharedPtr<FJsonObject> MakeBoundsObject(const FGeometry& Geometry)
	{
		const FSlateRect Bounds = Geometry.GetLayoutBoundingRect();
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Bounds.Left);
		Json->SetNumberField(TEXT("y"), Bounds.Top);
		Json->SetNumberField(TEXT("width"), FMath::Max(0.0f, Bounds.Right - Bounds.Left));
		Json->SetNumberField(TEXT("height"), FMath::Max(0.0f, Bounds.Bottom - Bounds.Top));
		return Json;
	}

	bool HasUsableBounds(const FGeometry& Geometry)
	{
		const FSlateRect Bounds = Geometry.GetLayoutBoundingRect();
		return Bounds.Right > Bounds.Left && Bounds.Bottom > Bounds.Top;
	}

	FString GetSemanticText(const TSharedPtr<SWidget>& Widget)
	{
		if (!Widget.IsValid())
		{
			return FString();
		}

		FString Text = Widget->GetAccessibleText().ToString().TrimStartAndEnd();
		if (!Text.IsEmpty())
		{
			return Text;
		}

		// STextBlock is the most common editor label and occasionally has no
		// explicit accessibility behavior. Read its own text without recursively
		// summarizing child subtrees, which would duplicate labels and inflate output.
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			return StaticCastSharedPtr<STextBlock>(Widget)->GetText().ToString().TrimStartAndEnd();
		}

		return FString();
	}

	bool LooksActionable(const FString& TypeName)
	{
		static const TCHAR* ActionableTokens[] = {
			TEXT("Button"), TEXT("CheckBox"), TEXT("Combo"), TEXT("Editable"),
			TEXT("Search"), TEXT("SpinBox"), TEXT("Slider"), TEXT("Tab"),
			TEXT("Menu"), TEXT("Hyperlink"), TEXT("TableRow"), TEXT("TreeView"),
			TEXT("ListView")
		};

		for (const TCHAR* Token : ActionableTokens)
		{
			if (TypeName.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> DescribeWidget(const TSharedPtr<SWidget>& Widget)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Widget.IsValid())
		{
			return Json;
		}

		Json->SetStringField(TEXT("type"), Widget->GetType().ToString());
		Json->SetStringField(TEXT("text"), GetSemanticText(Widget));
		if (Widget->GetType() == TEXT("SEditableText") || Widget->GetType() == TEXT("SEditableTextBox") || Widget->GetType() == TEXT("SSearchBox"))
			Json->SetStringField(TEXT("value"), GetEditableValue(Widget));
		Json->SetBoolField(TEXT("enabled"), Widget->IsEnabled());
		Json->SetBoolField(TEXT("focusable"), Widget->SupportsKeyboardFocus());
		Json->SetStringField(TEXT("visibility"), Widget->GetVisibility().ToString());
		Json->SetObjectField(TEXT("screen_bounds"), MakeBoundsObject(Widget->GetCachedGeometry()));
		return Json;
	}

	struct FSnapshotBuilder
	{
		int32 MaxControls = DefaultMaxControls;
		int32 MaxDepth = DefaultMaxDepth;
		FString Filter;
		TSharedPtr<SWidget> FocusedWidget;
		TArray<TSharedPtr<FJsonValue>> Controls;
		int32 WalkedWidgets = 0;
		int32 MatchedControls = 0;
		int32 FocusedControlIndex = INDEX_NONE;
		bool bTruncated = false;
		bool bDepthTruncated = false;
		bool bWalkLimitReached = false;

		bool MatchesFilter(const FString& TypeName, const FString& Text) const
		{
			return Filter.IsEmpty()
				|| TypeName.Contains(Filter, ESearchCase::IgnoreCase)
				|| Text.Contains(Filter, ESearchCase::IgnoreCase);
		}

		void Walk(
			const TSharedPtr<SWidget>& Widget,
			int32 Depth,
			int32 WindowIndex,
			const FString& NearestActionText = FString())
		{
			if (!Widget.IsValid() || bWalkLimitReached)
			{
				return;
			}

			++WalkedWidgets;
			if (WalkedWidgets > AbsoluteWalkLimit)
			{
				bWalkLimitReached = true;
				bTruncated = true;
				return;
			}

			if (Depth > MaxDepth)
			{
				bDepthTruncated = true;
				bTruncated = true;
				return;
			}

			const EVisibility Visibility = Widget->GetVisibility();
			if (!Visibility.IsVisible())
			{
				return;
			}

			const FString TypeName = Widget->GetType().ToString();
			const FString Text = GetSemanticText(Widget);
			const bool bFocused = Widget == FocusedWidget;
			const bool bActionable = Widget->SupportsKeyboardFocus() || LooksActionable(TypeName);
			const bool bHasUsableBounds = HasUsableBounds(Widget->GetCachedGeometry());
			const bool bDuplicateActionLabel = TypeName == TEXT("STextBlock")
				&& !NearestActionText.IsEmpty()
				&& Text.Equals(NearestActionText, ESearchCase::IgnoreCase);
			const bool bSemantic = bFocused || (bHasUsableBounds && (bActionable || !Text.IsEmpty()));

			if (bSemantic && !bDuplicateActionLabel && MatchesFilter(TypeName, Text))
			{
				++MatchedControls;
				if (Controls.Num() < MaxControls)
				{
					const int32 SnapshotIndex = Controls.Num();
					TSharedPtr<FJsonObject> Row = DescribeWidget(Widget);
					Row->SetNumberField(TEXT("index"), SnapshotIndex);
					Row->SetNumberField(TEXT("window_index"), WindowIndex);
					Row->SetNumberField(TEXT("depth"), Depth);
					Row->SetBoolField(TEXT("focused"), bFocused);
					Controls.Add(MakeShared<FJsonValueObject>(Row));
					ObservedControls.Add({Widget, Widget->GetCachedGeometry().GetLayoutBoundingRect(), Text, WindowIndex, GetEditableValue(Widget)});
					if (bFocused)
					{
						FocusedControlIndex = SnapshotIndex;
					}
				}
				else
				{
					bTruncated = true;
				}
			}

			if (FChildren* Children = Widget->GetChildren())
			{
				FString ChildNearestActionText = NearestActionText;
				if (bActionable && !Text.IsEmpty())
				{
					ChildNearestActionText = Text;
				}
				for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
				{
					Walk(Children->GetChildAt(ChildIndex), Depth + 1, WindowIndex, ChildNearestActionText);
					if (bWalkLimitReached)
					{
						break;
					}
				}
			}
		}
	};

	struct FButtonMatch
	{
		TSharedPtr<SButton> Button;
		int32 WindowIndex = INDEX_NONE;
		int32 Depth = 0;
		FString WindowTitle;
	};

	void FindExactSlateButtons(
		const TSharedPtr<SWidget>& Widget,
		const FString& TargetText,
		int32 Depth,
		int32 MaxDepth,
		int32 WindowIndex,
		const FString& WindowTitle,
		TArray<FButtonMatch>& OutMatches,
		int32& InOutWalked)
	{
		if (!Widget.IsValid() || Depth > MaxDepth || InOutWalked >= AbsoluteWalkLimit)
		{
			return;
		}
		++InOutWalked;

		if (!Widget->GetVisibility().IsVisible() || !Widget->IsEnabled())
		{
			return;
		}

		if ((Widget->GetType() == TEXT("SButton") || Widget->GetType() == TEXT("SMenuEntryButton"))
			&& Widget->IsEnabled()
			&& Widget->GetVisibility().IsHitTestVisible()
			&& HasUsableBounds(Widget->GetCachedGeometry())
			&& GetSemanticText(Widget).Equals(TargetText, ESearchCase::IgnoreCase))
		{
			FButtonMatch Match;
			Match.Button = StaticCastSharedPtr<SButton>(Widget);
			Match.WindowIndex = WindowIndex;
			Match.Depth = Depth;
			Match.WindowTitle = WindowTitle;
			OutMatches.Add(MoveTemp(Match));
		}

		if (!Widget->GetVisibility().AreChildrenHitTestVisible()) return;
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
			{
				FindExactSlateButtons(
					Children->GetChildAt(ChildIndex),
					TargetText,
					Depth + 1,
					MaxDepth,
					WindowIndex,
					WindowTitle,
					OutMatches,
					InOutWalked);
				if (InOutWalked >= AbsoluteWalkLimit)
				{
					break;
				}
			}
		}
	}
}

void FMonolithEditorSlateActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	if (!UITicker.IsValid())
		UITicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) { DrainUIOperation(); return true; }));
	Registry.RegisterAction(TEXT("editor"), TEXT("get_slate_operation"),
		TEXT("Poll one deferred UI operation. Timeout reports current windows/modal without retrying or automatically dismissing anything."),
		FMonolithActionHandler::CreateStatic(&HandleGetSlateOperation),
		FParamSchemaBuilder().Required(TEXT("operation_id"), TEXT("string"), TEXT("Id returned by interact_slate")).Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("interact_slate"),
		TEXT("Queue one UI action outside the HTTP callback. Returns operation_id; poll status then re-observe. Rejects stale, disabled, detached and modal-blocked targets. One action consumes the snapshot."),
		FMonolithActionHandler::CreateStatic(&HandleInteractSlate),
		FParamSchemaBuilder().Required(TEXT("snapshot_id"), TEXT("string"), TEXT("Latest snapshot id"))
		.Required(TEXT("control_index"), TEXT("integer"), TEXT("Observed control index"))
		.Required(TEXT("operation"), TEXT("string"), TEXT("click, set_text, enter, down, or escape"))
		.Optional(TEXT("value"), TEXT("string"), TEXT("Text for set_text"))
		.Optional(TEXT("timeout_seconds"), TEXT("number"), TEXT("Observation deadline, 1..30 seconds; timeout never repeats the action"), TEXT("5"))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("capture_slate_window"),
		TEXT("Capture one observed editor/dialog window through the Slate renderer to Saved/AI/SlateCaptures."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSlateWindow),
		FParamSchemaBuilder().Required(TEXT("snapshot_id"), TEXT("string"), TEXT("Latest snapshot id"))
		.Required(TEXT("window_index"), TEXT("integer"), TEXT("Observed window index"))
		.Build());
	Registry.RegisterAction(
		TEXT("editor"),
		TEXT("get_slate_snapshot"),
		TEXT("Return a bounded read-only snapshot of visible semantic/actionable Slate controls, top-level windows, focus, modal identity, and screen-space bounds. Use it to ground editor interaction when platform accessibility is weak."),
		FMonolithActionHandler::CreateStatic(&HandleGetSlateSnapshot),
		FParamSchemaBuilder()
			.Optional(TEXT("max_controls"), TEXT("integer"), TEXT("Maximum semantic/actionable controls returned. Clamped to 10..500."), TEXT("120"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Maximum Slate child depth traversed per visible window. Clamped to 1..64."), TEXT("32"))
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Optional case-insensitive substring matched against widget type or semantic text."))
			.Build());

	Registry.RegisterAction(
		TEXT("editor"),
		TEXT("click_slate_button"),
		TEXT("Click one visible enabled Slate SButton resolved by a unique exact semantic-text match. Read-only observation should precede this action; re-observe after it because layout/focus/modality may change."),
		FMonolithActionHandler::CreateStatic(&HandleClickSlateButton),
		FParamSchemaBuilder()
			.Required(TEXT("text"), TEXT("string"), TEXT("Exact case-insensitive semantic text of the visible SButton, normally copied from get_slate_snapshot."))
			.Optional(TEXT("window_title"), TEXT("string"), TEXT("Optional exact case-insensitive top-level window title used to disambiguate."))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Maximum Slate child depth searched. Clamped to 1..64."), TEXT("32"))
			.Build());

	Registry.RegisterAction(
		TEXT("editor"),
		TEXT("dismiss_slate_menus"),
		TEXT("Dismiss all currently open Slate menus using FSlateApplication::DismissAllMenus. Idempotent recovery/cleanup action for unattended editor interaction."),
		FMonolithActionHandler::CreateStatic(&HandleDismissSlateMenus),
		FParamSchemaBuilder().Build());
}

FMonolithActionResult FMonolithEditorSlateActions::HandleGetSlateSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FMonolithActionResult::Error(TEXT("Slate application is not initialized"));
	}

	FSnapshotBuilder Builder;
	SnapshotId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ObservedControls.Reset();
	ObservedWindows.Reset();
	if (Params.IsValid())
	{
		double MaxControlsValue = Builder.MaxControls;
		if (Params->TryGetNumberField(TEXT("max_controls"), MaxControlsValue))
		{
			Builder.MaxControls = FMath::Clamp(FMath::RoundToInt(MaxControlsValue), 10, 500);
		}

		double MaxDepthValue = Builder.MaxDepth;
		if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthValue))
		{
			Builder.MaxDepth = FMath::Clamp(FMath::RoundToInt(MaxDepthValue), 1, MaximumRequestedDepth);
		}
		Params->TryGetStringField(TEXT("filter"), Builder.Filter);
		Builder.Filter.TrimStartAndEndInline();
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	Builder.FocusedWidget = Slate.GetKeyboardFocusedWidget();
	const TSharedPtr<SWindow> ActiveModal = Slate.GetActiveModalWindow();

	TArray<TSharedRef<SWindow>> VisibleWindows;
	Slate.GetAllVisibleWindowsOrdered(VisibleWindows);
	const TArray<TSharedRef<SWindow>> InteractiveWindows = Slate.GetInteractiveTopLevelWindows();
	TSet<const SWindow*> InteractiveWindowSet;
	for (const TSharedRef<SWindow>& Window : InteractiveWindows)
	{
		InteractiveWindowSet.Add(&Window.Get());
	}

	TArray<TSharedPtr<FJsonValue>> WindowsJson;
	int32 ActiveModalWindowIndex = INDEX_NONE;
	for (int32 WindowIndex = 0; WindowIndex < VisibleWindows.Num(); ++WindowIndex)
	{
		const TSharedRef<SWindow>& Window = VisibleWindows[WindowIndex];
		ObservedWindows.Add(Window);
		TSharedPtr<FJsonObject> WindowJson = MakeShared<FJsonObject>();
		WindowJson->SetNumberField(TEXT("index"), WindowIndex);
		WindowJson->SetStringField(TEXT("title"), Window->GetTitle().ToString());
		WindowJson->SetBoolField(TEXT("visible"), Window->IsVisible());
		WindowJson->SetBoolField(TEXT("interactive"), InteractiveWindowSet.Contains(&Window.Get()));
		const bool bIsModal = ActiveModal.IsValid() && ActiveModal.Get() == &Window.Get();
		WindowJson->SetBoolField(TEXT("active_modal"), bIsModal);
		WindowJson->SetObjectField(TEXT("screen_bounds"), MakeBoundsObject(Window->GetCachedGeometry()));
		WindowsJson.Add(MakeShared<FJsonValueObject>(WindowJson));
		if (bIsModal)
		{
			ActiveModalWindowIndex = WindowIndex;
		}

	}
	// A bounded snapshot must not spend its entire control budget on the
	// blocked background editor while a dialog owns interaction.
	if (ActiveModalWindowIndex != INDEX_NONE)
		Builder.Walk(VisibleWindows[ActiveModalWindowIndex]->GetContent(), 0, ActiveModalWindowIndex);
	for (int32 WindowIndex = 0; WindowIndex < VisibleWindows.Num(); ++WindowIndex)
		if (WindowIndex != ActiveModalWindowIndex)
			Builder.Walk(VisibleWindows[WindowIndex]->GetContent(), 0, WindowIndex);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("snapshot_id"), SnapshotId);
	const FVector2D CursorPosition = Slate.GetCursorPos();
	Result->SetNumberField(TEXT("cursor_x"), CursorPosition.X);
	Result->SetNumberField(TEXT("cursor_y"), CursorPosition.Y);
	Result->SetNumberField(TEXT("schema_version"), 1);
	Result->SetStringField(TEXT("scope"), TEXT("visible_semantic_actionable_controls"));
	Result->SetArrayField(TEXT("windows"), WindowsJson);
	Result->SetArrayField(TEXT("controls"), Builder.Controls);
	Result->SetNumberField(TEXT("visible_window_count"), VisibleWindows.Num());
	Result->SetNumberField(TEXT("walked_widgets"), Builder.WalkedWidgets);
	Result->SetNumberField(TEXT("matched_controls"), Builder.MatchedControls);
	Result->SetNumberField(TEXT("returned_controls"), Builder.Controls.Num());
	Result->SetNumberField(TEXT("max_controls"), Builder.MaxControls);
	Result->SetNumberField(TEXT("max_depth"), Builder.MaxDepth);
	Result->SetBoolField(TEXT("truncated"), Builder.bTruncated);
	Result->SetBoolField(TEXT("depth_truncated"), Builder.bDepthTruncated);
	Result->SetBoolField(TEXT("walk_limit_reached"), Builder.bWalkLimitReached);
	Result->SetNumberField(TEXT("active_modal_window_index"), ActiveModalWindowIndex);
	Result->SetNumberField(TEXT("focused_control_index"), Builder.FocusedControlIndex);
	Result->SetStringField(TEXT("filter"), Builder.Filter);
	if (Builder.FocusedWidget.IsValid())
	{
		Result->SetObjectField(TEXT("focused_widget"), DescribeWidget(Builder.FocusedWidget));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSlateActions::HandleClickSlateButton(const TSharedPtr<FJsonObject>& Params)
{
#if UE_BUILD_SHIPPING
	return FMonolithActionResult::Error(TEXT("click_slate_button is unavailable in Shipping builds"));
#else
	if (!FSlateApplication::IsInitialized())
	{
		return FMonolithActionResult::Error(TEXT("Slate application is not initialized"));
	}
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: text"));
	}

	FString TargetText;
	if (!Params->TryGetStringField(TEXT("text"), TargetText))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: text"));
	}
	TargetText.TrimStartAndEndInline();
	if (TargetText.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'text' must be non-empty"));
	}

	FString RequiredWindowTitle;
	Params->TryGetStringField(TEXT("window_title"), RequiredWindowTitle);
	RequiredWindowTitle.TrimStartAndEndInline();

	double MaxDepthValue = DefaultMaxDepth;
	Params->TryGetNumberField(TEXT("max_depth"), MaxDepthValue);
	const int32 MaxDepth = FMath::Clamp(FMath::RoundToInt(MaxDepthValue), 1, MaximumRequestedDepth);

	FSlateApplication& Slate = FSlateApplication::Get();
	TArray<TSharedRef<SWindow>> VisibleWindows;
	Slate.GetAllVisibleWindowsOrdered(VisibleWindows);

	TArray<FButtonMatch> Matches;
	int32 WalkedWidgets = 0;
	for (int32 WindowIndex = 0; WindowIndex < VisibleWindows.Num(); ++WindowIndex)
	{
		const TSharedRef<SWindow>& Window = VisibleWindows[WindowIndex];
		if (!Window->IsEnabled() || !Window->GetVisibility().AreChildrenHitTestVisible()) continue;
		const TSharedPtr<SWindow> Modal = Slate.GetActiveModalWindow();
		if (Modal.IsValid() && Modal != Window) continue;
		const FString WindowTitle = Window->GetTitle().ToString();
		if (!RequiredWindowTitle.IsEmpty()
			&& !WindowTitle.Equals(RequiredWindowTitle, ESearchCase::IgnoreCase))
		{
			continue;
		}

		FindExactSlateButtons(
			Window->GetContent(),
			TargetText,
			0,
			MaxDepth,
			WindowIndex,
			WindowTitle,
			Matches,
			WalkedWidgets);
	}

	if (Matches.Num() != 1)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("text"), TargetText);
		Data->SetStringField(TEXT("window_title"), RequiredWindowTitle);
		Data->SetNumberField(TEXT("matched_count"), Matches.Num());
		Data->SetNumberField(TEXT("walked_widgets"), WalkedWidgets);
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (const FButtonMatch& Match : Matches)
		{
			TSharedPtr<FJsonObject> Candidate = DescribeWidget(Match.Button);
			Candidate->SetNumberField(TEXT("window_index"), Match.WindowIndex);
			Candidate->SetStringField(TEXT("window_title"), Match.WindowTitle);
			Candidate->SetNumberField(TEXT("depth"), Match.Depth);
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Data->SetArrayField(TEXT("candidates"), Candidates);

		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expected one visible enabled SButton with semantic text '%s', found %d"),
			*TargetText,
			Matches.Num())).WithErrorData(Data);
	}

	const FButtonMatch& Match = Matches[0];
	TSharedPtr<FJsonObject> Target = DescribeWidget(Match.Button);
	Target->SetNumberField(TEXT("window_index"), Match.WindowIndex);
	Target->SetStringField(TEXT("window_title"), Match.WindowTitle);
	Target->SetNumberField(TEXT("depth"), Match.Depth);

	SnapshotId.Reset();
	const FSlateRect ClickBounds = Match.Button->GetCachedGeometry().GetLayoutBoundingRect();
	PositionPointerForClick(Slate, ClickBounds);
	Match.Button->SimulateClick();

	TArray<TSharedRef<SWindow>> PostWindows;
	Slate.GetAllVisibleWindowsOrdered(PostWindows);
	const TSharedPtr<SWindow> PostModal = Slate.GetActiveModalWindow();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("clicked"), true);
	Result->SetStringField(TEXT("text"), TargetText);
	Result->SetNumberField(TEXT("walked_widgets"), WalkedWidgets);
	Result->SetObjectField(TEXT("target"), Target);
	Result->SetNumberField(TEXT("post_visible_window_count"), PostWindows.Num());
	Result->SetStringField(TEXT("post_active_modal_title"), PostModal.IsValid() ? PostModal->GetTitle().ToString() : FString());
	Result->SetStringField(TEXT("next"), TEXT("Re-run get_slate_snapshot before the next UI action."));
	return FMonolithActionResult::Success(Result);
#endif
}

FMonolithActionResult FMonolithEditorSlateActions::HandleInteractSlate(const TSharedPtr<FJsonObject>& Params)
{
	FString Token, Operation, Value;
	double IndexValue = -1;
	if (!FSlateApplication::IsInitialized() || !Params.IsValid()
		|| !Params->TryGetStringField(TEXT("snapshot_id"), Token) || Token.IsEmpty() || Token != SnapshotId
		|| !Params->TryGetNumberField(TEXT("control_index"), IndexValue)
		|| IndexValue < 0 || IndexValue >= ObservedControls.Num() || IndexValue != FMath::FloorToDouble(IndexValue)
		|| !Params->TryGetStringField(TEXT("operation"), Operation))
		return FMonolithActionResult::Error(TEXT("Invalid or stale target; obtain a fresh get_slate_snapshot."));

	const FObservedControl Observed = ObservedControls[static_cast<int32>(IndexValue)];
	const TSharedPtr<SWidget> Widget = Observed.Widget.Pin();
	const TSharedPtr<SWindow> Window = ObservedWindows.IsValidIndex(Observed.WindowIndex)
		? ObservedWindows[Observed.WindowIndex].Pin() : nullptr;
	FSlateApplication& Slate = FSlateApplication::Get();
	const TSharedPtr<SWindow> Modal = Slate.GetActiveModalWindow();
	int32 Budget = AbsoluteWalkLimit;
	if (!Widget.IsValid() || !Window.IsValid() || !Window->IsVisible()
		|| (Modal.IsValid() && Modal != Window)
		|| !IsReachableEnabled(Window.ToSharedRef(), Widget, Budget)
		|| !HasUsableBounds(Widget->GetCachedGeometry())
		|| Widget->GetCachedGeometry().GetLayoutBoundingRect() != Observed.Bounds
		|| GetSemanticText(Widget) != Observed.Text || GetEditableValue(Widget) != Observed.Value)
		return FMonolithActionResult::Error(TEXT("Target is stale, disabled, detached, or blocked by an active modal; re-observe."));

	const FName Type = Widget->GetType();
	const bool bButton = Type == TEXT("SButton") || Type == TEXT("SMenuEntryButton");
	const bool bText = Type == TEXT("SEditableText") || Type == TEXT("SEditableTextBox") || Type == TEXT("SSearchBox");
	const bool bKey = Operation == TEXT("enter") || Operation == TEXT("down") || Operation == TEXT("escape");
	if ((Operation == TEXT("click") && !bButton)
		|| (Operation == TEXT("set_text") && (!bText || !Params->TryGetStringField(TEXT("value"), Value)))
		|| (bKey && !Widget->SupportsKeyboardFocus())
		|| (Operation != TEXT("click") && Operation != TEXT("set_text") && !bKey))
		return FMonolithActionResult::Error(TEXT("Operation unsupported for the observed control. No action taken."));

	if (PendingOperation.IsValid())
		return FMonolithActionResult::Error(TEXT("A UI action is already queued; poll its operation_id before submitting another."));
	// Keep a bounded history but never evict an active or queued operation.
	while (Operations.Num() >= 64)
	{
		const int32 Completed = Operations.IndexOfByPredicate([](const auto& Item) { return Item->Phase == TEXT("completed") || Item->Phase == TEXT("rejected") || Item->Phase == TEXT("expired"); });
		if (Completed == INDEX_NONE) return FMonolithActionResult::Error(TEXT("UI operation history is full of active operations."));
		Operations.RemoveAt(Completed);
	}
	double Timeout = 5;
	Params->TryGetNumberField(TEXT("timeout_seconds"), Timeout);
	if (!FMath::IsFinite(Timeout) || Timeout < 1 || Timeout > 30)
		return FMonolithActionResult::Error(TEXT("timeout_seconds must be finite and between 1 and 30."));
	auto Item = MakeShared<FSlateOperation>();
	Item->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Item->Target = Observed;
	Item->Window = Window;
	Item->Operation = Operation;
	Item->Value = Value;
	Item->QueuedAt = FPlatformTime::Seconds();
	Item->Deadline = Item->QueuedAt + Timeout;
	Operations.Add(Item);
	PendingOperation = Item;
	SnapshotId.Reset();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("queued"), true);
	Result->SetStringField(TEXT("operation_id"), Item->Id);
	Result->SetStringField(TEXT("operation"), Operation);
	Result->SetStringField(TEXT("next"), TEXT("Poll get_slate_operation; never repeat an action after a timeout. Re-observe to verify effects or recover the active modal."));
	return FMonolithActionResult::Success(Result);
}

namespace
{
	void DrainUIOperation()
	{
		if (!PendingOperation.IsValid() || IsMonolithActionExecuting()) return;
		const auto Item = PendingOperation;
		PendingOperation.Reset();
		if (!IsSlateAvailableForOperation())
		{
			Item->Phase = TEXT("rejected");
			Item->Error = TEXT("Slate is unavailable. No action taken.");
			Item->FinishedAt = FPlatformTime::Seconds();
			return;
		}
		if (FPlatformTime::Seconds() > Item->Deadline)
		{
			Item->Phase = TEXT("expired");
			Item->bTimedOut = true;
			Item->Error = TEXT("Deadline elapsed before dispatch. No action taken.");
			Item->FinishedAt = FPlatformTime::Seconds();
			return;
		}
		const auto Widget = Item->Target.Widget.Pin();
		const auto Window = Item->Window.Pin();
		auto& Slate = FSlateApplication::Get();
		const auto Modal = Slate.GetActiveModalWindow();
		int32 Budget = AbsoluteWalkLimit;
		if (!Widget.IsValid() || !Window.IsValid() || !Window->IsVisible()
			|| (Modal.IsValid() && Modal != Window)
			|| !IsReachableEnabled(Window.ToSharedRef(), Widget, Budget)
			|| Widget->GetCachedGeometry().GetLayoutBoundingRect() != Item->Target.Bounds
			|| GetSemanticText(Widget) != Item->Target.Text || GetEditableValue(Widget) != Item->Target.Value)
		{
			Item->Phase = TEXT("rejected");
			Item->Error = TEXT("Target changed between observation and dispatch. No action taken.");
			Item->FinishedAt = FPlatformTime::Seconds();
			return;
		}
		Item->Phase = TEXT("running");
		++RunningUIActions;
		if (Item->Operation == TEXT("click"))
		{
			// A synthetic click must move the pointer too: leaving it over another
			// menu heading lets Slate hover-switch menus on the following frame.
			const FSlateRect Bounds = Widget->GetCachedGeometry().GetLayoutBoundingRect();
			PositionPointerForClick(Slate, Bounds);
			StaticCastSharedPtr<SButton>(Widget)->SimulateClick();
		}
		else if (Item->Operation == TEXT("set_text"))
		{
			Slate.SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
			if (Widget->GetType() == TEXT("SEditableText")) StaticCastSharedPtr<SEditableText>(Widget)->SetText(FText::FromString(Item->Value));
			else StaticCastSharedPtr<SEditableTextBox>(Widget)->SetText(FText::FromString(Item->Value));
		}
		else
		{
			Slate.SetKeyboardFocus(Widget, EFocusCause::SetDirectly);
			const FKey Key = Item->Operation == TEXT("enter") ? EKeys::Enter : Item->Operation == TEXT("down") ? EKeys::Down : EKeys::Escape;
			const FKeyEvent Event(Key, FModifierKeysState(), 0, false, 0, 0);
			Slate.ProcessKeyDownEvent(Event);
			Slate.ProcessKeyUpEvent(Event);
		}
		--RunningUIActions;
		// Recovery polling may already have terminated an operation while its
		// callback was on the stack. Preserve that outcome and terminal time.
		if (Item->Phase == TEXT("running"))
		{
			Item->FinishedAt = FPlatformTime::Seconds();
			Item->bTimedOut |= Item->FinishedAt > Item->Deadline;
			Item->Phase = TEXT("completed");
		}
	}

	void PumpModalRecoveryHttpTick(float DeltaTime, TFunctionRef<void(float)> HttpTick)
	{
		{
			TGuardValue<bool> PumpGuard(bPumpingRecovery, true);
			const bool bPrevious = SetMonolithUIRecoveryOnly(true);
			HttpTick(DeltaTime);
			SetMonolithUIRecoveryOnly(bPrevious);
		}
		// Dispatch only after HTTP returns, so the listener cannot be re-entered
		// by the action queued by its own request.
		DrainUIOperation();
	}
}

void FMonolithEditorSlateActions::TickModalRecovery(float DeltaTime)
{
	// Deferred dispatch guarantees the initiating HTTP request is no longer on
	// the stack. Never pump HTTP for a legacy synchronous domain action.
#if WITH_DEV_AUTOMATION_TESTS
	if (IsMonolithActionExecuting() && !bTestAllowModalRecoveryTickDuringAction) return;
#else
	if (IsMonolithActionExecuting()) return;
#endif
	if (bPumpingRecovery) return;
#if WITH_DEV_AUTOMATION_TESTS
	if (ModalRecoveryHttpTickOverride)
	{
		PumpModalRecoveryHttpTick(DeltaTime, ModalRecoveryHttpTickOverride);
		return;
	}
#endif
	if (!FHttpServerModule::IsAvailable()) return;
	PumpModalRecoveryHttpTick(DeltaTime, [](float TickDeltaTime)
	{
		// Tick through the public virtual ticker interface; no Engine changes.
		static_cast<FTSTickerObjectBase&>(FHttpServerModule::Get()).Tick(TickDeltaTime);
	});
}

void FMonolithEditorSlateActions::Shutdown()
{
	if (UITicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(UITicker);
	UITicker.Reset();
	PendingOperation.Reset();
	Operations.Reset();
	SnapshotId.Reset();
	ObservedControls.Reset();
	ObservedWindows.Reset();
}

FMonolithActionResult FMonolithEditorSlateActions::HandleGetSlateOperation(const TSharedPtr<FJsonObject>& Params)
{
	FString Id;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("operation_id"), Id))
		return FMonolithActionResult::Error(TEXT("operation_id is required."));
	const auto* Found = Operations.FindByPredicate([&Id](const auto& Item) { return Item->Id == Id; });
	if (!Found) return FMonolithActionResult::Error(TEXT("Unknown or expired operation_id. Do not repeat an uncertain UI action; re-observe first."));
	const auto Item = *Found;
	const bool bSlateAvailable = IsSlateAvailableForOperation();
	if (!bSlateAvailable && (Item->Phase == TEXT("queued") || Item->Phase == TEXT("running")))
	{
		Item->Phase = TEXT("rejected");
		Item->Error = TEXT("Slate became unavailable. Re-observe after recovery; do not repeat an uncertain action.");
		Item->FinishedAt = FPlatformTime::Seconds();
		Item->bTimedOut |= Item->FinishedAt > Item->Deadline;
		if (PendingOperation == Item) PendingOperation.Reset();
	}
	const bool bActive = Item->Phase == TEXT("queued") || Item->Phase == TEXT("running");
	if (bActive && FPlatformTime::Seconds() > Item->Deadline) Item->bTimedOut = true;
	const TSharedPtr<SWindow> Modal = bSlateAvailable ? FSlateApplication::Get().GetActiveModalWindow() : nullptr;
	TArray<TSharedRef<SWindow>> Windows;
	if (bSlateAvailable) FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
	TArray<TSharedPtr<FJsonValue>> Titles;
	for (const auto& Window : Windows) Titles.Add(MakeShared<FJsonValueString>(Window->GetTitle().ToString()));
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("operation_id"), Id);
	Result->SetStringField(TEXT("phase"), Item->Phase == TEXT("running") && Modal.IsValid() ? TEXT("waiting_modal") : Item->Phase);
	Result->SetBoolField(TEXT("terminal"), !bActive);
	Result->SetBoolField(TEXT("slate_available"), bSlateAvailable);
	Result->SetBoolField(TEXT("timed_out"), Item->bTimedOut);
	Result->SetNumberField(TEXT("elapsed_seconds"), (bActive ? FPlatformTime::Seconds() : Item->FinishedAt) - Item->QueuedAt);
	Result->SetStringField(TEXT("error"), Item->Error);
	Result->SetStringField(TEXT("active_modal_title"), Modal.IsValid() ? Modal->GetTitle().ToString() : FString());
	Result->SetArrayField(TEXT("window_titles"), Titles);
	Result->SetStringField(TEXT("next"), Item->bTimedOut && bActive ? TEXT("Do not repeat this action. Observe the current modal and recover with a fresh target.") : TEXT("Re-observe to verify the intended effect. This operation was not retried."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSlateActions::HandleCaptureSlateWindow(const TSharedPtr<FJsonObject>& Params)
{
	FString Token;
	double IndexValue = -1;
	if (!FSlateApplication::IsInitialized() || !Params.IsValid()
		|| !Params->TryGetStringField(TEXT("snapshot_id"), Token) || Token.IsEmpty() || Token != SnapshotId
		|| !Params->TryGetNumberField(TEXT("window_index"), IndexValue)
		|| IndexValue < 0 || IndexValue >= ObservedWindows.Num() || IndexValue != FMath::FloorToDouble(IndexValue))
		return FMonolithActionResult::Error(TEXT("Invalid or stale window; obtain a fresh get_slate_snapshot."));
	const TSharedPtr<SWindow> Window = ObservedWindows[static_cast<int32>(IndexValue)].Pin();
	if (!Window.IsValid() || !Window->IsVisible())
		return FMonolithActionResult::Error(TEXT("Observed window is no longer visible."));
	TArray<FColor> Pixels;
	FIntVector Size;
	if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
		return FMonolithActionResult::Error(TEXT("Slate renderer could not capture the observed window."));
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AI/SlateCaptures"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = Directory / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png"));
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
		return FMonolithActionResult::Error(TEXT("Failed to write Slate window PNG."));
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("window_title"), Window->GetTitle().ToString());
	Result->SetNumberField(TEXT("width"), Size.X);
	Result->SetNumberField(TEXT("height"), Size.Y);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorSlateActions::HandleDismissSlateMenus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FMonolithActionResult::Error(TEXT("Slate application is not initialized"));
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const bool bHadVisibleMenu = Slate.GetVisibleMenuWindow().IsValid();
	SnapshotId.Reset();
	Slate.DismissAllMenus();
	const bool bHasVisibleMenuAfter = Slate.GetVisibleMenuWindow().IsValid();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("had_visible_menu"), bHadVisibleMenu);
	Result->SetBoolField(TEXT("visible_menu_after"), bHasVisibleMenuAfter);
	Result->SetBoolField(TEXT("dismissed"), bHadVisibleMenu && !bHasVisibleMenuAfter);
	Result->SetStringField(TEXT("next"), TEXT("Re-run get_slate_snapshot before the next UI action."));
	return FMonolithActionResult::Success(Result);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSlateUnavailableOperationTest,
	"Monolith.Editor.Accessibility.UnavailableOperation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSlateUnavailableOperationTest::RunTest(const FString&)
{
	if (!TestFalse(TEXT("No user operation pending before fixture"), PendingOperation.IsValid())) return false;
	for (const FString Phase : {FString(TEXT("queued")), FString(TEXT("running"))})
	{
		const auto Item = MakeShared<FSlateOperation>();
		Item->Id = FGuid::NewGuid().ToString();
		Item->Phase = Phase;
		Item->QueuedAt = FPlatformTime::Seconds();
		Item->Deadline = Item->QueuedAt + 5;
		Operations.Add(Item);
		if (Phase == TEXT("queued")) PendingOperation = Item;
		const auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation_id"), Item->Id);
		{
			// Inject the unavailable dependency state; do not destroy the editor's
			// global Slate instance to exercise this production status handler.
			TGuardValue<bool> Unavailable(bTestSlateUnavailable, true);
			const auto Result = FMonolithEditorSlateActions::HandleGetSlateOperation(Params);
			if (TestTrue(TEXT("Unavailable polling returns a structured result"), Result.bSuccess))
			{
				TestTrue(TEXT("Operation becomes terminal"), Result.Result->GetBoolField(TEXT("terminal")));
				TestFalse(TEXT("Availability is reported false"), Result.Result->GetBoolField(TEXT("slate_available")));
				TestEqual(TEXT("Unavailable operation rejected"), Result.Result->GetStringField(TEXT("phase")), FString(TEXT("rejected")));
				TestEqual(TEXT("No stale window inventory"), Result.Result->GetArrayField(TEXT("window_titles")).Num(), 0);
			}
		}
		const double Finished = Item->FinishedAt;
		const auto Recovered = FMonolithEditorSlateActions::HandleGetSlateOperation(Params);
		TestTrue(TEXT("Polling remains responsive after availability returns"), Recovered.bSuccess);
		TestEqual(TEXT("Terminal timestamp stays fixed"), Item->FinishedAt, Finished);
		TestEqual(TEXT("Recovery does not retry the operation"), Item->Phase, FString(TEXT("rejected")));
		TestFalse(TEXT("Unavailable queued input removed"), PendingOperation.IsValid());
		Operations.Remove(Item);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSlateModalRecoveryLivenessTest,
	"Monolith.Editor.Accessibility.ModalRecoveryLiveness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSlateModalRecoveryLivenessTest::RunTest(const FString&)
{
	if (!TestFalse(TEXT("No operation is pending before the fixture"), PendingOperation.IsValid())) return false;
	if (!TestEqual(TEXT("Recovery starts with no running UI actions"), RunningUIActions, 0)) return false;

	const FString ControlLabel = TEXT("__MonolithModalRecoveryLiveness__");
	int32 Clicks = 0;
	TSharedPtr<SButton> Button;
	const auto Window = SNew(SWindow).Title(FText::FromString(TEXT("Monolith modal recovery fixture"))).ClientSize(FVector2D(400, 180))
	[
		SAssignNew(Button, SButton)
		.Text(FText::FromString(ControlLabel))
		.OnClicked_Lambda([&Clicks]() { ++Clicks; return FReply::Handled(); })
	];
	TestTrue(TEXT("Fixture button is created"), Button.IsValid());
	auto& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window);
	Slate.Tick();

	const bool bPreviousRecoveryOnly = SetMonolithUIRecoveryOnly(false);
	FString OperationId;
	int32 HttpTicks = 0;
	bool bSnapshotServed = false;
	bool bOperationQueued = false;
	bool bQueuedOperationPolled = false;
	bool bUnrelatedActionBlocked = false;
	bool bNestedPumpBlocked = false;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const bool bPreviousTestActionBypass = bTestAllowModalRecoveryTickDuringAction;
	bTestAllowModalRecoveryTickDuringAction = false;
	ModalRecoveryHttpTickOverride = [this, &Registry, &OperationId, &HttpTicks, &bSnapshotServed, &bOperationQueued, &bQueuedOperationPolled, &bUnrelatedActionBlocked, &bNestedPumpBlocked, &ControlLabel](float DeltaTime)
	{
		++HttpTicks;
		TestEqual(TEXT("HTTP pump runs before any UI action"), RunningUIActions, 0);

		const auto Unrelated = Registry.ExecuteAction(TEXT("editor"), TEXT("get_module_status"), MakeShared<FJsonObject>());
		bUnrelatedActionBlocked = !Unrelated.bSuccess && Unrelated.ErrorMessage.Contains(TEXT("Modal recovery"));
		TestTrue(TEXT("Unrelated editor action remains blocked during recovery"), bUnrelatedActionBlocked);

		auto SnapshotParams = MakeShared<FJsonObject>();
		SnapshotParams->SetStringField(TEXT("filter"), ControlLabel);
		const auto Snapshot = Registry.ExecuteAction(TEXT("editor"), TEXT("get_slate_snapshot"), SnapshotParams);
		bSnapshotServed = Snapshot.bSuccess && Snapshot.Result.IsValid();
		TestTrue(TEXT("First recovery snapshot is served by the modal HTTP pump"), bSnapshotServed);
		if (bSnapshotServed)
		{
			const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
			TestTrue(TEXT("Recovery snapshot contains the observed fixture control"), Snapshot.Result->TryGetArrayField(TEXT("controls"), Controls));
			if (Controls && Controls->Num() > 0)
			{
				auto InteractParams = MakeShared<FJsonObject>();
				InteractParams->SetStringField(TEXT("snapshot_id"), Snapshot.Result->GetStringField(TEXT("snapshot_id")));
				InteractParams->SetNumberField(TEXT("control_index"), 0);
				InteractParams->SetStringField(TEXT("operation"), TEXT("click"));
				const auto Queued = Registry.ExecuteAction(TEXT("editor"), TEXT("interact_slate"), InteractParams);
				bOperationQueued = Queued.bSuccess && Queued.Result.IsValid();
				TestTrue(TEXT("Observed UI action queues through the recovery request"), bOperationQueued);
				if (bOperationQueued)
				{
					OperationId = Queued.Result->GetStringField(TEXT("operation_id"));
					auto PollParams = MakeShared<FJsonObject>();
					PollParams->SetStringField(TEXT("operation_id"), OperationId);
					const auto QueuedPoll = Registry.ExecuteAction(TEXT("editor"), TEXT("get_slate_operation"), PollParams);
					bQueuedOperationPolled = QueuedPoll.bSuccess && QueuedPoll.Result.IsValid()
						&& QueuedPoll.Result->GetStringField(TEXT("phase")) == TEXT("queued");
					TestTrue(TEXT("Queued operation can be polled before the HTTP tick returns"), bQueuedOperationPolled);
				}
			}
		}

		FMonolithEditorSlateActions::TickModalRecovery(DeltaTime);
		bNestedPumpBlocked = HttpTicks == 1;
		TestTrue(TEXT("Nested modal tick cannot re-enter the HTTP pump"), bNestedPumpBlocked);
	};

	const bool bActionExecuting = IsMonolithActionExecuting();
	if (bActionExecuting)
	{
		FMonolithEditorSlateActions::TickModalRecovery(0.016f);
		TestEqual(TEXT("Production action-execution guard blocks the synthetic tick"), HttpTicks, 0);
	}
	bTestAllowModalRecoveryTickDuringAction = true;
	FMonolithEditorSlateActions::TickModalRecovery(0.016f);
	bTestAllowModalRecoveryTickDuringAction = false;
	TestEqual(TEXT("Modal pump ran once with zero running actions"), HttpTicks, 1);
	if (bActionExecuting)
	{
		TestTrue(TEXT("Operation remains deferred while the outer Monolith action is active"), PendingOperation.IsValid());
		TestEqual(TEXT("Deferred operation does not dispatch inside the outer action"), Clicks, 0);
	}
	else
	{
		TestFalse(TEXT("Queued operation drains after the HTTP callback returns"), PendingOperation.IsValid());
		TestEqual(TEXT("Recovery operation dispatches after the pump returns"), Clicks, 1);
	}
	if (!OperationId.IsEmpty())
	{
		auto PollParams = MakeShared<FJsonObject>();
		PollParams->SetStringField(TEXT("operation_id"), OperationId);
		const auto Completed = Registry.ExecuteAction(TEXT("editor"), TEXT("get_slate_operation"), PollParams);
		TestTrue(TEXT("Operation status remains available after recovery"), Completed.bSuccess && Completed.Result.IsValid());
		if (Completed.bSuccess && Completed.Result.IsValid())
			TestEqual(TEXT("Operation reflects deferred dispatch state"), Completed.Result->GetStringField(TEXT("phase")), FString(bActionExecuting ? TEXT("queued") : TEXT("completed")));
	}
	const auto NormalAccess = Registry.ExecuteAction(TEXT("editor"), TEXT("get_module_status"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Normal editor action access resumes when recovery ends"), NormalAccess.bSuccess);
	TestFalse(TEXT("Pump reentrancy guard is released"), bPumpingRecovery);
	TestEqual(TEXT("UI action counter is balanced"), RunningUIActions, 0);

	ModalRecoveryHttpTickOverride = TFunction<void(float)>();
	bTestAllowModalRecoveryTickDuringAction = bPreviousTestActionBypass;
	if (PendingOperation.IsValid())
	{
		Operations.Remove(PendingOperation);
		PendingOperation.Reset();
	}
	if (!OperationId.IsEmpty()) Operations.RemoveAll([&OperationId](const auto& Item) { return Item->Id == OperationId; });
	SnapshotId.Reset();
	ObservedControls.Reset();
	ObservedWindows.Reset();
	Slate.RequestDestroyWindow(Window);
	Slate.Tick();
	SetMonolithUIRecoveryOnly(bPreviousRecoveryOnly);
	return true;
}
#endif
