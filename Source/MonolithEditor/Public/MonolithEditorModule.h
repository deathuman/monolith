#pragma once

#include "CoreMinimal.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"
#include "Delegates/IDelegateInstance.h"

class FMonolithLogCapture;

class FMonolithEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FMonolithLogCapture* LogCapture = nullptr;

	// PART C — passive modal watcher. UE 5.8+ classifies slow-task windows before
	// they enter their nested loop; genuine blocking modals are harvested on the
	// first modal-loop tick, after Slate has pushed the actual window onto its stack.
	// Paired MODAL_OPEN/MODAL_CLOSE records carry the upstream window identity and
	// slow-task classification without reintroducing false modal captures.
	FDelegateHandle PreSlateModalHandle;
	FDelegateHandle PostSlateModalHandle;
	FDelegateHandle ModalLoopTickHandle;

#if WITH_EDITOR
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	struct FOpenModalRecord
	{
		FString Title;
		FString SlowTask;
		FDateTime OpenedAt;
	};
	TMap<int64, FOpenModalRecord> OpenModals;
	TArray<int64> PendingModalIdentifiers;
#else
	bool bBlockingModalLogPending = false;
#endif
#endif

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
	void OnPostSlateModal(const FCoreDelegates::FModalWindowContext& Context);
#else
	void OnPreSlateModal();
	void OnPostSlateModal();
#endif
	void OnModalLoopTick(float DeltaTime);
};
