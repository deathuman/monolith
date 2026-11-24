#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

namespace MonolithLocomotionAnalysis
{
	constexpr int32 MaxSampleCount = 2048;

	struct FInterval
	{
		double StartTime = 0.0;
		double EndTime = 0.0;
	};

	struct FSample
	{
		double Time = 0.0;
		FVector RootLocation = FVector::ZeroVector;
		FQuat RootRotation = FQuat::Identity;
		FVector LeftFootLocation = FVector::ZeroVector;
		FQuat LeftFootRotation = FQuat::Identity;
		FVector RightFootLocation = FVector::ZeroVector;
		FQuat RightFootRotation = FQuat::Identity;
	};

	struct FFootSummary
	{
		double MinimumHeight = 0.0;
		double MaximumHeight = 0.0;
		bool bHasSpeedSamples = false;
		int32 SpeedSampleCount = 0;
		double MinimumPlanarSpeed = 0.0;
		double MaximumPlanarSpeed = 0.0;
		TArray<FInterval> GeometricCandidateIntervals;
	};

	struct FIntervalMotion
	{
		FInterval Interval;
		double PlanarPathLength = 0.0;
		double NetPlanarDrift = 0.0;
		double MaximumPlanarSpeed = 0.0;
	};

	struct FFootSlidingAssessment
	{
		bool bHasEvidence = false;
		bool bSuspectSliding = false;
		FString EvidenceSource;
		TArray<FIntervalMotion> Intervals;
	};

	struct FAnalysisResult
	{
		double RootPlanarPathLength = 0.0;
		double RootPlanarNetDisplacement = 0.0;
		double RootCumulativeShortestYawChange = 0.0;
		double RootNetShortestYawChange = 0.0;
		FFootSummary LeftFoot;
		FFootSummary RightFoot;
		bool bSlidingAssessmentAvailable = false;
		FString SlidingUnavailableReason;
		FFootSlidingAssessment LeftSliding;
		FFootSlidingAssessment RightSliding;
	};

	/** Only the actual skeleton root may provide the root-motion trajectory. */
	bool ValidateRootBoneIdentity(FName RequestedRootBone, FName SkeletonRootBone, FString& OutError);

	/** Construct fixed-rate sample times plus the exact end time, with a hard cap. */
	bool ParseSampleRate(double Value, int32& OutSampleRate, FString& OutError);
	bool BuildSampleTimes(double StartTime, double EndTime, int32 SampleRate,
		TArray<double>& OutTimes, FString& OutError);

	/** Group high-valued adjacent authored-curve samples into contiguous intervals. */
	bool BuildAuthoredContactIntervals(const TArray<double>& Times,
		const TArray<double>& Values, double Threshold, double MinimumDuration,
		TArray<FInterval>& OutIntervals, FString& OutError);

	/** Pure sample reducer used by the action and focused synthetic fixtures. */
	bool AnalyzeSamples(const TArray<FSample>& Samples, double GroundHeightThreshold,
		double ContactSpeedThreshold, double SlidingSpeedThreshold,
		double MinimumContactDuration, bool bRootMotionEnabled, bool bForceRootLock,
		FAnalysisResult& OutResult, FString& OutError);

	/** Measure foot motion over selected intervals (geometric or authored evidence). */
	bool MeasureSliding(const TArray<FSample>& Samples, bool bLeftFoot,
		const TArray<FInterval>& Intervals, const TCHAR* EvidenceSource,
		double SlidingSpeedThreshold, FFootSlidingAssessment& OutAssessment,
		FString& OutError);
}

class FMonolithLocomotionAnalysisActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleAnalyzeLocomotion(const TSharedPtr<FJsonObject>& Params);
};
