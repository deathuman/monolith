#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.generated.h"

class FMonolithSourceIndexer;
class FJsonObject;
// UE 5.7: declared without underlying type at UObject/UObjectGlobals.h:3216 — match exactly.
enum class EReloadCompleteReason;

/**
 * Editor subsystem that owns the engine source DB and triggers C++ source indexing.
 */
UCLASS()
class MONOLITHSOURCE_API UMonolithSourceSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual ~UMonolithSourceSubsystem();
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Get the source database (read-only). May be null if DB doesn't exist. */
	FMonolithSourceDatabase* GetDatabase() { return Database.IsValid() ? Database.Get() : nullptr; }

	/**
	 * Full reindex: engine + shaders + project source (clean build).
	 * @return true if a run was actually started. False means nothing is
	 *         happening — a run is already in flight, or the worker thread
	 *         could not be created.
	 */
	bool TriggerReindex();

	/**
	 * Incremental project-only reindex: loads existing engine symbols, indexes only project C++ source.
	 * @return true if a run was actually started. False means nothing is
	 *         happening — a run is already in flight, the engine DB is missing,
	 *         or the worker thread could not be created.
	 */
	bool TriggerProjectReindex();

	/** Is indexing currently running? */
	bool IsIndexing() const { return bIsIndexing; }

	/** Read-only status snapshot for source.get_index_status. */
	TSharedPtr<FJsonObject> GetIndexStatus() const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test seam for deterministic startup-failure coverage; never used in production builds. */
	static void SetStartAsyncOverrideForTests(TFunction<bool(FMonolithSourceIndexer*)> Override);
	/** Test seam for deterministic completion/reopen failure coverage. */
	static void SetReopenDatabaseOverrideForTests(TFunction<bool(const FString&)> Override);
	void CompleteIndexForTests(const FString& DbPath, int32 Files, int32 Symbols, int32 Errors, bool bCancelled, int32 Discovered);
#endif

private:
	FString GetDatabasePath() const;
	FString GetEngineSourcePath() const;
	FString GetEngineShaderPath() const;
	FString GetProjectPath() const;
	bool ReopenDatabase(const FString& DbPath);
	void HandleIndexComplete(const FString& DbPath, int32 Files, int32 Symbols, int32 Errors, bool bCancelled, int32 Discovered);
	void MarkIndexFailure(const FString& Mode, const FString& Error);

	/**
	 * F17 (2026-04-26): Auto-reindex hook. Fires when Live Coding / hot-reload completes.
	 * Kicks an incremental project-only reindex so newly-shipped C++ symbols become
	 * queryable via source_query without requiring a manual `source.trigger_project_reindex` call.
	 *
	 * Cooldown-guarded (LastReindexTimeSeconds + 5s) and idempotency-guarded
	 * (bIsIndexing) to prevent storming when UBT fires multiple reload signals back-to-back.
	 */
	void OnReloadComplete(EReloadCompleteReason Reason);

	TUniquePtr<FMonolithSourceDatabase> Database;
	FMonolithSourceIndexer* Indexer = nullptr;
	TAtomic<bool> bIsIndexing{false};

	/**
	 * Set at the top of Deinitialize(). A completion callback already queued on
	 * the game thread cannot be cancelled, so it re-checks this before touching
	 * the database.
	 */
	TAtomic<bool> bIsShuttingDown{false};

	/** F17: Handle into FCoreUObjectDelegates::ReloadCompleteDelegate; cleared on Deinitialize. */
	FDelegateHandle ReloadCompleteHandle;

	/** F17: FPlatformTime::Seconds() at last successful auto-kick — used for the 5s cooldown. */
	double LastReindexTimeSeconds = 0.0;

	mutable FCriticalSection StatusLock;
	FString IndexState = TEXT("idle");
	FString IndexMode = TEXT("none");
	int64 IndexGeneration = 0;
	int32 IndexSchemaVersion = 0;
	bool bRequiresFullReindex = true;
	FString IndexStartedAt;
	FString IndexCompletedAt;
	int32 IndexDiscoveredFiles = 0;
	int32 IndexProcessedFiles = 0;
	int32 IndexSymbols = 0;
	int32 IndexErrors = 0;
	FString IndexLastError;
};
