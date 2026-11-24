// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "MonolithAssetUtils.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

namespace MonolithMeshBuildSettingsTests
{
	FMonolithActionResult Execute(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), Action, Params);
	}

	TSharedPtr<FJsonObject> MakeParams(const FString& AssetPath, double LodIndex = 0.0)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetNumberField(TEXT("lod_index"), LodIndex);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeSettings(bool bGenerateLightmapUVs = false,
		int32 SrcLightmapIndex = 0, int32 DstLightmapIndex = 1)
	{
		auto Settings = MakeShared<FJsonObject>();
		Settings->SetBoolField(TEXT("generate_lightmap_uvs"), bGenerateLightmapUVs);
		Settings->SetNumberField(TEXT("src_lightmap_index"), SrcLightmapIndex);
		Settings->SetNumberField(TEXT("dst_lightmap_index"), DstLightmapIndex);
		return Settings;
	}

	TSharedPtr<FJsonObject> MakeSetParams(const FString& AssetPath,
		const TSharedPtr<FJsonObject>& Settings, bool bPreview = false)
	{
		auto Params = MakeParams(AssetPath);
		Params->SetObjectField(TEXT("settings"), Settings);
		if (bPreview)
		{
			Params->SetBoolField(TEXT("preview"), true);
		}
		return Params;
	}

	struct FScopedBuildCounter
	{
		UStaticMesh* Mesh = nullptr;
		int32 Count = 0;
		FDelegateHandle Handle;

		explicit FScopedBuildCounter(UStaticMesh* InMesh)
			: Mesh(InMesh)
		{
			Handle = Mesh->OnPostMeshBuild().AddLambda([this](UStaticMesh* BuiltMesh)
			{
				if (BuiltMesh == Mesh)
				{
					++Count;
				}
			});
		}

		~FScopedBuildCounter()
		{
			if (Mesh && Handle.IsValid())
			{
				Mesh->OnPostMeshBuild().Remove(Handle);
			}
		}
	};

	struct FScopedMeshFixture
	{
		FString PackagePath;
		FString ObjectPath;
		UPackage* Package = nullptr;
		UStaticMesh* Mesh = nullptr;

		FScopedMeshFixture()
		{
			PackagePath = FString::Printf(TEXT("/Temp/MonolithMeshBuildSettings/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
		}

		~FScopedMeshFixture()
		{
			if (Mesh)
			{
				const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), Mesh->GetClass(),
					*FString::Printf(TEXT("MONOLITH_MESH_BUILD_SETTINGS_%s"), *Mesh->GetName()));
				Mesh->Rename(*TransientName.ToString(), GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
				Mesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
				Mesh->MarkAsGarbage();
				Mesh = nullptr;
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
				Package = nullptr;
			}
		}

		bool CreateFromAsset(const FString& SourceAssetPath)
		{
			UStaticMesh* SourceMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(SourceAssetPath);
			if (!SourceMesh)
			{
				return false;
			}

			Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				return false;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			Mesh = DuplicateObject<UStaticMesh>(SourceMesh, Package, *AssetName);
			if (!Mesh || Mesh->GetNumSourceModels() == 0)
			{
				return false;
			}

			Package->SetDirtyFlag(false);
			return true;
		}

		bool Create()
		{
			if (!CreateFromAsset(TEXT("/Engine/BasicShapes/Cube.Cube")))
			{
				return false;
			}
			FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
			Settings.bGenerateLightmapUVs = false;
			Settings.SrcLightmapIndex = 0;
			Settings.DstLightmapIndex = 1;
			Settings.MinLightmapResolution = 96;
			Settings.bRemoveDegenerates = false;
			Mesh->SetLightMapCoordinateIndex(2);
			Package->SetDirtyFlag(false);
			return true;
		}
	};

	struct FUVChannelMetrics
	{
		int32 Index = INDEX_NONE;
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;
		int32 Islands = 0;
		FString Overlap;
		double OverlapPercent = 0.0;
	};

	bool ReadUVChannel(const FMonolithActionResult& ActionResult, int32 ExpectedIndex,
		FUVChannelMetrics& OutMetrics)
	{
		if (!ActionResult.bSuccess || !ActionResult.Result.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (!ActionResult.Result->TryGetArrayField(TEXT("channels"), Channels) || !Channels || Channels->Num() != 1
			|| !(*Channels)[0].IsValid() || (*Channels)[0]->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Channel = (*Channels)[0]->AsObject();
		if (!Channel.IsValid())
		{
			return false;
		}

		double ChannelIndex = -1.0;
		double Islands = 0.0;
		if (!Channel->TryGetNumberField(TEXT("index"), ChannelIndex)
			|| !Channel->TryGetNumberField(TEXT("islands"), Islands)
			|| static_cast<int32>(ChannelIndex) != ExpectedIndex)
		{
			return false;
		}
		const TSharedPtr<FJsonValue>* BoundsValue = Channel->Values.Find(TEXT("bounds"));
		if (!BoundsValue || !BoundsValue->IsValid() || (*BoundsValue)->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Bounds = (*BoundsValue)->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* MinValues = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* MaxValues = nullptr;
		if (!Bounds.IsValid() || !Bounds->TryGetArrayField(TEXT("min"), MinValues)
			|| !Bounds->TryGetArrayField(TEXT("max"), MaxValues)
			|| !MinValues || !MaxValues || MinValues->Num() < 2 || MaxValues->Num() < 2)
		{
			return false;
		}

		OutMetrics.Index = ExpectedIndex;
		OutMetrics.Min = FVector2D((*MinValues)[0]->AsNumber(), (*MinValues)[1]->AsNumber());
		OutMetrics.Max = FVector2D((*MaxValues)[0]->AsNumber(), (*MaxValues)[1]->AsNumber());
		OutMetrics.Islands = static_cast<int32>(Islands);
		const TSharedPtr<FJsonValue>* OverlapValue = Channel->Values.Find(TEXT("overlap_pct"));
		if (!OverlapValue || !OverlapValue->IsValid() || (*OverlapValue)->Type != EJson::Number)
		{
			return false;
		}
		OutMetrics.OverlapPercent = (*OverlapValue)->AsNumber();
		OutMetrics.Overlap = FString::Printf(TEXT("%.1f%%"), OutMetrics.OverlapPercent);
		return true;
	}

	TSharedPtr<FJsonObject> MakeUVParams(const FString& AssetPath, int32 Channel)
	{
		auto Params = MakeParams(AssetPath);
		Params->SetNumberField(TEXT("uv_channel"), Channel);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBuildSettingsReadPreviewTest,
	"Monolith.Mesh.BuildSettings.ReadPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBuildSettingsReadPreviewTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable in-memory cube fixture created"), Fixture.Create()))
	{
		return false;
	}

	const FMeshBuildSettings Before = Fixture.Mesh->GetSourceModel(0).BuildSettings;
	const int32 LightMapCoordinateIndex = Fixture.Mesh->GetLightMapCoordinateIndex();
	FMonolithActionResult Read = Execute(TEXT("get_lod_build_settings"), MakeParams(Fixture.ObjectPath));
	TestTrue(TEXT("Read action succeeds"), Read.bSuccess);
	if (Read.bSuccess && Read.Result.IsValid())
	{
		TestEqual(TEXT("Read returns loaded object identity"),
			Read.Result->GetStringField(TEXT("object_path")), Fixture.Mesh->GetPathName());
		TestEqual(TEXT("Read returns package identity"),
			Read.Result->GetStringField(TEXT("package_name")), Fixture.PackagePath);
		TestEqual(TEXT("Read returns source LOD count"),
			static_cast<int32>(Read.Result->GetNumberField(TEXT("source_lod_count"))), Fixture.Mesh->GetNumSourceModels());
		TestEqual(TEXT("Read reports read-only minimum lightmap resolution"),
			static_cast<int32>(Read.Result->GetNumberField(TEXT("min_lightmap_resolution"))), Before.MinLightmapResolution);
		TestEqual(TEXT("Read reports mesh lightmap coordinate index separately"),
			static_cast<int32>(Read.Result->GetNumberField(TEXT("lightmap_coordinate_index"))), LightMapCoordinateIndex);
		TestFalse(TEXT("Read leaves the package clean"), Read.Result->GetBoolField(TEXT("package_dirty")));
	}

	auto PreviewParams = MakeSetParams(Fixture.ObjectPath, MakeSettings(false, 0, 2), true);
	FMonolithActionResult Preview = Execute(TEXT("set_lod_build_settings"), PreviewParams);
	TestTrue(TEXT("Preview action succeeds"), Preview.bSuccess);
	if (Preview.bSuccess && Preview.Result.IsValid())
	{
		TestTrue(TEXT("Preview reports before"), Preview.Result->HasField(TEXT("before")));
		TestTrue(TEXT("Preview reports proposed settings"), Preview.Result->HasField(TEXT("proposed")));
		TestTrue(TEXT("Preview reports a proposed change"), Preview.Result->GetBoolField(TEXT("would_change")));
		TestTrue(TEXT("Preview reports that a rebuild would be required"), Preview.Result->GetBoolField(TEXT("rebuild_required")));
		TestFalse(TEXT("Preview leaves the package clean"), Preview.Result->GetBoolField(TEXT("package_dirty")));
	}
	TestFalse(TEXT("Preview does not change supported settings"),
		Fixture.Mesh->GetSourceModel(0).BuildSettings.DstLightmapIndex != Before.DstLightmapIndex);
	TestEqual(TEXT("Read and preview preserve package cleanliness"), Fixture.Package->IsDirty(), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBuildSettingsNoOpTest,
	"Monolith.Mesh.BuildSettings.NoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBuildSettingsNoOpTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable in-memory cube fixture created"), Fixture.Create()))
	{
		return false;
	}

	const auto SameSettings = MakeSettings(false, 0, 1);
	Fixture.Package->SetDirtyFlag(true);
	FMonolithActionResult DirtyNoOp = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, SameSettings));
	TestTrue(TEXT("No-op on an already-dirty package succeeds"), DirtyNoOp.bSuccess);
	TestTrue(TEXT("No-op preserves existing dirty state"), Fixture.Package->IsDirty());
	if (DirtyNoOp.bSuccess && DirtyNoOp.Result.IsValid())
	{
		TestEqual(TEXT("No-op does not rebuild"), DirtyNoOp.Result->GetStringField(TEXT("rebuild_status")), FString(TEXT("not_required")));
		TestTrue(TEXT("No-op reports actual dirty state"), DirtyNoOp.Result->GetBoolField(TEXT("package_dirty")));
		TestEqual(TEXT("No-op does not save"), DirtyNoOp.Result->GetStringField(TEXT("save_status")), FString(TEXT("not_attempted")));
	}

	Fixture.Package->SetDirtyFlag(false);
	FScopedBuildCounter BuildCounter(Fixture.Mesh);
	FMonolithActionResult CleanNoOp = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, SameSettings));
	TestTrue(TEXT("No-op on a clean package succeeds"), CleanNoOp.bSuccess);
	TestFalse(TEXT("Clean no-op remains clean"), Fixture.Package->IsDirty());
	TestEqual(TEXT("Clean no-op does not fire a target build callback"), BuildCounter.Count, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBuildSettingsChangedPatchTest,
	"Monolith.Mesh.BuildSettings.ChangedPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBuildSettingsChangedPatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable in-memory cube fixture created"), Fixture.Create()))
	{
		return false;
	}

	const FMeshBuildSettings Before = Fixture.Mesh->GetSourceModel(0).BuildSettings;
	const FString ObjectPathBefore = Fixture.Mesh->GetPathName();
	const FString PackagePathBefore = Fixture.Mesh->GetOutermost()->GetName();
	const int32 LightMapCoordinateIndex = Fixture.Mesh->GetLightMapCoordinateIndex();
	FScopedBuildCounter BuildCounter(Fixture.Mesh);
	FMonolithActionResult Changed = Execute(TEXT("set_lod_build_settings"),
		MakeSetParams(Fixture.ObjectPath, MakeSettings(true)));

	TestTrue(TEXT("Changed patch and synchronous mesh rebuild succeed"), Changed.bSuccess);
	TestEqual(TEXT("Changed patch triggers exactly one target build callback"), BuildCounter.Count, 1);
	TestTrue(TEXT("Changed patch marks the package dirty"), Fixture.Package->IsDirty());
	TestEqual(TEXT("Patch preserves object identity"), Fixture.Mesh->GetPathName(), ObjectPathBefore);
	TestEqual(TEXT("Patch preserves package identity"), Fixture.Mesh->GetOutermost()->GetName(), PackagePathBefore);
	TestEqual(TEXT("Patch preserves UStaticMesh LightMapCoordinateIndex"),
		Fixture.Mesh->GetLightMapCoordinateIndex(), LightMapCoordinateIndex);

	FMeshBuildSettings Expected = Before;
	Expected.bGenerateLightmapUVs = true;
	TestTrue(TEXT("Only the requested field changes; omitted build settings remain intact"),
		Expected == Fixture.Mesh->GetSourceModel(0).BuildSettings);
	if (Changed.bSuccess && Changed.Result.IsValid())
	{
		TestTrue(TEXT("Changed result reports settings_changed"), Changed.Result->GetBoolField(TEXT("settings_changed")));
		TestEqual(TEXT("Changed result reports successful rebuild"), Changed.Result->GetStringField(TEXT("rebuild_status")), FString(TEXT("succeeded")));
		TestEqual(TEXT("Changed result reports actual package dirtiness"), Changed.Result->GetBoolField(TEXT("package_dirty")), true);
		TestEqual(TEXT("Changed result does not save"), Changed.Result->GetStringField(TEXT("save_status")), FString(TEXT("not_attempted")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBuildSettingsInvalidPatchTest,
	"Monolith.Mesh.BuildSettings.InvalidPatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBuildSettingsInvalidPatchTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable in-memory cube fixture created"), Fixture.Create()))
	{
		return false;
	}

	FMeshDescription* MeshDescription = Fixture.Mesh->GetMeshDescription(0);
	if (!TestNotNull(TEXT("Cube source MeshDescription exists"), MeshDescription))
	{
		return false;
	}
	const int32 SourceUVCount = FStaticMeshConstAttributes(*MeshDescription).GetVertexInstanceUVs().GetNumChannels();
	TestTrue(TEXT("Fixture has a missing-but-representable UV channel for validation"),
		SourceUVCount < MAX_MESH_TEXTURE_COORDS_MD);

	TArray<TPair<FString, TSharedPtr<FJsonObject>>> InvalidCases;
	{
		auto Params = MakeSetParams(Fixture.ObjectPath, MakeSettings(true));
		Params->SetBoolField(TEXT("unexpected"), true);
		InvalidCases.Emplace(TEXT("unknown top-level key"), Params);
	}
	{
		auto Settings = MakeSettings(true);
		Settings->SetNumberField(TEXT("unsupported"), 1);
		InvalidCases.Emplace(TEXT("unknown nested setting alongside a valid setting"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Params = MakeParams(Fixture.ObjectPath);
		Params->SetField(TEXT("settings"), MakeShared<FJsonValueNull>());
		InvalidCases.Emplace(TEXT("null settings"), Params);
	}
	{
		auto Settings = MakeShared<FJsonObject>();
		Settings->SetStringField(TEXT("generate_lightmap_uvs"), TEXT("true"));
		InvalidCases.Emplace(TEXT("wrong supported setting type"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Params = MakeSetParams(Fixture.ObjectPath, MakeSettings(true));
		Params->SetStringField(TEXT("preview"), TEXT("true"));
		InvalidCases.Emplace(TEXT("wrong preview type"), Params);
	}
	{
		auto Params = MakeSetParams(Fixture.ObjectPath, MakeSettings(true));
		Params->SetNumberField(TEXT("lod_index"), 0.5);
		InvalidCases.Emplace(TEXT("fractional LOD index"), Params);
	}
	{
		auto Params = MakeSetParams(Fixture.ObjectPath, MakeSettings(true));
		Params->SetNumberField(TEXT("lod_index"), std::numeric_limits<double>::quiet_NaN());
		InvalidCases.Emplace(TEXT("non-finite LOD index"), Params);
	}
	{
		auto Params = MakeSetParams(Fixture.ObjectPath, MakeSettings(true));
		Params->SetNumberField(TEXT("lod_index"), Fixture.Mesh->GetNumSourceModels());
		InvalidCases.Emplace(TEXT("out-of-range LOD index"), Params);
	}
	{
		auto Settings = MakeSettings(true);
		Settings->SetNumberField(TEXT("src_lightmap_index"), 0.5);
		InvalidCases.Emplace(TEXT("fractional UV index"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Settings = MakeSettings(true);
		Settings->SetNumberField(TEXT("src_lightmap_index"), std::numeric_limits<double>::infinity());
		InvalidCases.Emplace(TEXT("non-finite UV index"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Settings = MakeSettings(true);
		Settings->SetNumberField(TEXT("src_lightmap_index"), MAX_MESH_TEXTURE_COORDS_MD);
		InvalidCases.Emplace(TEXT("UV index at engine maximum"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Settings = MakeSettings(true);
		Settings->SetNumberField(TEXT("src_lightmap_index"), -1);
		InvalidCases.Emplace(TEXT("negative UV index"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Params = MakeParams(Fixture.ObjectPath);
		auto EmptySettings = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("settings"), EmptySettings);
		InvalidCases.Emplace(TEXT("empty settings object"), Params);
	}
	{
		auto Settings = MakeSettings(true, SourceUVCount, 0);
		InvalidCases.Emplace(TEXT("source UV channel absent from source MeshDescription"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	{
		auto Settings = MakeSettings(true, 0, 0);
		InvalidCases.Emplace(TEXT("source and destination channels equal"), MakeSetParams(Fixture.ObjectPath, Settings));
	}
	if (SourceUVCount + 1 < MAX_MESH_TEXTURE_COORDS_MD)
	{
		auto Settings = MakeSettings(true, 0, SourceUVCount + 1);
		InvalidCases.Emplace(TEXT("generation destination skips the next source UV channel"), MakeSetParams(Fixture.ObjectPath, Settings));
	}

	FScopedBuildCounter BuildCounter(Fixture.Mesh);
	bool bAllRejectedWithoutMutation = true;
	{
		FMeshBuildSettings& Current = Fixture.Mesh->GetSourceModel(0).BuildSettings;
		Current.SrcLightmapIndex = 0;
		Current.DstLightmapIndex = 0;
		auto PartialSettings = MakeShared<FJsonObject>();
		PartialSettings->SetBoolField(TEXT("generate_lightmap_uvs"), true);
		const FMonolithActionResult Result = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, PartialSettings));
		bAllRejectedWithoutMutation &= TestFalse(TEXT("Validate omitted source/destination settings as a complete proposed state"), Result.bSuccess);
		Current.DstLightmapIndex = 1;
	}
	{
		FMeshBuildSettings& Current = Fixture.Mesh->GetSourceModel(0).BuildSettings;
		Current.SrcLightmapIndex = SourceUVCount;
		auto PartialSettings = MakeShared<FJsonObject>();
		PartialSettings->SetBoolField(TEXT("generate_lightmap_uvs"), true);
		const FMonolithActionResult Result = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, PartialSettings));
		bAllRejectedWithoutMutation &= TestFalse(TEXT("Validate an omitted source index against the source MeshDescription"), Result.bSuccess);
		Current.SrcLightmapIndex = 0;
	}
	{
		FMeshBuildSettings& Current = Fixture.Mesh->GetSourceModel(0).BuildSettings;
		Current.SrcLightmapIndex = -1;
		auto PartialSettings = MakeShared<FJsonObject>();
		PartialSettings->SetBoolField(TEXT("generate_lightmap_uvs"), true);
		const FMonolithActionResult Result = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, PartialSettings));
		bAllRejectedWithoutMutation &= TestFalse(TEXT("Reject retained negative source index before enabling generation"), Result.bSuccess);
		bAllRejectedWithoutMutation &= TestEqual(TEXT("Retained invalid source is not clamped or mutated"), Current.SrcLightmapIndex, -1);
		Current.SrcLightmapIndex = 0;
	}
	{
		FMeshBuildSettings& Current = Fixture.Mesh->GetSourceModel(0).BuildSettings;
		Current.DstLightmapIndex = MAX_MESH_TEXTURE_COORDS_MD;
		auto PartialSettings = MakeShared<FJsonObject>();
		PartialSettings->SetNumberField(TEXT("src_lightmap_index"), 0);
		const FMonolithActionResult Result = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, PartialSettings));
		bAllRejectedWithoutMutation &= TestFalse(TEXT("Reject retained out-of-range destination even when generation is disabled"), Result.bSuccess);
		bAllRejectedWithoutMutation &= TestEqual(TEXT("Retained invalid destination is not clamped or mutated"), Current.DstLightmapIndex, MAX_MESH_TEXTURE_COORDS_MD);
		Current.DstLightmapIndex = 1;
	}
	for (const TPair<FString, TSharedPtr<FJsonObject>>& InvalidCase : InvalidCases)
	{
		const FMonolithActionResult Result = Execute(TEXT("set_lod_build_settings"), InvalidCase.Value);
		bAllRejectedWithoutMutation &= TestFalse(*FString::Printf(TEXT("Reject %s"), *InvalidCase.Key), Result.bSuccess);
		bAllRejectedWithoutMutation &= TestFalse(*FString::Printf(TEXT("%s does not dirty the package"), *InvalidCase.Key), Fixture.Package->IsDirty());
		bAllRejectedWithoutMutation &= TestFalse(*FString::Printf(TEXT("%s leaves generation unchanged"), *InvalidCase.Key),
			Fixture.Mesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs);
		bAllRejectedWithoutMutation &= TestEqual(*FString::Printf(TEXT("%s leaves source channel unchanged"), *InvalidCase.Key),
			Fixture.Mesh->GetSourceModel(0).BuildSettings.SrcLightmapIndex, 0);
		bAllRejectedWithoutMutation &= TestEqual(*FString::Printf(TEXT("%s leaves destination channel unchanged"), *InvalidCase.Key),
			Fixture.Mesh->GetSourceModel(0).BuildSettings.DstLightmapIndex, 1);
	}
	TestEqual(TEXT("Invalid requests do not build the target"), BuildCounter.Count, 0);
	return bAllRejectedWithoutMutation;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithRebarBuildFailureOutcomeTest,
	"Monolith.Mesh.BuildSettings.RebarBuildFailureOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRebarBuildFailureOutcomeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable Rebar duplicate created"), Fixture.CreateFromAsset(
		TEXT("/Game/Assets/Building/Meshes/Rebar/Rebar_Wall1x1"))))
	{
		return false;
	}

	FMonolithActionResult Changed = Execute(TEXT("set_lod_build_settings"),
		MakeSetParams(Fixture.ObjectPath, MakeSettings(true, 0, 1)));
	TestFalse(TEXT("Known invalid Rebar generation returns a partial build failure"), Changed.bSuccess);
	if (Changed.bSuccess || !Changed.ErrorData.IsValid() || Changed.ErrorData->Type != EJson::Object)
	{
		return false;
	}
	const auto Outcome = Changed.ErrorData->AsObject();
	TestTrue(TEXT("Failure reports changed settings"), Outcome->GetBoolField(TEXT("settings_changed")));
	TestEqual(TEXT("Failure reports failed rebuild"), Outcome->GetStringField(TEXT("rebuild_status")), FString(TEXT("failed")));
	TestEqual(TEXT("Failure never reports persistence"), Outcome->GetStringField(TEXT("save_status")), FString(TEXT("not_attempted")));
	TestTrue(TEXT("Failure preserves the actual dirty partial state"), Outcome->GetBoolField(TEXT("package_dirty")) && Fixture.Package->IsDirty());
	TestTrue(TEXT("In-memory requested setting remains changed after build failure"), Fixture.Mesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs);
	TestTrue(TEXT("Failure retains build diagnostics"), Outcome->GetArrayField(TEXT("diagnostics")).Num() > 0);
	AddInfo(Changed.ErrorMessage);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithWallGeneratedLightmapUVEligibilityTest,
	"Monolith.Mesh.BuildSettings.WallGeneratedUVEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWallGeneratedLightmapUVEligibilityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithMeshBuildSettingsTests;
	FScopedMeshFixture Fixture;
	if (!TestTrue(TEXT("Disposable project wall duplicate created"), Fixture.CreateFromAsset(
		TEXT("/Game/Assets/Building/Meshes/Blockout/Blockout_Wall2x2"))))
	{
		return false;
	}
	// The project representative may already have generation enabled after delivery.
	// Reset only this disposable copy so every run exercises a real generation build.
	auto DisabledSettings = MakeShared<FJsonObject>();
	DisabledSettings->SetBoolField(TEXT("generate_lightmap_uvs"), false);
	const auto Reset = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, DisabledSettings));
	if (!TestTrue(TEXT("Disposable wall generation baseline resets successfully"), Reset.bSuccess))
	{
		AddError(Reset.ErrorMessage);
		return false;
	}
	Fixture.Package->SetDirtyFlag(false);
	const auto BaselineUVs = Execute(TEXT("get_mesh_uvs"), MakeParams(Fixture.ObjectPath));
	if (!TestTrue(TEXT("Disposable wall baseline UV query succeeds"), BaselineUVs.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Generation-disabled copy has only its original source UV channel"),
		static_cast<int32>(BaselineUVs.Result->GetNumberField(TEXT("uv_channels"))), 1);
	const auto Changed = Execute(TEXT("set_lod_build_settings"), MakeSetParams(Fixture.ObjectPath, MakeSettings(true, 0, 1)));
	TestTrue(TEXT("Project wall generation succeeds without diagnostics"), Changed.bSuccess);
	if (!Changed.bSuccess)
	{
		AddError(Changed.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("Every eligibility run changes generation instead of accepting a no-op"), Changed.Result->GetBoolField(TEXT("settings_changed")));
	TestEqual(TEXT("Representative generation observes one target build"),
		static_cast<int32>(Changed.Result->GetNumberField(TEXT("build_callback_count"))), 1);

	FMonolithActionResult SourceUVs = Execute(TEXT("get_mesh_uvs"), MakeUVParams(Fixture.ObjectPath, 0));
	FMonolithActionResult GeneratedUVs = Execute(TEXT("get_mesh_uvs"), MakeUVParams(Fixture.ObjectPath, 1));
	FUVChannelMetrics SourceMetrics;
	FUVChannelMetrics GeneratedMetrics;
	const bool bReadSourceMetrics = ReadUVChannel(SourceUVs, 0, SourceMetrics);
	const bool bReadGeneratedMetrics = ReadUVChannel(GeneratedUVs, 1, GeneratedMetrics);
	TestTrue(TEXT("UV action returns source channel 0 measurements"), bReadSourceMetrics);
	TestTrue(TEXT("UV action returns generated channel 1 measurements"), bReadGeneratedMetrics);
	if (!bReadSourceMetrics || !bReadGeneratedMetrics)
	{
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("Wall disposable UV eligibility: source0 bounds min=(%.6f,%.6f) max=(%.6f,%.6f), islands=%d, overlap_pct=%s; generated1 bounds min=(%.6f,%.6f) max=(%.6f,%.6f), islands=%d, overlap_pct=%s"),
		SourceMetrics.Min.X, SourceMetrics.Min.Y, SourceMetrics.Max.X, SourceMetrics.Max.Y,
		SourceMetrics.Islands, *SourceMetrics.Overlap,
		GeneratedMetrics.Min.X, GeneratedMetrics.Min.Y, GeneratedMetrics.Max.X, GeneratedMetrics.Max.Y,
		GeneratedMetrics.Islands, *GeneratedMetrics.Overlap));

	const bool bFinite = FMath::IsFinite(GeneratedMetrics.Min.X) && FMath::IsFinite(GeneratedMetrics.Min.Y)
		&& FMath::IsFinite(GeneratedMetrics.Max.X) && FMath::IsFinite(GeneratedMetrics.Max.Y);
	if (!TestTrue(TEXT("Generated channel 1 has finite bounds"), bFinite))
	{
		return false;
	}
	TestTrue(TEXT("Generated channel 1 stays within normalized bounds"), GeneratedMetrics.Min.X >= 0.0 && GeneratedMetrics.Min.Y >= 0.0
		&& GeneratedMetrics.Max.X <= 1.0 && GeneratedMetrics.Max.Y <= 1.0);
	TestTrue(TEXT("Generated channel 1 has nonzero U extent"), GeneratedMetrics.Max.X > GeneratedMetrics.Min.X);
	TestTrue(TEXT("Generated channel 1 has nonzero V extent"), GeneratedMetrics.Max.Y > GeneratedMetrics.Min.Y);
	TestTrue(TEXT("Generated channel 1 contains at least one island"), GeneratedMetrics.Islands > 0);
	TestTrue(TEXT("Simple wall generated channel has zero raster-estimated overlap"),
		FMath::IsFinite(GeneratedMetrics.OverlapPercent) && GeneratedMetrics.OverlapPercent <= 0.01);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
