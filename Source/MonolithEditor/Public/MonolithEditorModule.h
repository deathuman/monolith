#pragma once

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
	FDelegateHandle PreSlateModalHandle;
	FDelegateHandle ModalLoopTickHandle;
	bool bBlockingModalLogPending = false;

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	void OnPreSlateModal(const FCoreDelegates::FModalWindowContext& Context);
#else
	void OnPreSlateModal();
#endif
	void OnModalLoopTick(float DeltaTime);
};
