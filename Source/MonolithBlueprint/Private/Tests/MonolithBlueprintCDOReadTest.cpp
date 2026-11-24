#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Editor/EditorPerformanceSettings.h"
#include "MonolithBlueprintCDOActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithBlueprintCDONativeScriptReadTest,
	"Monolith.Blueprint.CDORead.NativeScriptObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintCDONativeScriptReadTest::RunTest(const FString& /*Parameters*/)
{
	const FString AssetPath = TEXT("/Script/UnrealEd.Default__EditorPerformanceSettings");
	const UEditorPerformanceSettings* Settings = GetDefault<UEditorPerformanceSettings>();
	if (!TestNotNull(TEXT("Editor performance settings CDO exists"), Settings))
	{
		return false;
	}

	TestEqual(TEXT("fixture is the exact native CDO path"), Settings->GetPathName(), AssetPath);
	const bool ExpectedValue = Settings->bThrottleCPUWhenNotForeground;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	TArray<TSharedPtr<FJsonValue>> PropertyNames;
	PropertyNames.Add(MakeShared<FJsonValueString>(TEXT("bThrottleCPUWhenNotForeground")));
	Params->SetArrayField(TEXT("property_names"), PropertyNames);

	FAutomationTestExecutionInfo BeforeRead;
	GetExecutionInfo(BeforeRead);
	const int32 FirstNewEntry = BeforeRead.GetEntries().Num();

	const FMonolithActionResult Result = FMonolithBlueprintCDOActions::HandleGetCDOProperties(Params);

	FAutomationTestExecutionInfo AfterRead;
	GetExecutionInfo(AfterRead);
	bool bEmittedWarning = false;
	for (int32 Index = FirstNewEntry; Index < AfterRead.GetEntries().Num(); ++Index)
	{
		const FAutomationEvent& Event = AfterRead.GetEntries()[Index].Event;
		if (Event.Type == EAutomationEventType::Warning && Event.Context == TEXT("log"))
		{
			bEmittedWarning = true;
			break;
		}
	}
	TestFalse(TEXT("native CDO read emits no log warning"), bEmittedWarning);

	TestTrue(FString::Printf(TEXT("native CDO read succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess);
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	FString ActualAssetPath;
	TestTrue(TEXT("response includes asset_path"),
		Result.Result->TryGetStringField(TEXT("asset_path"), ActualAssetPath));
	TestEqual(TEXT("response preserves the exact native asset path"), ActualAssetPath, AssetPath);

	FString ActualClass;
	TestTrue(TEXT("response includes native_class"),
		Result.Result->TryGetStringField(TEXT("native_class"), ActualClass));
	TestEqual(TEXT("response reports the exact editor performance class"),
		ActualClass, UEditorPerformanceSettings::StaticClass()->GetName());

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (!TestTrue(TEXT("response includes the filtered properties array"),
		Result.Result->TryGetArrayField(TEXT("properties"), Properties) && Properties))
	{
		return false;
	}
	if (!TestEqual(TEXT("exact property filter returns one property"), Properties->Num(), 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Property = (*Properties)[0]->AsObject();
	if (!TestNotNull(TEXT("filtered property is an object"), Property.Get()))
	{
		return false;
	}

	FString PropertyName;
	TestTrue(TEXT("response includes the exact bool property name"),
		Property->TryGetStringField(TEXT("name"), PropertyName));
	TestEqual(TEXT("property name is bThrottleCPUWhenNotForeground"),
		PropertyName, FString(TEXT("bThrottleCPUWhenNotForeground")));

	const TSharedPtr<FJsonValue> Value = Property->TryGetField(TEXT("value"));
	if (!TestTrue(TEXT("property value is a JSON boolean"), Value.IsValid() && Value->Type == EJson::Boolean))
	{
		return false;
	}
	TestEqual(TEXT("JSON boolean equals the CDO's typed bool value"), Value->AsBool(), ExpectedValue);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
