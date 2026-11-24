#include "MonolithLocomotionAnalysisActions.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

namespace
{
	using namespace MonolithLocomotionAnalysis;

	FSample MakeSample(double Time, double RootX, double LeftX, double LeftZ,
		double RightX = 0.0, double RightZ = 0.0, double RootYawDegrees = 0.0)
	{
		FSample Sample;
		Sample.Time = Time;
		Sample.RootLocation = FVector(RootX, 0.0, 0.0);
		Sample.RootRotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(RootYawDegrees));
		Sample.LeftFootLocation = FVector(LeftX, 0.0, LeftZ);
		Sample.RightFootLocation = FVector(RightX, 0.0, RightZ);
		return Sample;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisRootBoneIdentityTest,
	"Monolith.Animation.LocomotionAnalysis.RootBoneIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisRootBoneIdentityTest::RunTest(const FString& Parameters)
{
	const FName SkeletonRootBone(TEXT("root"));
	FString Error;
	TestTrue(TEXT("actual skeleton root is accepted"),
		ValidateRootBoneIdentity(SkeletonRootBone, SkeletonRootBone, Error));
	TestTrue(TEXT("supplied actual root retains FName case semantics"),
		ValidateRootBoneIdentity(FName(TEXT("ROOT")), SkeletonRootBone, Error));
	TestFalse(TEXT("moving child pelvis cannot supply the root-motion trajectory"),
		ValidateRootBoneIdentity(FName(TEXT("pelvis")), SkeletonRootBone, Error));
	TestTrue(TEXT("child rejection names the actual skeleton root"),
		Error.Contains(FString::Printf(TEXT("actual skeleton root '%s'"), *SkeletonRootBone.ToString())));
	TestFalse(TEXT("none is rejected"), ValidateRootBoneIdentity(NAME_None, SkeletonRootBone, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisSampleTimesTest,
	"Monolith.Animation.LocomotionAnalysis.SampleTimesAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisSampleTimesTest::RunTest(const FString& Parameters)
{
	TArray<double> Times;
	FString Error;
	int32 ParsedRate = 0;
	TestTrue(TEXT("integer sample rate is accepted"), ParseSampleRate(30.0, ParsedRate, Error));
	TestEqual(TEXT("sample rate is preserved"), ParsedRate, 30);
	TestFalse(TEXT("fractional sample rate is rejected"), ParseSampleRate(30.5, ParsedRate, Error));
	TestFalse(TEXT("nonfinite sample rate is rejected"), ParseSampleRate(std::numeric_limits<double>::quiet_NaN(), ParsedRate, Error));
	TestTrue(TEXT("valid bounded sample times build"), BuildSampleTimes(0.0, 0.1, 30, Times, Error));
	TestEqual(TEXT("fixed rate samples plus endpoint"), Times.Num(), 4);
	TestEqual(TEXT("start included exactly"), Times[0], 0.0);
	TestEqual(TEXT("end included exactly once"), Times.Last(), 0.1);
	int32 ExactEndpointCount = 0;
	for (const double Time : Times)
	{
		ExactEndpointCount += Time == 0.1 ? 1 : 0;
	}
	TestEqual(TEXT("non-grid endpoint occurs only once"), ExactEndpointCount, 1);
	TestTrue(TEXT("exact maximum sample count including endpoint is accepted"),
		BuildSampleTimes(0.0, 2047.0 / 120.0, 120, Times, Error));
	TestEqual(TEXT("exact maximum sample count is respected"), Times.Num(), MaxSampleCount);
	TestFalse(TEXT("fractional range exceeding cap is rejected"), BuildSampleTimes(0.0, 20.0, 120, Times, Error));
	TestTrue(TEXT("cap error is explicit"), Error.Contains(TEXT("2048-sample limit")));
	TestFalse(TEXT("reversed range is rejected"), BuildSampleTimes(1.0, 0.5, 30, Times, Error));
	TestFalse(TEXT("out-of-range sample rate is rejected"), BuildSampleTimes(0.0, 1.0, 121, Times, Error));
	TestFalse(TEXT("nonfinite end time is rejected"), BuildSampleTimes(0.0, std::numeric_limits<double>::quiet_NaN(), 30, Times, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisMetricsTest,
	"Monolith.Animation.LocomotionAnalysis.ActualDeltaTimeAndReturnToOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisMetricsTest::RunTest(const FString& Parameters)
{
	TArray<FSample> Samples;
	Samples.Add(MakeSample(0.0, 0.0, 0.0, 0.0, 0.0, 10.0, 179.0));
	Samples.Add(MakeSample(0.1, 10.0, 10.0, 0.0, 0.0, 10.0, -179.0));
	Samples.Add(MakeSample(0.35, 0.0, 35.0, 0.0, 0.0, 10.0, -178.0));
	FAnalysisResult Result;
	FString Error;
	TestTrue(TEXT("finite synthetic samples analyze"), AnalyzeSamples(Samples, 3.0, 0.0, 5.0, 0.0,
		true, false, Result, Error));
	TestTrue(TEXT("root path accumulates even when it returns to origin"), FMath::IsNearlyEqual(Result.RootPlanarPathLength, 20.0));
	TestTrue(TEXT("root net displacement preserves return to origin"), FMath::IsNearlyZero(Result.RootPlanarNetDisplacement));
	TestTrue(TEXT("shortest yaw deltas cross the wrap safely"), FMath::IsNearlyEqual(Result.RootCumulativeShortestYawChange, 3.0, 1.0e-3));
	TestTrue(TEXT("foot speed uses actual unequal sample intervals"), FMath::IsNearlyEqual(Result.LeftFoot.MinimumPlanarSpeed, 100.0, 1.0e-3)
		&& FMath::IsNearlyEqual(Result.LeftFoot.MaximumPlanarSpeed, 100.0, 1.0e-3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisContactIntervalsTest,
	"Monolith.Animation.LocomotionAnalysis.ContactIntervalGrouping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisContactIntervalsTest::RunTest(const FString& Parameters)
{
	const TArray<double> Times = { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
	const TArray<double> Values = { 0.0, 0.8, 0.9, 1.0, 0.0, 0.8, 0.9 };
	TArray<FInterval> Intervals;
	FString Error;
	TestTrue(TEXT("authored curve runs group"), BuildAuthoredContactIntervals(Times, Values,
		0.5, 0.1, Intervals, Error));
	TestEqual(TEXT("separated runs stay separate"), Intervals.Num(), 2);
	TestTrue(TEXT("first high run starts on its first high sample"), FMath::IsNearlyEqual(Intervals[0].StartTime, 0.1));
	TestTrue(TEXT("first run merges adjacent high segments"), FMath::IsNearlyEqual(Intervals[0].EndTime, 0.3));
	TestTrue(TEXT("second run starts at its first high sample, excluding the preceding zero"), FMath::IsNearlyEqual(Intervals[1].StartTime, 0.5)
		&& FMath::IsNearlyEqual(Intervals[1].EndTime, 0.6));
	const TArray<double> MismatchedValues = { 1.0 };
	TestFalse(TEXT("mismatched authored samples are rejected"), BuildAuthoredContactIntervals(Times,
		MismatchedValues, 0.5, 0.0, Intervals, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisSlidingTest,
	"Monolith.Animation.LocomotionAnalysis.SlidingAvailabilityAndEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisSlidingTest::RunTest(const FString& Parameters)
{
	TArray<FSample> Samples;
	Samples.Add(MakeSample(0.0, 0.0, 0.0, 0.0));
	Samples.Add(MakeSample(0.1, 10.0, 1.0, 0.0));
	Samples.Add(MakeSample(0.2, 20.0, 2.0, 0.0));
	FAnalysisResult Result;
	FString Error;
	TestTrue(TEXT("root-motion-enabled trajectory analyzes"), AnalyzeSamples(Samples, 0.0, 20.0,
		5.0, 0.05, true, false, Result, Error));
	TestTrue(TEXT("nontrivial sampled root path enables sliding assessment"), Result.bSlidingAssessmentAvailable);
	TestTrue(TEXT("slow-contact geometric evidence is measured"), Result.LeftSliding.bHasEvidence);
	TestTrue(TEXT("speed threshold flags sliding suspicion"), Result.LeftSliding.bSuspectSliding);
	TestEqual(TEXT("geometric source remains labelled"), Result.LeftSliding.EvidenceSource, FString(TEXT("geometric_candidate")));
	TestEqual(TEXT("geometric candidates merge contiguous segments"), Result.LeftFoot.GeometricCandidateIntervals.Num(), 1);
	TestTrue(TEXT("candidate interval spans the contiguous low-height slow-speed run"),
		FMath::IsNearlyEqual(Result.LeftFoot.GeometricCandidateIntervals[0].StartTime, 0.0)
		&& FMath::IsNearlyEqual(Result.LeftFoot.GeometricCandidateIntervals[0].EndTime, 0.2));

	TestTrue(TEXT("root lock case analyzes"), AnalyzeSamples(Samples, 0.0, 20.0,
		5.0, 0.05, true, true, Result, Error));
	TestFalse(TEXT("root lock makes the sliding basis unavailable"), Result.bSlidingAssessmentAvailable);
	TestEqual(TEXT("root lock reason is explicit"), Result.SlidingUnavailableReason, FString(TEXT("force_root_lock")));
	TestTrue(TEXT("disabled root motion case analyzes"), AnalyzeSamples(Samples, 0.0, 20.0,
		5.0, 0.05, false, false, Result, Error));
	TestEqual(TEXT("disabled root motion reason is explicit"), Result.SlidingUnavailableReason, FString(TEXT("root_motion_disabled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLocomotionAnalysisUnknownsTest,
	"Monolith.Animation.LocomotionAnalysis.UnknownAndInvalidSamples",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocomotionAnalysisUnknownsTest::RunTest(const FString& Parameters)
{
	FString Error;
	FAnalysisResult Result;
	TArray<FSample> OneSample;
	OneSample.Add(MakeSample(0.0, 0.0, 0.0, 0.0));
	TestTrue(TEXT("single pose has valid height but no speed"), AnalyzeSamples(OneSample,
		3.0, 20.0, 5.0, 0.05, true, false, Result, Error));
	TestFalse(TEXT("single pose speed remains unknown"), Result.LeftFoot.bHasSpeedSamples);
	TestFalse(TEXT("in-place root trajectory cannot support sliding"), Result.bSlidingAssessmentAvailable);
	TestEqual(TEXT("in-place reason is explicit"), Result.SlidingUnavailableReason,
		FString(TEXT("no_root_planar_motion_in_sampled_range")));

	OneSample[0].RightFootLocation.X = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("nonfinite evaluated transform is rejected"), AnalyzeSamples(OneSample,
		3.0, 20.0, 5.0, 0.05, true, false, Result, Error));
	TestTrue(TEXT("invalid sample error identifies sample"), Error.Contains(TEXT("sample 0")));
	OneSample[0].RightFootLocation.X = 0.0;
	TestFalse(TEXT("negative thresholds are rejected"), AnalyzeSamples(OneSample,
		3.0, -1.0, 5.0, 0.05, true, false, Result, Error));

	const TArray<double> Times = { 0.0, 0.1 };
	const TArray<double> Values = { 0.0, 0.0 };
	TArray<FInterval> NoIntervals;
	TestTrue(TEXT("a configured but inactive curve is valid and has no intervals"),
		BuildAuthoredContactIntervals(Times, Values, 0.5, 0.05, NoIntervals, Error));
	TestEqual(TEXT("inactive curve does not fabricate contact"), NoIntervals.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
