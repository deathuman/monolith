#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Read-only semantic observation of the live editor Slate tree.
 *
 * The snapshot intentionally returns a bounded set of visible, semantic or
 * actionable controls rather than serializing every layout widget. It is an
 * observation surface for agent workflows and a coordinate/semantic guide for
 * external Computer Use when platform accessibility is unavailable.
 */
class FMonolithEditorSlateActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void Shutdown();
	static void TickModalRecovery(float DeltaTime);
	static FMonolithActionResult HandleGetSlateOperation(const TSharedPtr<FJsonObject>& Params);

	static FMonolithActionResult HandleGetSlateSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleClickSlateButton(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDismissSlateMenus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleInteractSlate(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureSlateWindow(const TSharedPtr<FJsonObject>& Params);
};
