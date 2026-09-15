// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#include "MonolithToolRegistry.h"
#include "MonolithChooserTestTypes.h"

#if WITH_CHOOSER && WITH_EDITORONLY_DATA

#include "Chooser.h"
#include "ChooserFunctionLibrary.h"
#include "ChooserPropertyAccess.h"
#include "EnumColumn.h"
#include "FloatRangeColumn.h"
#include "NameColumn.h"
#include "ObjectChooser_Asset.h"
#include "ObjectColumn.h"
#include "OutputObjectColumn.h"
#include "OutputStructColumn.h"
#include "Animation/AnimMontage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "PackageTools.h"
#include "StructUtils/InstancedStruct.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "Editor.h"

namespace MonolithChooserNameOutputStructTest
{
static const TCHAR* const ContextStructPath = TEXT("/Script/MonolithAnimation.MonolithChooserTestChooserContext");
static const TCHAR* const ContextClassPath = TEXT("/Script/MonolithAnimation.MonolithChooserTestContextObject");

static FMonolithActionResult Execute(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("chooser"), Action, Params);
}

static TSharedPtr<FJsonObject> Params(const TCHAR* AssetPath)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	return Result;
}

static UObject* CreateTransientAsset(const FString& AssetPath)
{
	UPackage* Package = CreatePackage(*AssetPath);
	const int32 Slash = AssetPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	return Package ? NewObject<UAnimMontage>(Package, FName(*AssetPath.RightChop(Slash + 1)), RF_Public | RF_Standalone) : nullptr;
}

static UScriptStruct* ContextStruct()
{
	return LoadObject<UScriptStruct>(nullptr, ContextStructPath);
}

static UScriptStruct* AttackStruct()
{
	return LoadObject<UScriptStruct>(nullptr, TEXT("/Script/MonolithAnimation.MonolithChooserTestAttackRowData"));
}

static bool SaveAsset(UObject* Asset)
{
	if (!Asset)
	{
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, SaveArgs);
}

static FMonolithActionResult CleanupAssets(const FString& TablePath, const FString& OutputPath)
{
	TSharedPtr<FJsonObject> CleanupParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Paths;
	Paths.Add(MakeShared<FJsonValueString>(TablePath));
	Paths.Add(MakeShared<FJsonValueString>(OutputPath));
	CleanupParams->SetArrayField(TEXT("paths"), Paths);
	CleanupParams->SetBoolField(TEXT("dry_run"), false);
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("project"), TEXT("cleanup_generated_assets"), CleanupParams);
}

static bool CleanupHandledBothAssets(const FMonolithActionResult& Result, bool bAllowAlreadyAbsent)
{
	const TArray<TSharedPtr<FJsonValue>>* Deleted = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Skipped = nullptr;
	if (!Result.bSuccess || !Result.Result.IsValid()
		|| !Result.Result->TryGetArrayField(TEXT("deleted"), Deleted) || !Deleted || Deleted->Num() != 2
		|| !Result.Result->TryGetArrayField(TEXT("skipped_with_referencers"), Skipped) || !Skipped || !Skipped->IsEmpty())
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Deleted)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		bool bDeleted = false;
		bool bAlreadyAbsent = false;
		if (!Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
		{
			return false;
		}
		(*Entry)->TryGetBoolField(TEXT("deleted"), bDeleted);
		(*Entry)->TryGetBoolField(TEXT("already_absent"), bAlreadyAbsent);
		if (!bDeleted && !(bAllowAlreadyAbsent && bAlreadyAbsent))
		{
			return false;
		}
	}
	return true;
}

static bool SetContextProfileAndResetOutput(
	UObject* ContextObject,
	UScriptStruct* ExpectedContextStruct,
	UScriptStruct* ExpectedAttackStruct,
	FName Profile)
{
	FStructProperty* ContextProperty = ContextObject
		? FindFProperty<FStructProperty>(ContextObject->GetClass(), TEXT("ChooserContext"))
		: nullptr;
	if (!ContextProperty || ContextProperty->Struct != ExpectedContextStruct)
	{
		return false;
	}

	void* ContextMemory = ContextProperty->ContainerPtrToValuePtr<void>(ContextObject);
	FNameProperty* ProfileProperty = FindFProperty<FNameProperty>(ExpectedContextStruct, TEXT("MovesetId"));
	FStructProperty* AttackProperty = FindFProperty<FStructProperty>(ExpectedContextStruct, TEXT("AttackRowData"));
	if (!ContextMemory || !ProfileProperty || !AttackProperty || AttackProperty->Struct != ExpectedAttackStruct)
	{
		return false;
	}

	FStructOnScope DefaultContext(ExpectedContextStruct);
	ExpectedContextStruct->CopyScriptStruct(ContextMemory, DefaultContext.GetStructMemory());
	ProfileProperty->SetPropertyValue(ProfileProperty->ContainerPtrToValuePtr<void>(ContextMemory), Profile);
	return true;
}

static float GetContextDamage(UObject* ContextObject, UScriptStruct* ContextStruct, UScriptStruct* AttackStruct)
{
	FStructProperty* ContextProperty = ContextObject
		? FindFProperty<FStructProperty>(ContextObject->GetClass(), TEXT("ChooserContext"))
		: nullptr;
	FStructProperty* AttackProperty = FindFProperty<FStructProperty>(ContextStruct, TEXT("AttackRowData"));
	FFloatProperty* DamageProperty = FindFProperty<FFloatProperty>(AttackStruct, TEXT("Damage"));
	if (!ContextProperty || !AttackProperty || !DamageProperty)
	{
		return -1.0f;
	}
	void* ContextMemory = ContextProperty->ContainerPtrToValuePtr<void>(ContextObject);
	void* AttackMemory = AttackProperty->ContainerPtrToValuePtr<void>(ContextMemory);
	return DamageProperty->GetPropertyValue(DamageProperty->ContainerPtrToValuePtr<void>(AttackMemory));
}

static bool IsContextAttackDefault(UObject* ContextObject, UScriptStruct* ContextStruct, UScriptStruct* AttackStruct)
{
	FStructProperty* ContextProperty = ContextObject
		? FindFProperty<FStructProperty>(ContextObject->GetClass(), TEXT("ChooserContext"))
		: nullptr;
	FStructProperty* AttackProperty = FindFProperty<FStructProperty>(ContextStruct, TEXT("AttackRowData"));
	if (!ContextProperty || !AttackProperty || AttackProperty->Struct != AttackStruct)
	{
		return false;
	}
	const void* ContextMemory = ContextProperty->ContainerPtrToValuePtr<void>(ContextObject);
	const void* AttackMemory = AttackProperty->ContainerPtrToValuePtr<void>(ContextMemory);
	FStructOnScope DefaultAttack(AttackStruct);
	return AttackStruct->CompareScriptStruct(AttackMemory, DefaultAttack.GetStructMemory(), 0);
}

static float GetAttackDamage(const FInstancedStruct& AttackValue, UScriptStruct* AttackStruct)
{
	FFloatProperty* DamageProperty = FindFProperty<FFloatProperty>(AttackStruct, TEXT("Damage"));
	return DamageProperty && AttackValue.GetScriptStruct() == AttackStruct
		? DamageProperty->GetPropertyValue(DamageProperty->ContainerPtrToValuePtr<void>(AttackValue.GetMemory()))
		: -1.0f;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserNameOutputStructTest,
	"Monolith.Chooser.Authoring.NameAndOutputStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserNameOutputStructTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithChooserNameOutputStructTest;

	if (!GEditor)
	{
		AddError(TEXT("GEditor is required for Chooser authoring integration coverage"));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TablePath = FString::Printf(TEXT("/Game/Tests/Monolith/ChooserNameOutputStructTest_%s"), *Suffix);
	const FString OutputPath = FString::Printf(TEXT("/Game/Tests/Monolith/ChooserOutputAsset_%s"), *Suffix);

	UObject* OutputAsset = nullptr;
	UPackage* OutputPackage = nullptr;
	UPackage* TablePackage = nullptr;
	UChooserTable* Table = nullptr;
	bool bCleanupComplete = false;
	ON_SCOPE_EXIT
	{
		if (!bCleanupComplete)
		{
			const FMonolithActionResult CleanupResult = CleanupAssets(TablePath, OutputPath);
			if (!CleanupHandledBothAssets(CleanupResult, true))
			{
				AddError(TEXT("Fallback synthetic-asset cleanup did not handle both exact paths"));
			}
		}
	};

	OutputAsset = CreateTransientAsset(OutputPath);
	if (!TestNotNull(TEXT("Transient output asset created"), OutputAsset))
	{
		return false;
	}
	OutputPackage = OutputAsset->GetOutermost();
	FAssetRegistryModule::AssetCreated(OutputAsset);

	TSharedPtr<FJsonObject> CreateParams = Params(*TablePath);
	CreateParams->SetStringField(TEXT("output_class"), UAnimMontage::StaticClass()->GetPathName());
	CreateParams->SetStringField(TEXT("context_class"), ContextClassPath);
	FMonolithActionResult Result = Execute(TEXT("create_chooser_table"), CreateParams);
	if (!TestTrue(TEXT("Chooser table created"), Result.bSuccess))
	{
		return false;
	}

	const int32 TableSlash = TablePath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	TablePackage = FindPackage(nullptr, *TablePath);
	Table = TablePackage
		? FindObject<UChooserTable>(TablePackage, *TablePath.RightChop(TableSlash + 1))
		: nullptr;
	UScriptStruct* Context = ContextStruct();
	UScriptStruct* OutputStruct = AttackStruct();
	UClass* ContextClass = LoadObject<UClass>(nullptr, ContextClassPath);
	if (!TestNotNull(TEXT("Created chooser table resolved"), Table)
		|| !TestNotNull(TEXT("Plugin Chooser context struct resolved"), Context)
		|| !TestNotNull(TEXT("Plugin attack output struct resolved"), OutputStruct)
		|| !TestNotNull(TEXT("Reflected Chooser context class resolved"), ContextClass))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Table);
	FStructProperty* InheritedContextProperty = FindFProperty<FStructProperty>(ContextClass, TEXT("ChooserContext"));
	if (!TestTrue(TEXT("Plugin context exposes the native ChooserContext property"),
		InheritedContextProperty && InheritedContextProperty->Struct == Context))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddNameParams = Params(*TablePath);
	AddNameParams->SetStringField(TEXT("column_kind"), TEXT("Name"));
	AddNameParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.MovesetId"));
	Result = Execute(TEXT("add_chooser_column"), AddNameParams);
	if (!TestTrue(TEXT("Name column added"), Result.bSuccess))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AttackValue = MakeShared<FJsonObject>();
	AttackValue->SetNumberField(TEXT("Damage"), 25.0);
	AttackValue->SetNumberField(TEXT("StaminaCost"), 4.0);

	TSharedPtr<FJsonObject> MissingColumnRow = Params(*TablePath);
	TArray<TSharedPtr<FJsonValue>> MissingColumnCells;
	MissingColumnCells.Add(MakeShared<FJsonValueString>(TEXT("ProfileA")));
	MissingColumnRow->SetArrayField(TEXT("cells"), MissingColumnCells);
	MissingColumnRow->SetStringField(TEXT("output_psd"), OutputPath);
	MissingColumnRow->SetObjectField(TEXT("output_struct"), AttackValue);
	Result = Execute(TEXT("add_chooser_row"), MissingColumnRow);
	if (!TestTrue(TEXT("OutputStruct value without a column is rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Rejected row leaves result count unchanged"), Table->ResultsStructs.Num(), 0))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidStructParams = Params(*TablePath);
	InvalidStructParams->SetStringField(TEXT("column_kind"), TEXT("OutputStruct"));
	InvalidStructParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.AttackRowData"));
	InvalidStructParams->SetStringField(TEXT("struct_type"), TEXT("/Script/MonolithAnimation.DoesNotExist"));
	Result = Execute(TEXT("add_chooser_column"), InvalidStructParams);
	if (!TestTrue(TEXT("Invalid OutputStruct path is rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Invalid struct path leaves columns unchanged"), Table->ColumnsStructs.Num(), 1))
	{
		return false;
	}

	TSharedPtr<FJsonObject> IncompatibleBindingParams = Params(*TablePath);
	IncompatibleBindingParams->SetStringField(TEXT("column_kind"), TEXT("OutputStruct"));
	IncompatibleBindingParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.MovesetId"));
	IncompatibleBindingParams->SetStringField(TEXT("struct_type"), OutputStruct->GetPathName());
	Result = Execute(TEXT("add_chooser_column"), IncompatibleBindingParams);
	if (!TestTrue(TEXT("Incompatible OutputStruct binding is rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Invalid binding leaves columns unchanged"), Table->ColumnsStructs.Num(), 1))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddStructParams = Params(*TablePath);
	AddStructParams->SetStringField(TEXT("column_kind"), TEXT("OutputStruct"));
	AddStructParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.AttackRowData"));
	AddStructParams->SetStringField(TEXT("struct_type"), OutputStruct->GetPathName());
	Result = Execute(TEXT("add_chooser_column"), AddStructParams);
	if (!TestTrue(TEXT("OutputStruct column added"), Result.bSuccess))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddRowParams = Params(*TablePath);
	TArray<TSharedPtr<FJsonValue>> Cells;
	Cells.Add(MakeShared<FJsonValueString>(TEXT("ProfileA")));
	AddRowParams->SetArrayField(TEXT("cells"), Cells);
	AddRowParams->SetStringField(TEXT("output_psd"), OutputPath);
	AddRowParams->SetObjectField(TEXT("output_struct"), AttackValue);
	Result = Execute(TEXT("add_chooser_row"), AddRowParams);
	if (!TestTrue(TEXT("Name and OutputStruct row added"), Result.bSuccess))
	{
		return false;
	}
	if (!TestTrue(TEXT("Expected Name and OutputStruct columns exist"),
		Table->ColumnsStructs.IsValidIndex(0) && Table->ColumnsStructs.IsValidIndex(1)))
	{
		return false;
	}
	const FOutputStructColumn* OutputColumnBeforeFailure = Table->ColumnsStructs[1].GetPtr<FOutputStructColumn>();
	const int32 OutputRowsBeforeFailure = OutputColumnBeforeFailure ? OutputColumnBeforeFailure->RowValues.Num() : INDEX_NONE;
	TSharedPtr<FJsonObject> MissingValueRow = Params(*TablePath);
	MissingValueRow->SetArrayField(TEXT("cells"), Cells);
	MissingValueRow->SetStringField(TEXT("output_psd"), OutputPath);
	Result = Execute(TEXT("add_chooser_row"), MissingValueRow);
	const FOutputStructColumn* OutputColumnAfterFailure = Table->ColumnsStructs[1].GetPtr<FOutputStructColumn>();
	if (!TestTrue(TEXT("Missing OutputStruct row value is rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Failed append leaves result count unchanged"), Table->ResultsStructs.Num(), 1)
		|| !TestEqual(TEXT("Failed append leaves OutputStruct rows unchanged"),
			OutputColumnAfterFailure ? OutputColumnAfterFailure->RowValues.Num() : INDEX_NONE, OutputRowsBeforeFailure))
	{
		return false;
	}
	TSharedPtr<FJsonObject> RollbackRow = Params(*TablePath);
	TArray<TSharedPtr<FJsonValue>> InvalidCells;
	TSharedPtr<FJsonObject> InvalidNameCell = MakeShared<FJsonObject>();
	InvalidNameCell->SetStringField(TEXT("value"), TEXT("ProfileC"));
	InvalidNameCell->SetStringField(TEXT("comparison"), TEXT("InvalidComparison"));
	InvalidCells.Add(MakeShared<FJsonValueObject>(InvalidNameCell));
	RollbackRow->SetArrayField(TEXT("cells"), InvalidCells);
	RollbackRow->SetStringField(TEXT("output_psd"), OutputPath);
	RollbackRow->SetObjectField(TEXT("output_struct"), AttackValue);
	Result = Execute(TEXT("add_chooser_row"), RollbackRow);
	const FChooserNameColumn* NameColumnAfterRollback = Table->ColumnsStructs[0].GetPtr<FChooserNameColumn>();
	const FOutputStructColumn* OutputColumnAfterRollback = Table->ColumnsStructs[1].GetPtr<FOutputStructColumn>();
	if (!TestTrue(TEXT("Invalid input cell triggers append rollback"), !Result.bSuccess)
		|| !TestEqual(TEXT("Rollback restores result count"), Table->ResultsStructs.Num(), 1)
		|| !TestEqual(TEXT("Rollback restores disabled-row count"), Table->DisabledRows.Num(), 1)
		|| !TestEqual(TEXT("Rollback restores Name rows"),
			NameColumnAfterRollback ? NameColumnAfterRollback->RowValues.Num() : INDEX_NONE, 1)
		|| !TestEqual(TEXT("Rollback restores OutputStruct rows"),
			OutputColumnAfterRollback ? OutputColumnAfterRollback->RowValues.Num() : INDEX_NONE, 1))
	{
		return false;
	}

	TSharedPtr<FJsonObject> SetNameParams = Params(*TablePath);
	SetNameParams->SetNumberField(TEXT("column_index"), 0);
	SetNameParams->SetNumberField(TEXT("row_index"), 0);
	SetNameParams->SetStringField(TEXT("name_value"), TEXT("ProfileB"));
	SetNameParams->SetStringField(TEXT("comparison"), TEXT("MatchEqual"));
	Result = Execute(TEXT("set_chooser_cell"), SetNameParams);
	if (!TestTrue(TEXT("Name cell updated"), Result.bSuccess))
	{
		return false;
	}
	SetNameParams->SetStringField(TEXT("comparison"), TEXT("InvalidComparison"));
	Result = Execute(TEXT("set_chooser_cell"), SetNameParams);
	if (!TestTrue(TEXT("Invalid Name comparison is rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Invalid Name comparison preserves the row value"),
			NameColumnAfterRollback->RowValues[0].Value, FName(TEXT("ProfileB")))
		|| !TestTrue(TEXT("Invalid Name comparison preserves the row comparison"),
			NameColumnAfterRollback->RowValues[0].Comparison == ENameColumnCellValueComparison::MatchEqual))
	{
		return false;
	}
	SetNameParams->SetStringField(TEXT("comparison"), TEXT("MatchEqual"));

	TSharedPtr<FJsonObject> SetStructParams = Params(*TablePath);
	SetStructParams->SetNumberField(TEXT("column_index"), 1);
	SetStructParams->SetNumberField(TEXT("row_index"), 0);
	TSharedPtr<FJsonObject> UpdatedAttack = MakeShared<FJsonObject>();
	UpdatedAttack->SetNumberField(TEXT("Damage"), 40.0);
	UpdatedAttack->SetNumberField(TEXT("StaminaCost"), 6.0);
	SetStructParams->SetObjectField(TEXT("struct_value"), UpdatedAttack);
	Result = Execute(TEXT("set_chooser_cell"), SetStructParams);
	if (!TestTrue(TEXT("OutputStruct cell updated"), Result.bSuccess))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidField = MakeShared<FJsonObject>();
	InvalidField->SetNumberField(TEXT("DoesNotExist"), 1.0);
	SetStructParams->SetObjectField(TEXT("struct_value"), InvalidField);
	Result = Execute(TEXT("set_chooser_cell"), SetStructParams);
	if (!TestTrue(TEXT("Unknown OutputStruct field is rejected"), !Result.bSuccess)
		|| !TestTrue(TEXT("Unknown OutputStruct field preserves the row value"),
			FMath::IsNearlyEqual(GetAttackDamage(OutputColumnAfterRollback->RowValues[0], OutputStruct), 40.0f)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidType = MakeShared<FJsonObject>();
	InvalidType->SetStringField(TEXT("Damage"), TEXT("not-a-number"));
	SetStructParams->SetObjectField(TEXT("struct_value"), InvalidType);
	Result = Execute(TEXT("set_chooser_cell"), SetStructParams);
	if (!TestTrue(TEXT("Invalid OutputStruct type is rejected"), !Result.bSuccess)
		|| !TestTrue(TEXT("Invalid OutputStruct type preserves the row value"),
			FMath::IsNearlyEqual(GetAttackDamage(OutputColumnAfterRollback->RowValues[0], OutputStruct), 40.0f)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InvalidRange = MakeShared<FJsonObject>();
	InvalidRange->SetNumberField(TEXT("Damage"), -1.0);
	SetStructParams->SetObjectField(TEXT("struct_value"), InvalidRange);
	Result = Execute(TEXT("set_chooser_cell"), SetStructParams);
	if (!TestTrue(TEXT("Invalid OutputStruct range is rejected"), !Result.bSuccess)
		|| !TestTrue(TEXT("Invalid OutputStruct range preserves the row value"),
			FMath::IsNearlyEqual(GetAttackDamage(OutputColumnAfterRollback->RowValues[0], OutputStruct), 40.0f)))
	{
		return false;
	}

	SetNameParams->SetNumberField(TEXT("column_index"), 0.5);
	Result = Execute(TEXT("set_chooser_cell"), SetNameParams);
	if (!TestTrue(TEXT("Fractional chooser index is rejected"), !Result.bSuccess))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddOutputObjectParams = Params(*TablePath);
	AddOutputObjectParams->SetStringField(TEXT("column_kind"), TEXT("OutputObject"));
	Result = Execute(TEXT("add_chooser_column"), AddOutputObjectParams);
	if (!TestTrue(TEXT("Existing OutputObject column authoring remains supported"), Result.bSuccess)
		|| !TestTrue(TEXT("OutputObject column was appended"), !Table->ColumnsStructs.IsEmpty()))
	{
		return false;
	}
	FOutputObjectColumn* OutputObjectColumn = Table->ColumnsStructs.Last().GetMutablePtr<FOutputObjectColumn>();
	FObjectContextProperty* OutputObjectBinding = OutputObjectColumn
		? OutputObjectColumn->InputValue.GetMutablePtr<FObjectContextProperty>()
		: nullptr;
	if (!TestTrue(TEXT("OutputObject rows stay aligned"),
			OutputObjectColumn && OutputObjectColumn->RowValues.Num() == Table->ResultsStructs.Num()
			&& OutputObjectColumn->RowValues.IsValidIndex(0))
		|| !TestNotNull(TEXT("OutputObject context binding exists"), OutputObjectBinding))
	{
		return false;
	}
	OutputObjectBinding->Binding.PropertyBindingChain = {TEXT("AirAttackMontage")};
	OutputObjectColumn->RowValues[0].Value = FInstancedStruct::Make<FAssetChooser>();
	OutputObjectColumn->RowValues[0].Value.GetMutable<FAssetChooser>().Asset = OutputAsset;

	if (!TestTrue(TEXT("OutputStruct column remains available for nested coverage"), Table->ColumnsStructs.IsValidIndex(1))
		|| !TestTrue(TEXT("Result row remains available for nested coverage"), Table->ResultsStructs.IsValidIndex(0)))
	{
		return false;
	}
	UChooserTable* NestedChild = NewObject<UChooserTable>(
		Table, UChooserTable::StaticClass(), TEXT("NestedOutputStruct"), RF_Transactional);
	NestedChild->ResultsStructs.Add(Table->ResultsStructs[0]);
	NestedChild->DisabledRows.Add(false);
	NestedChild->RootChooser = Table;
	Table->AddNestedObject(NestedChild);
	NestedChild->ColumnsStructs.Add(Table->ColumnsStructs[1]);
	FChooserColumnBase* ChildColumn = NestedChild->ColumnsStructs[0].GetMutablePtr<FChooserColumnBase>();
	FChooserColumnBase* SourceColumn = Table->ColumnsStructs[1].GetMutablePtr<FChooserColumnBase>();
	if (!TestNotNull(TEXT("Embedded child column base resolves"), ChildColumn)
		|| !TestNotNull(TEXT("Root OutputStruct column base resolves"), SourceColumn))
	{
		return false;
	}
	ChildColumn->Initialize(NestedChild);
	ChildColumn->SetNumRows(0);
	ChildColumn->SetNumRows(1);
	ChildColumn->CopyRow(*SourceColumn, 0, 0);
	const FOutputStructColumn* ChildOutputColumn = NestedChild->ColumnsStructs[0].GetPtr<FOutputStructColumn>();
	if (!TestTrue(TEXT("Embedded child stores one typed OutputStruct row before save"),
		ChildOutputColumn && ChildOutputColumn->RowValues.IsValidIndex(0)
		&& ChildOutputColumn->RowValues[0].GetScriptStruct() == OutputStruct))
	{
		return false;
	}

	Table->Compile(true);
	if (!TestTrue(TEXT("Synthetic montage saves to disk"), SaveAsset(OutputAsset))
		|| !TestTrue(TEXT("Synthetic chooser saves to disk"), SaveAsset(Table)))
	{
		return false;
	}

	TArray<UPackage*> PackagesToUnload{TablePackage, OutputPackage};
	Table = nullptr;
	OutputAsset = nullptr;
	FText UnloadError;
	if (!TestTrue(TEXT("Synthetic packages unload after save"),
		UPackageTools::UnloadPackages(PackagesToUnload, UnloadError, false)))
	{
		AddError(UnloadError.ToString());
		return false;
	}
	TablePackage = nullptr;
	OutputPackage = nullptr;

	TablePackage = LoadPackage(nullptr, *TablePath, LOAD_None);
	OutputPackage = LoadPackage(nullptr, *OutputPath, LOAD_None);
	Table = TablePackage
		? FindObject<UChooserTable>(TablePackage, *TablePath.RightChop(TableSlash + 1))
		: nullptr;
	OutputAsset = OutputPackage
		? FindObject<UAnimMontage>(OutputPackage, *OutputPath.RightChop(OutputPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1))
		: nullptr;
	if (!TestNotNull(TEXT("Chooser reloads from disk"), Table)
		|| !TestNotNull(TEXT("Primary montage reloads from disk"), OutputAsset))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InspectParams = Params(*TablePath);
	InspectParams->SetBoolField(TEXT("include_cells"), true);
	InspectParams->SetBoolField(TEXT("recursive"), true);
	Result = Execute(TEXT("inspect_chooser"), InspectParams);
	if (!TestTrue(TEXT("Chooser inspection succeeded"), Result.bSuccess))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ContextData = nullptr;
	const TSharedPtr<FJsonObject>* ContextEntry = nullptr;
	if (!TestTrue(TEXT("Live plugin context class read back"), Result.Result.IsValid()
		&& Result.Result->TryGetArrayField(TEXT("context_data"), ContextData)
		&& ContextData && ContextData->Num() == 1
		&& (*ContextData)[0]->TryGetObject(ContextEntry)
		&& ContextEntry && ContextEntry->IsValid()
		&& (*ContextEntry)->GetStringField(TEXT("class")) == ContextClassPath))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
	if (!TestTrue(TEXT("Inspection returned columns"), Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("columns"), Columns) && Columns && Columns->Num() == 3))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* NameColumn = nullptr;
	const TSharedPtr<FJsonObject>* StructColumn = nullptr;
	if (!TestTrue(TEXT("Name column read back"), (*Columns)[0]->TryGetObject(NameColumn) && NameColumn && NameColumn->IsValid()))
	{
		return false;
	}
	if (!TestTrue(TEXT("OutputStruct column read back"), (*Columns)[1]->TryGetObject(StructColumn) && StructColumn && StructColumn->IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* NameBinding = nullptr;
	const TSharedPtr<FJsonObject>* StructBinding = nullptr;
	if (!TestTrue(TEXT("Nested Name binding survives reload"),
		(*NameColumn)->TryGetObjectField(TEXT("input_binding"), NameBinding)
		&& NameBinding && NameBinding->IsValid()
		&& (*NameBinding)->GetStringField(TEXT("display")) == TEXT("ChooserContext.MovesetId"))
		|| !TestTrue(TEXT("Nested OutputStruct binding survives reload"),
			(*StructColumn)->TryGetObjectField(TEXT("input_binding"), StructBinding)
			&& StructBinding && StructBinding->IsValid()
			&& (*StructBinding)->GetStringField(TEXT("display")) == TEXT("ChooserContext.AttackRowData")))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NameCells = nullptr;
	if (!TestTrue(TEXT("Name cells read back"), (*NameColumn)->TryGetArrayField(TEXT("cells"), NameCells) && NameCells && NameCells->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* NameCell = nullptr;
	if (!TestTrue(TEXT("Name cell is an object"), (*NameCells)[0]->TryGetObject(NameCell) && NameCell && NameCell->IsValid()))
	{
		return false;
	}
	if (!TestEqual(TEXT("Name value round-trips"), (*NameCell)->GetStringField(TEXT("value")), FString(TEXT("ProfileB")))
		|| !TestEqual(TEXT("Name comparison round-trips"), (*NameCell)->GetStringField(TEXT("comparison")), FString(TEXT("MatchEqual"))))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* StructCells = nullptr;
	if (!TestTrue(TEXT("OutputStruct cells read back"), (*StructColumn)->TryGetArrayField(TEXT("cells"), StructCells) && StructCells && StructCells->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* StructCell = nullptr;
	const TSharedPtr<FJsonObject>* StructValue = nullptr;
	if (!TestTrue(TEXT("OutputStruct cell is an object"),
		(*StructCells)[0]->TryGetObject(StructCell) && StructCell && StructCell->IsValid())
		|| !TestTrue(TEXT("OutputStruct value is an object"),
			(*StructCell)->TryGetObjectField(TEXT("value"), StructValue) && StructValue && StructValue->IsValid()))
	{
		return false;
	}
	const bool bStructRoundTrips = StructValue && StructValue->IsValid()
		&& (*StructValue)->GetStringField(TEXT("__struct")) == OutputStruct->GetPathName()
		&& FMath::IsNearlyEqual(static_cast<float>((*StructValue)->GetNumberField(TEXT("Damage"))), 40.0f)
		&& FMath::IsNearlyEqual(static_cast<float>((*StructValue)->GetNumberField(TEXT("StaminaCost"))), 6.0f)
		&& (*StructValue)->GetStringField(TEXT("MovementPathOffsetCurve")).IsEmpty();
	if (!TestTrue(TEXT("OutputStruct reflected value round-trips"), bStructRoundTrips))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Tree = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RecursiveColumns = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RecursiveOutputColumns = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ChildTables = nullptr;
	const bool bRecursiveReadback = Result.Result->TryGetObjectField(TEXT("tree"), Tree)
		&& Tree && Tree->IsValid()
		&& (*Tree)->TryGetArrayField(TEXT("output_struct_columns"), RecursiveColumns)
		&& RecursiveColumns && RecursiveColumns->Num() == 1
		&& (*Tree)->TryGetArrayField(TEXT("output_columns"), RecursiveOutputColumns)
		&& RecursiveOutputColumns && RecursiveOutputColumns->Num() == 1
		&& (*Tree)->TryGetArrayField(TEXT("child_tables"), ChildTables)
		&& ChildTables && ChildTables->Num() == 1;
	if (!TestTrue(TEXT("Recursive OutputStruct readback is present"), bRecursiveReadback))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RecursiveOutputColumn = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RecursiveOutputCells = nullptr;
	const TSharedPtr<FJsonObject>* RecursiveOutputCell = nullptr;
	if (!TestTrue(TEXT("Recursive OutputObject column is an object"),
		(*RecursiveOutputColumns)[0]->TryGetObject(RecursiveOutputColumn)
		&& RecursiveOutputColumn && RecursiveOutputColumn->IsValid())
		|| !TestTrue(TEXT("Recursive OutputObject row is present"),
			(*RecursiveOutputColumn)->TryGetArrayField(TEXT("cells"), RecursiveOutputCells)
			&& RecursiveOutputCells && RecursiveOutputCells->Num() == 1
			&& (*RecursiveOutputCells)[0]->TryGetObject(RecursiveOutputCell)
			&& RecursiveOutputCell && RecursiveOutputCell->IsValid()
			&& (*RecursiveOutputCell)->GetStringField(TEXT("asset")) == OutputAsset->GetPathName()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ChildTree = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ChildOutputStructColumns = nullptr;
	const TSharedPtr<FJsonObject>* ChildOutputStructColumn = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ChildOutputStructCells = nullptr;
	const TSharedPtr<FJsonObject>* ChildOutputStructCell = nullptr;
	const TSharedPtr<FJsonObject>* ChildOutputStructValue = nullptr;
	if (!TestTrue(TEXT("Embedded child tree is an object"),
		(*ChildTables)[0]->TryGetObject(ChildTree) && ChildTree && ChildTree->IsValid())
		|| !TestTrue(TEXT("Embedded child recursive OutputStruct readback is present"),
			(*ChildTree)->TryGetArrayField(TEXT("output_struct_columns"), ChildOutputStructColumns)
			&& ChildOutputStructColumns && ChildOutputStructColumns->Num() == 1)
		|| !TestTrue(TEXT("Embedded child OutputStruct column is an object"),
			(*ChildOutputStructColumns)[0]->TryGetObject(ChildOutputStructColumn)
			&& ChildOutputStructColumn && ChildOutputStructColumn->IsValid())
		|| !TestTrue(TEXT("Embedded child OutputStruct cell is present"),
			(*ChildOutputStructColumn)->TryGetArrayField(TEXT("cells"), ChildOutputStructCells)
			&& ChildOutputStructCells && !ChildOutputStructCells->IsEmpty()))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& CellValue : *ChildOutputStructCells)
	{
		const TSharedPtr<FJsonObject>* Candidate = nullptr;
		if (CellValue->TryGetObject(Candidate) && Candidate && Candidate->IsValid()
			&& (*Candidate)->GetStringField(TEXT("site")) == TEXT("row"))
		{
			ChildOutputStructCell = Candidate;
			break;
		}
	}
	if (!TestTrue(TEXT("Embedded child authored OutputStruct row is present"),
		ChildOutputStructCell && ChildOutputStructCell->IsValid()
			&& (*ChildOutputStructCell)->TryGetObjectField(TEXT("value"), ChildOutputStructValue)
			&& ChildOutputStructValue && ChildOutputStructValue->IsValid()
			&& (*ChildOutputStructValue)->GetStringField(TEXT("__struct")) == OutputStruct->GetPathName()
			&& FMath::IsNearlyEqual(static_cast<float>((*ChildOutputStructValue)->GetNumberField(TEXT("Damage"))), 40.0f)))
	{
		return false;
	}

	UObject* ContextObject = NewObject<UObject>(GetTransientPackage(), ContextClass);
	if (!TestNotNull(TEXT("Concrete plugin Chooser context instantiates"), ContextObject)
		|| !TestTrue(TEXT("Matching context resets before evaluation"),
			SetContextProfileAndResetOutput(ContextObject, Context, OutputStruct, TEXT("ProfileB"))))
	{
		return false;
	}
	UObject* MatchingResult = UChooserFunctionLibrary::EvaluateChooser(ContextObject, Table, UAnimMontage::StaticClass());
	FObjectProperty* AirAttackMontageProperty = FindFProperty<FObjectProperty>(ContextClass, TEXT("AirAttackMontage"));
	if (!TestEqual(TEXT("Matching Name row returns the saved montage"), MatchingResult, OutputAsset)
		|| !TestTrue(TEXT("Matching row writes the reflected attack output"),
			FMath::IsNearlyEqual(GetContextDamage(ContextObject, Context, OutputStruct), 40.0f))
		|| !TestTrue(TEXT("Existing OutputObject value evaluates through its context binding"),
			AirAttackMontageProperty
			&& AirAttackMontageProperty->GetObjectPropertyValue_InContainer(ContextObject) == OutputAsset))
	{
		return false;
	}

	if (!TestTrue(TEXT("Missing context resets before evaluation"),
		SetContextProfileAndResetOutput(ContextObject, Context, OutputStruct, TEXT("MissingProfile"))))
	{
		return false;
	}
	UObject* MissingResult = UChooserFunctionLibrary::EvaluateChooser(ContextObject, Table, UAnimMontage::StaticClass());
	if (!TestNull(TEXT("Missing Name row returns no montage"), MissingResult)
		|| !TestTrue(TEXT("Missing row leaves the exact default attack output"),
			IsContextAttackDefault(ContextObject, Context, OutputStruct)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddEnumParams = Params(*TablePath);
	AddEnumParams->SetStringField(TEXT("column_kind"), TEXT("Enum"));
	AddEnumParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.AttackVariant"));
	AddEnumParams->SetStringField(TEXT("enum_class"), StaticEnum<EMonolithChooserTestAttackVariant>()->GetPathName());
	Result = Execute(TEXT("add_chooser_column"), AddEnumParams);
	if (!TestTrue(TEXT("Enum regression column added"), Result.bSuccess) || Table->ColumnsStructs.IsEmpty())
	{
		return false;
	}
	const int32 EnumColumnIndex = Table->ColumnsStructs.Num() - 1;
	FEnumColumn* EnumColumn = Table->ColumnsStructs[EnumColumnIndex].GetMutablePtr<FEnumColumn>();
	if (!TestTrue(TEXT("Enum regression row is aligned"), EnumColumn && EnumColumn->RowValues.IsValidIndex(0)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> SetEnumParams = Params(*TablePath);
	SetEnumParams->SetNumberField(TEXT("column_index"), EnumColumnIndex);
	SetEnumParams->SetNumberField(TEXT("row_index"), 0);
	SetEnumParams->SetNumberField(TEXT("enum_value"), 1);
	SetEnumParams->SetStringField(TEXT("comparison"), TEXT("MatchEqual"));
	Result = Execute(TEXT("set_chooser_cell"), SetEnumParams);
	if (!TestTrue(TEXT("Enum regression cell updated"), Result.bSuccess))
	{
		return false;
	}
	SetEnumParams->SetNumberField(TEXT("enum_value"), 0);
	SetEnumParams->SetStringField(TEXT("comparison"), TEXT("InvalidComparison"));
	Result = Execute(TEXT("set_chooser_cell"), SetEnumParams);
	if (!TestTrue(TEXT("Invalid Enum comparison is rejected"), !Result.bSuccess)
		|| !TestTrue(TEXT("Invalid Enum comparison preserves value and comparison"),
			EnumColumn->RowValues[0].Value == 1
			&& EnumColumn->RowValues[0].Comparison == EEnumColumnCellValueComparison::MatchEqual))
	{
		return false;
	}
	SetEnumParams->SetNumberField(TEXT("enum_value"), 0.5);
	SetEnumParams->SetStringField(TEXT("comparison"), TEXT("MatchEqual"));
	Result = Execute(TEXT("set_chooser_cell"), SetEnumParams);
	if (!TestTrue(TEXT("Fractional Enum value is rejected"), !Result.bSuccess)
		|| !TestTrue(TEXT("Fractional Enum value preserves value and comparison"),
			EnumColumn->RowValues[0].Value == 1
			&& EnumColumn->RowValues[0].Comparison == EEnumColumnCellValueComparison::MatchEqual))
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddRangeParams = Params(*TablePath);
	AddRangeParams->SetStringField(TEXT("column_kind"), TEXT("FloatRange"));
	AddRangeParams->SetStringField(TEXT("binding_property"), TEXT("ChooserContext.AttackIndexValue"));
	Result = Execute(TEXT("add_chooser_column"), AddRangeParams);
	if (!TestTrue(TEXT("FloatRange regression column added"), Result.bSuccess) || Table->ColumnsStructs.IsEmpty())
	{
		return false;
	}
	const int32 RangeColumnIndex = Table->ColumnsStructs.Num() - 1;
	FFloatRangeColumn* RangeColumn = Table->ColumnsStructs[RangeColumnIndex].GetMutablePtr<FFloatRangeColumn>();
	if (!TestTrue(TEXT("FloatRange regression row is aligned"), RangeColumn && RangeColumn->RowValues.IsValidIndex(0)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> InvalidRangeCell = MakeShared<FJsonObject>();
	InvalidRangeCell->SetNumberField(TEXT("min"), 0.0);
	TArray<TSharedPtr<FJsonValue>> InvalidRangeCells;
	InvalidRangeCells.Add(MakeShared<FJsonValueString>(TEXT("ProfileC")));
	InvalidRangeCells.Add(MakeShared<FJsonValueNumber>(0.0));
	InvalidRangeCells.Add(MakeShared<FJsonValueObject>(InvalidRangeCell));
	TSharedPtr<FJsonObject> InvalidRangeRow = Params(*TablePath);
	InvalidRangeRow->SetArrayField(TEXT("cells"), InvalidRangeCells);
	InvalidRangeRow->SetStringField(TEXT("output_psd"), OutputPath);
	InvalidRangeRow->SetObjectField(TEXT("output_struct"), AttackValue);
	const int32 ResultsBeforeRangeFailure = Table->ResultsStructs.Num();
	Result = Execute(TEXT("add_chooser_row"), InvalidRangeRow);
	if (!TestTrue(TEXT("FloatRange row requires both numeric bounds"), !Result.bSuccess)
		|| !TestEqual(TEXT("Invalid FloatRange append restores results"), Table->ResultsStructs.Num(), ResultsBeforeRangeFailure)
		|| !TestEqual(TEXT("Invalid FloatRange append restores range rows"), RangeColumn->RowValues.Num(), ResultsBeforeRangeFailure))
	{
		return false;
	}

	Result = Execute(TEXT("add_chooser_column"), AddStructParams);
	if (!TestTrue(TEXT("A second OutputStruct column can be represented"), Result.bSuccess))
	{
		return false;
	}
	TSharedPtr<FJsonObject> ValidRangeCell = MakeShared<FJsonObject>();
	ValidRangeCell->SetNumberField(TEXT("min"), 0.0);
	ValidRangeCell->SetNumberField(TEXT("max"), 0.0);
	TArray<TSharedPtr<FJsonValue>> MultipleOutputCells;
	MultipleOutputCells.Add(MakeShared<FJsonValueString>(TEXT("ProfileC")));
	MultipleOutputCells.Add(MakeShared<FJsonValueNumber>(0.0));
	MultipleOutputCells.Add(MakeShared<FJsonValueObject>(ValidRangeCell));
	AddRowParams->SetArrayField(TEXT("cells"), MultipleOutputCells);
	const int32 ResultsBeforeMultipleFailure = Table->ResultsStructs.Num();
	Result = Execute(TEXT("add_chooser_row"), AddRowParams);
	if (!TestTrue(TEXT("Rows with multiple OutputStruct columns are rejected"), !Result.bSuccess)
		|| !TestEqual(TEXT("Multiple-column rejection leaves results unchanged"),
			Table->ResultsStructs.Num(), ResultsBeforeMultipleFailure))
	{
		return false;
	}

	ContextObject = nullptr;
	Result = CleanupAssets(TablePath, OutputPath);
	bCleanupComplete = CleanupHandledBothAssets(Result, false);
	Table = nullptr;
	OutputAsset = nullptr;
	TablePackage = nullptr;
	OutputPackage = nullptr;
	const FString TableFilename = FPackageName::LongPackageNameToFilename(TablePath, FPackageName::GetAssetPackageExtension());
	const FString OutputFilename = FPackageName::LongPackageNameToFilename(OutputPath, FPackageName::GetAssetPackageExtension());
	UPackage* ResidualTablePackage = FindPackage(nullptr, *TablePath);
	UPackage* ResidualOutputPackage = FindPackage(nullptr, *OutputPath);
	bool bDirtySyntheticPackage = false;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->IsDirty() && (It->GetName() == TablePath || It->GetName() == OutputPath))
		{
			bDirtySyntheticPackage = true;
			break;
		}
	}
	return TestTrue(TEXT("Synthetic assets are deleted through the allowlisted cleanup action"), bCleanupComplete)
		&& TestFalse(TEXT("Synthetic chooser leaves no asset-registry residue"), UEditorAssetLibrary::DoesAssetExist(TablePath))
		&& TestFalse(TEXT("Synthetic montage leaves no asset-registry residue"), UEditorAssetLibrary::DoesAssetExist(OutputPath))
		&& TestFalse(TEXT("Synthetic chooser package remains loaded"), ResidualTablePackage != nullptr)
		&& TestFalse(TEXT("Synthetic montage package remains loaded"), ResidualOutputPackage != nullptr)
		&& TestFalse(TEXT("Synthetic assets leave no dirty package"), bDirtySyntheticPackage)
		&& TestFalse(TEXT("Synthetic chooser leaves no disk residue"), FPaths::FileExists(TableFilename))
		&& TestFalse(TEXT("Synthetic montage leaves no disk residue"), FPaths::FileExists(OutputFilename));
}

#else

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithChooserNameOutputStructTest,
	"Monolith.Chooser.Authoring.NameAndOutputStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithChooserNameOutputStructTest::RunTest(const FString& /*Parameters*/)
{
	AddError(TEXT("Chooser editor data is required for Name and OutputStruct integration coverage"));
	return false;
}

#endif
#endif
