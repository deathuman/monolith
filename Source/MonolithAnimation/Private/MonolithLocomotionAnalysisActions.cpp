#include "MonolithLocomotionAnalysisActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "AnimPose.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonValue.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/Package.h"

namespace MonolithLocomotionAnalysis
{
	namespace
	{
		constexpr double TimeTolerance = 1.0e-10;

		bool IsFiniteVector(const FVector& Value)
		{
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
		}

		bool IsFiniteQuat(const FQuat& Value)
		{
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W)
				&& FMath::IsFinite(Value.SizeSquared())
				&& Value.SizeSquared() > SMALL_NUMBER;
		}

		double PlanarDistance(const FVector& A, const FVector& B)
		{
			return FVector2D(B.X - A.X, B.Y - A.Y).Size();
		}

		const FVector& GetFootLocation(const FSample& Sample, bool bLeftFoot)
		{
			return bLeftFoot ? Sample.LeftFootLocation : Sample.RightFootLocation;
		}

		void AppendIntervals(const TArray<double>& Times, const TArray<uint8>& ActiveSegments,
			double MinimumDuration, TArray<FInterval>& OutIntervals)
		{
			OutIntervals.Reset();
			int32 SegmentIndex = 0;
			while (SegmentIndex < ActiveSegments.Num())
			{
				if (ActiveSegments[SegmentIndex] == 0)
				{
					++SegmentIndex;
					continue;
				}

				const int32 FirstSegment = SegmentIndex;
				while (SegmentIndex + 1 < ActiveSegments.Num()
					&& ActiveSegments[SegmentIndex + 1] != 0)
				{
					++SegmentIndex;
				}

				const FInterval Interval{ Times[FirstSegment], Times[SegmentIndex + 1] };
				if (Interval.EndTime - Interval.StartTime + TimeTolerance >= MinimumDuration)
				{
					OutIntervals.Add(Interval);
				}
				++SegmentIndex;
			}
		}
	}

	bool ValidateRootBoneIdentity(FName RequestedRootBone, FName SkeletonRootBone, FString& OutError)
	{
		OutError.Reset();
		if (RequestedRootBone.IsNone() || SkeletonRootBone.IsNone() || RequestedRootBone != SkeletonRootBone)
		{
			OutError = FString::Printf(TEXT("root_bone must name the actual skeleton root '%s'; received '%s'"),
				*SkeletonRootBone.ToString(), *RequestedRootBone.ToString());
			return false;
		}
		return true;
	}

	bool ParseSampleRate(double Value, int32& OutSampleRate, FString& OutError)
	{
		OutError.Reset();
		if (!FMath::IsFinite(Value) || Value < 1.0 || Value > 120.0)
		{
			OutError = TEXT("sample_rate must be a finite integer in [1, 120]");
			return false;
		}
		const int32 ParsedRate = static_cast<int32>(Value);
		if (static_cast<double>(ParsedRate) != Value)
		{
			OutError = TEXT("sample_rate must be a finite integer in [1, 120]");
			return false;
		}
		OutSampleRate = ParsedRate;
		return true;
	}

	bool BuildSampleTimes(double StartTime, double EndTime, int32 SampleRate,
		TArray<double>& OutTimes, FString& OutError)
	{
		OutTimes.Reset();
		OutError.Reset();
		if (!FMath::IsFinite(StartTime) || !FMath::IsFinite(EndTime))
		{
			OutError = TEXT("start_time and end_time must be finite");
			return false;
		}
		if (StartTime < 0.0 || EndTime < StartTime)
		{
			OutError = TEXT("time range must satisfy 0 <= start_time <= end_time");
			return false;
		}
		if (SampleRate < 1 || SampleRate > 120)
		{
			OutError = TEXT("sample_rate must be an integer in [1, 120]");
			return false;
		}

		OutTimes.Add(StartTime);
		if (StartTime == EndTime)
		{
			return true;
		}

		for (int32 Index = 1; Index < MaxSampleCount; ++Index)
		{
			const double Candidate = StartTime + static_cast<double>(Index) / static_cast<double>(SampleRate);
			if (!FMath::IsFinite(Candidate) || Candidate <= OutTimes.Last())
			{
				OutTimes.Reset();
				OutError = TEXT("sample times are not representable as distinct finite values");
				return false;
			}

			const double Tolerance = TimeTolerance * FMath::Max(1.0, FMath::Abs(EndTime));
			if (FMath::Abs(Candidate - EndTime) <= Tolerance)
			{
				OutTimes.Add(EndTime);
				return true;
			}
			if (Candidate < EndTime)
			{
				OutTimes.Add(Candidate);
				continue;
			}

			OutTimes.Add(EndTime);
			return true;
		}

		OutTimes.Reset();
		OutError = FString::Printf(TEXT("requested range exceeds the %d-sample limit"), MaxSampleCount);
		return false;
	}

	bool BuildAuthoredContactIntervals(const TArray<double>& Times,
		const TArray<double>& Values, double Threshold, double MinimumDuration,
		TArray<FInterval>& OutIntervals, FString& OutError)
	{
		OutIntervals.Reset();
		OutError.Reset();
		if (Times.Num() != Values.Num() || Times.Num() > MaxSampleCount)
		{
			OutError = TEXT("curve sample times and values must have matching counts within the sample limit");
			return false;
		}
		if (!FMath::IsFinite(Threshold) || !FMath::IsFinite(MinimumDuration)
			|| Threshold < 0.5 || MinimumDuration < 0.0)
		{
			OutError = TEXT("curve threshold and minimum duration must be finite and nonnegative; threshold must be at least 0.5");
			return false;
		}
		for (int32 Index = 0; Index < Times.Num(); ++Index)
		{
			if (!FMath::IsFinite(Times[Index]) || !FMath::IsFinite(Values[Index])
				|| (Index > 0 && Times[Index] <= Times[Index - 1]))
			{
				OutError = TEXT("curve samples must contain finite values at strictly increasing times");
				return false;
			}
		}

		TArray<uint8> ActiveSegments;
		if (Times.Num() > 1)
		{
			ActiveSegments.SetNumUninitialized(Times.Num() - 1);
			for (int32 Index = 0; Index + 1 < Times.Num(); ++Index)
			{
				ActiveSegments[Index] = (Values[Index] >= Threshold && Values[Index + 1] >= Threshold) ? 1 : 0;
			}
		}
		AppendIntervals(Times, ActiveSegments, MinimumDuration, OutIntervals);
		return true;
	}

	bool AnalyzeSamples(const TArray<FSample>& Samples, double GroundHeightThreshold,
		double ContactSpeedThreshold, double SlidingSpeedThreshold,
		double MinimumContactDuration, bool bRootMotionEnabled, bool bForceRootLock,
		FAnalysisResult& OutResult, FString& OutError)
	{
		OutResult = FAnalysisResult();
		OutError.Reset();
		if (Samples.IsEmpty() || Samples.Num() > MaxSampleCount)
		{
			OutError = FString::Printf(TEXT("sample count must be in [1, %d]"), MaxSampleCount);
			return false;
		}
		if (!FMath::IsFinite(GroundHeightThreshold) || GroundHeightThreshold < 0.0
			|| !FMath::IsFinite(ContactSpeedThreshold) || ContactSpeedThreshold < 0.0
			|| !FMath::IsFinite(SlidingSpeedThreshold) || SlidingSpeedThreshold < 0.0
			|| !FMath::IsFinite(MinimumContactDuration) || MinimumContactDuration < 0.0)
		{
			OutError = TEXT("analysis thresholds must be finite and nonnegative");
			return false;
		}

		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FSample& Sample = Samples[Index];
			if (!FMath::IsFinite(Sample.Time)
				|| (Index > 0 && Sample.Time <= Samples[Index - 1].Time)
				|| !IsFiniteVector(Sample.RootLocation) || !IsFiniteQuat(Sample.RootRotation)
				|| !IsFiniteVector(Sample.LeftFootLocation) || !IsFiniteQuat(Sample.LeftFootRotation)
				|| !IsFiniteVector(Sample.RightFootLocation) || !IsFiniteQuat(Sample.RightFootRotation))
			{
				OutError = FString::Printf(TEXT("sample %d has nonfinite transforms or a non-increasing time"), Index);
				return false;
			}
		}

		const FSample& First = Samples[0];
		const FSample& Last = Samples.Last();
		OutResult.LeftFoot.MinimumHeight = First.LeftFootLocation.Z;
		OutResult.LeftFoot.MaximumHeight = First.LeftFootLocation.Z;
		OutResult.RightFoot.MinimumHeight = First.RightFootLocation.Z;
		OutResult.RightFoot.MaximumHeight = First.RightFootLocation.Z;
		for (const FSample& Sample : Samples)
		{
			OutResult.LeftFoot.MinimumHeight = FMath::Min(OutResult.LeftFoot.MinimumHeight, Sample.LeftFootLocation.Z);
			OutResult.LeftFoot.MaximumHeight = FMath::Max(OutResult.LeftFoot.MaximumHeight, Sample.LeftFootLocation.Z);
			OutResult.RightFoot.MinimumHeight = FMath::Min(OutResult.RightFoot.MinimumHeight, Sample.RightFootLocation.Z);
			OutResult.RightFoot.MaximumHeight = FMath::Max(OutResult.RightFoot.MaximumHeight, Sample.RightFootLocation.Z);
		}

		TArray<uint8> LeftGeometricSegments;
		TArray<uint8> RightGeometricSegments;
		if (Samples.Num() > 1)
		{
			LeftGeometricSegments.SetNumUninitialized(Samples.Num() - 1);
			RightGeometricSegments.SetNumUninitialized(Samples.Num() - 1);
		}

		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const FSample& Sample = Samples[Index];
			if (Index + 1 >= Samples.Num())
			{
				continue;
			}

			const FSample& Next = Samples[Index + 1];
			const double DeltaTime = Next.Time - Sample.Time;
			if (!FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0)
			{
				OutError = FString::Printf(TEXT("sample interval %d has invalid elapsed time"), Index);
				OutResult = FAnalysisResult();
				return false;
			}

			const double RootStep = PlanarDistance(Sample.RootLocation, Next.RootLocation);
			OutResult.RootPlanarPathLength += RootStep;
			const double PreviousYaw = Sample.RootRotation.GetNormalized().Rotator().Yaw;
			const double CurrentYaw = Next.RootRotation.GetNormalized().Rotator().Yaw;
			const double ShortestYaw = FMath::FindDeltaAngleDegrees(PreviousYaw, CurrentYaw);
			OutResult.RootCumulativeShortestYawChange += FMath::Abs(ShortestYaw);

			const double LeftSpeed = PlanarDistance(Sample.LeftFootLocation, Next.LeftFootLocation) / DeltaTime;
			const double RightSpeed = PlanarDistance(Sample.RightFootLocation, Next.RightFootLocation) / DeltaTime;
			if (!FMath::IsFinite(RootStep) || !FMath::IsFinite(OutResult.RootPlanarPathLength)
				|| !FMath::IsFinite(OutResult.RootCumulativeShortestYawChange)
				|| !FMath::IsFinite(OutResult.RootNetShortestYawChange) || !FMath::IsFinite(ShortestYaw)
				|| !FMath::IsFinite(LeftSpeed) || !FMath::IsFinite(RightSpeed))
			{
				OutError = FString::Printf(TEXT("sample interval %d produced a nonfinite measurement"), Index);
				OutResult = FAnalysisResult();
				return false;
			}

			if (!OutResult.LeftFoot.bHasSpeedSamples)
			{
				OutResult.LeftFoot.MinimumPlanarSpeed = LeftSpeed;
				OutResult.LeftFoot.MaximumPlanarSpeed = LeftSpeed;
				OutResult.RightFoot.MinimumPlanarSpeed = RightSpeed;
				OutResult.RightFoot.MaximumPlanarSpeed = RightSpeed;
				OutResult.LeftFoot.bHasSpeedSamples = true;
				OutResult.RightFoot.bHasSpeedSamples = true;
			}
			else
			{
				OutResult.LeftFoot.MinimumPlanarSpeed = FMath::Min(OutResult.LeftFoot.MinimumPlanarSpeed, LeftSpeed);
				OutResult.LeftFoot.MaximumPlanarSpeed = FMath::Max(OutResult.LeftFoot.MaximumPlanarSpeed, LeftSpeed);
				OutResult.RightFoot.MinimumPlanarSpeed = FMath::Min(OutResult.RightFoot.MinimumPlanarSpeed, RightSpeed);
				OutResult.RightFoot.MaximumPlanarSpeed = FMath::Max(OutResult.RightFoot.MaximumPlanarSpeed, RightSpeed);
			}
			++OutResult.LeftFoot.SpeedSampleCount;
			++OutResult.RightFoot.SpeedSampleCount;

			const bool bLeftLow = Sample.LeftFootLocation.Z <= OutResult.LeftFoot.MinimumHeight + GroundHeightThreshold
				&& Next.LeftFootLocation.Z <= OutResult.LeftFoot.MinimumHeight + GroundHeightThreshold;
			const bool bRightLow = Sample.RightFootLocation.Z <= OutResult.RightFoot.MinimumHeight + GroundHeightThreshold
				&& Next.RightFootLocation.Z <= OutResult.RightFoot.MinimumHeight + GroundHeightThreshold;
			LeftGeometricSegments[Index] = (bLeftLow && LeftSpeed <= ContactSpeedThreshold) ? 1 : 0;
			RightGeometricSegments[Index] = (bRightLow && RightSpeed <= ContactSpeedThreshold) ? 1 : 0;
		}

		OutResult.RootPlanarNetDisplacement = PlanarDistance(First.RootLocation, Last.RootLocation);
		OutResult.RootNetShortestYawChange = FMath::FindDeltaAngleDegrees(
			First.RootRotation.GetNormalized().Rotator().Yaw,
			Last.RootRotation.GetNormalized().Rotator().Yaw);
		if (!FMath::IsFinite(OutResult.RootPlanarNetDisplacement))
		{
			OutError = TEXT("root planar net displacement is nonfinite");
			OutResult = FAnalysisResult();
			return false;
		}
		if (!FMath::IsFinite(OutResult.RootNetShortestYawChange))
		{
			OutError = TEXT("root net yaw change is nonfinite");
			OutResult = FAnalysisResult();
			return false;
		}
		TArray<double> Times;
		Times.Reserve(Samples.Num());
		for (const FSample& Sample : Samples)
		{
			Times.Add(Sample.Time);
		}
		AppendIntervals(Times, LeftGeometricSegments, MinimumContactDuration,
			OutResult.LeftFoot.GeometricCandidateIntervals);
		AppendIntervals(Times, RightGeometricSegments, MinimumContactDuration,
			OutResult.RightFoot.GeometricCandidateIntervals);

		if (!bRootMotionEnabled)
		{
			OutResult.SlidingUnavailableReason = TEXT("root_motion_disabled");
		}
		else if (bForceRootLock)
		{
			OutResult.SlidingUnavailableReason = TEXT("force_root_lock");
		}
		else if (OutResult.RootPlanarPathLength <= KINDA_SMALL_NUMBER)
		{
			OutResult.SlidingUnavailableReason = TEXT("no_root_planar_motion_in_sampled_range");
		}
		else
		{
			OutResult.bSlidingAssessmentAvailable = true;
			if (!MeasureSliding(Samples, true, OutResult.LeftFoot.GeometricCandidateIntervals,
				TEXT("geometric_candidate"), SlidingSpeedThreshold, OutResult.LeftSliding, OutError)
				|| !MeasureSliding(Samples, false, OutResult.RightFoot.GeometricCandidateIntervals,
					TEXT("geometric_candidate"), SlidingSpeedThreshold, OutResult.RightSliding, OutError))
			{
				OutResult = FAnalysisResult();
				return false;
			}
		}
		return true;
	}

	bool MeasureSliding(const TArray<FSample>& Samples, bool bLeftFoot,
		const TArray<FInterval>& Intervals, const TCHAR* EvidenceSource,
		double SlidingSpeedThreshold, FFootSlidingAssessment& OutAssessment,
		FString& OutError)
	{
		OutAssessment = FFootSlidingAssessment();
		OutError.Reset();
		if (Samples.IsEmpty() || Samples.Num() > MaxSampleCount
			|| Intervals.Num() > MaxSampleCount || !FMath::IsFinite(SlidingSpeedThreshold)
			|| SlidingSpeedThreshold < 0.0)
		{
			OutError = TEXT("sliding inputs exceed bounds or contain an invalid threshold");
			return false;
		}
		OutAssessment.EvidenceSource = EvidenceSource;
		for (int32 IntervalIndex = 0; IntervalIndex < Intervals.Num(); ++IntervalIndex)
		{
			const FInterval& Interval = Intervals[IntervalIndex];
			if (!FMath::IsFinite(Interval.StartTime) || !FMath::IsFinite(Interval.EndTime)
				|| Interval.EndTime <= Interval.StartTime
				|| (IntervalIndex > 0 && Interval.StartTime < Intervals[IntervalIndex - 1].EndTime - TimeTolerance))
			{
				OutError = FString::Printf(TEXT("sliding interval %d is invalid or overlaps its predecessor"), IntervalIndex);
				return false;
			}

			int32 FirstSample = INDEX_NONE;
			int32 LastSample = INDEX_NONE;
			FIntervalMotion Motion;
			Motion.Interval = Interval;
			for (int32 SampleIndex = 0; SampleIndex + 1 < Samples.Num(); ++SampleIndex)
			{
				const FSample& Current = Samples[SampleIndex];
				const FSample& Next = Samples[SampleIndex + 1];
				const double DeltaTime = Next.Time - Current.Time;
				if (!FMath::IsFinite(Current.Time) || !FMath::IsFinite(Next.Time)
					|| !FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0
					|| !IsFiniteVector(GetFootLocation(Current, bLeftFoot))
					|| !IsFiniteVector(GetFootLocation(Next, bLeftFoot)))
				{
					OutError = FString::Printf(TEXT("sample interval %d is invalid for sliding measurement"), SampleIndex);
					return false;
				}
				if (Current.Time < Interval.StartTime - TimeTolerance
					|| Next.Time > Interval.EndTime + TimeTolerance)
				{
					continue;
				}

				const FVector& CurrentLocation = GetFootLocation(Current, bLeftFoot);
				const FVector& NextLocation = GetFootLocation(Next, bLeftFoot);
				const double StepDistance = PlanarDistance(CurrentLocation, NextLocation);
				Motion.PlanarPathLength += StepDistance;
				Motion.MaximumPlanarSpeed = FMath::Max(Motion.MaximumPlanarSpeed, StepDistance / DeltaTime);
				if (!FMath::IsFinite(Motion.PlanarPathLength) || !FMath::IsFinite(Motion.MaximumPlanarSpeed))
				{
					OutError = FString::Printf(TEXT("sliding interval %d produced a nonfinite measurement"), IntervalIndex);
					return false;
				}
				if (FirstSample == INDEX_NONE)
				{
					FirstSample = SampleIndex;
				}
				LastSample = SampleIndex + 1;
			}

			if (FirstSample != INDEX_NONE && LastSample > FirstSample)
			{
				Motion.NetPlanarDrift = PlanarDistance(
					GetFootLocation(Samples[FirstSample], bLeftFoot),
					GetFootLocation(Samples[LastSample], bLeftFoot));
				OutAssessment.bHasEvidence = true;
				OutAssessment.bSuspectSliding |= Motion.MaximumPlanarSpeed > SlidingSpeedThreshold;
				OutAssessment.Intervals.Add(Motion);
			}
		}
		return true;
	}
}

namespace
{
	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name,
		FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(Name)
			|| !Params->TryGetStringField(Name, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("required parameter '%s' must be a non-empty string"), Name);
			return false;
		}
		return true;
	}

	bool ReadOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name,
		const FString& DefaultValue, FString& OutValue, FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Name))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::String>(Name)
			|| !Params->TryGetStringField(Name, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("optional parameter '%s' must be a non-empty string when supplied"), Name);
			return false;
		}
		return true;
	}

	bool ReadNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name,
		double DefaultValue, double& OutValue, FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Name))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::Number>(Name)
			|| !Params->TryGetNumberField(Name, OutValue) || !FMath::IsFinite(OutValue))
		{
			OutError = FString::Printf(TEXT("parameter '%s' must be a finite number"), Name);
			return false;
		}
		return true;
	}

	bool ReadSampleRate(const TSharedPtr<FJsonObject>& Params, int32& OutRate, FString& OutError)
	{
		double Value = 30.0;
		if (!ReadNumber(Params, TEXT("sample_rate"), 30.0, Value, OutError))
		{
			return false;
		}
		return MonolithLocomotionAnalysis::ParseSampleRate(Value, OutRate, OutError);
	}

	bool ReadBoolean(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name,
		bool DefaultValue, bool& OutValue, FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Name))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::Boolean>(Name) || !Params->TryGetBoolField(Name, OutValue))
		{
			OutError = FString::Printf(TEXT("parameter '%s' must be a boolean"), Name);
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> ToJsonVector(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Value.X);
		Json->SetNumberField(TEXT("y"), Value.Y);
		Json->SetNumberField(TEXT("z"), Value.Z);
		return Json;
	}

	TSharedPtr<FJsonObject> ToJsonQuat(const FQuat& Value)
	{
		const FQuat Normalized = Value.GetNormalized();
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Normalized.X);
		Json->SetNumberField(TEXT("y"), Normalized.Y);
		Json->SetNumberField(TEXT("z"), Normalized.Z);
		Json->SetNumberField(TEXT("w"), Normalized.W);
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonIntervals(const TArray<MonolithLocomotionAnalysis::FInterval>& Intervals,
		const TCHAR* Evidence)
	{
		TArray<TSharedPtr<FJsonValue>> JsonIntervals;
		JsonIntervals.Reserve(Intervals.Num());
		for (const MonolithLocomotionAnalysis::FInterval& Interval : Intervals)
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetNumberField(TEXT("start_time"), Interval.StartTime);
			Json->SetNumberField(TEXT("end_time"), Interval.EndTime);
			Json->SetNumberField(TEXT("duration_seconds"), Interval.EndTime - Interval.StartTime);
			Json->SetStringField(TEXT("evidence"), Evidence);
			JsonIntervals.Add(MakeShared<FJsonValueObject>(Json));
		}
		return JsonIntervals;
	}

	TSharedPtr<FJsonObject> ToJsonIntervalMotion(const MonolithLocomotionAnalysis::FIntervalMotion& Motion)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("start_time"), Motion.Interval.StartTime);
		Json->SetNumberField(TEXT("end_time"), Motion.Interval.EndTime);
		Json->SetNumberField(TEXT("planar_path_cm"), Motion.PlanarPathLength);
		Json->SetNumberField(TEXT("net_planar_drift_cm"), Motion.NetPlanarDrift);
		Json->SetNumberField(TEXT("maximum_planar_speed_cm_s"), Motion.MaximumPlanarSpeed);
		return Json;
	}

	TSharedPtr<FJsonObject> ToJsonSlidingFoot(const MonolithLocomotionAnalysis::FFootSlidingAssessment& Assessment,
		bool bAvailable)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("available"), bAvailable);
		Json->SetBoolField(TEXT("contact_interval_evidence_available"), Assessment.bHasEvidence);
		Json->SetStringField(TEXT("evidence_source"), Assessment.EvidenceSource);
		Json->SetBoolField(TEXT("suspect_sliding"), bAvailable && Assessment.bSuspectSliding);
		Json->SetStringField(TEXT("status"), !bAvailable ? TEXT("unavailable")
			: Assessment.bHasEvidence ? (Assessment.bSuspectSliding ? TEXT("suspect_sliding") : TEXT("below_threshold"))
			: TEXT("no_contact_intervals"));
		TArray<TSharedPtr<FJsonValue>> JsonIntervals;
		for (const MonolithLocomotionAnalysis::FIntervalMotion& Motion : Assessment.Intervals)
		{
			JsonIntervals.Add(MakeShared<FJsonValueObject>(ToJsonIntervalMotion(Motion)));
		}
		Json->SetArrayField(TEXT("intervals"), JsonIntervals);
		return Json;
	}

	const FFloatCurve* FindFloatCurve(const IAnimationDataModel* DataModel, const FName& CurveName)
	{
		if (!DataModel || CurveName.IsNone())
		{
			return nullptr;
		}
		const FAnimationCurveIdentifier Identifier(CurveName, ERawCurveTrackTypes::RCT_Float);
		return DataModel->FindFloatCurve(Identifier);
	}

	struct FCurveRead
	{
		FName Name = NAME_None;
		const FFloatCurve* Curve = nullptr;
		TArray<double> Times;
		TArray<double> Values;
		TArray<MonolithLocomotionAnalysis::FInterval> ContactIntervals;
	};

	bool ReadCurve(const IAnimationDataModel* DataModel, FName Name,
		const TArray<double>& Times, double MinimumDuration, FCurveRead& OutCurve,
		FString& OutError)
	{
		OutCurve = FCurveRead();
		OutCurve.Name = Name;
		OutCurve.Times = Times;
		OutCurve.Curve = FindFloatCurve(DataModel, Name);
		if (!OutCurve.Curve)
		{
			return true;
		}

		OutCurve.Values.Reserve(Times.Num());
		for (double Time : Times)
		{
			const double Value = OutCurve.Curve->FloatCurve.Eval(static_cast<float>(Time));
			if (!FMath::IsFinite(Value))
			{
				OutError = FString::Printf(TEXT("float curve '%s' evaluated to a nonfinite value at %.9f seconds"),
					*Name.ToString(), Time);
				return false;
			}
			OutCurve.Values.Add(Value);
		}
		return MonolithLocomotionAnalysis::BuildAuthoredContactIntervals(
			Times, OutCurve.Values, 0.5, MinimumDuration, OutCurve.ContactIntervals, OutError);
	}

	TSharedPtr<FJsonObject> ToJsonFootSummary(const MonolithLocomotionAnalysis::FFootSummary& Summary,
		const FString& BoneName, int32 BoneIndex)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("bone_name"), BoneName);
		Json->SetNumberField(TEXT("evaluated_pose_bone_index"), BoneIndex);
		Json->SetNumberField(TEXT("minimum_height_cm"), Summary.MinimumHeight);
		Json->SetNumberField(TEXT("maximum_height_cm"), Summary.MaximumHeight);
		Json->SetBoolField(TEXT("speed_samples_available"), Summary.bHasSpeedSamples);
		Json->SetNumberField(TEXT("speed_sample_count"), Summary.SpeedSampleCount);
		if (Summary.bHasSpeedSamples)
		{
			Json->SetNumberField(TEXT("minimum_planar_speed_cm_s"), Summary.MinimumPlanarSpeed);
			Json->SetNumberField(TEXT("maximum_planar_speed_cm_s"), Summary.MaximumPlanarSpeed);
		}
		else
		{
			Json->SetField(TEXT("minimum_planar_speed_cm_s"), MakeShared<FJsonValueNull>());
			Json->SetField(TEXT("maximum_planar_speed_cm_s"), MakeShared<FJsonValueNull>());
		}
		Json->SetArrayField(TEXT("geometric_candidate_contact_intervals"),
			ToJsonIntervals(Summary.GeometricCandidateIntervals, TEXT("geometric_candidate")));
		return Json;
	}

	void AddAuthoredMarkers(const UAnimSequence* Sequence, TArray<TSharedPtr<FJsonValue>>& OutMarkers)
	{
		OutMarkers.Reserve(Sequence->AuthoredSyncMarkers.Num());
		for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
			Json->SetNumberField(TEXT("time"), Marker.Time);
			Json->SetStringField(TEXT("evidence"), TEXT("authored_sync_marker_point_event"));
			Json->SetBoolField(TEXT("physical_contact_proven"), false);
			OutMarkers.Add(MakeShared<FJsonValueObject>(Json));
		}
	}

	void AddAuthoredNotifies(const UAnimSequence* Sequence, TArray<TSharedPtr<FJsonValue>>& OutNotifies)
	{
		OutNotifies.Reserve(Sequence->Notifies.Num());
		for (const FAnimNotifyEvent& Event : Sequence->Notifies)
		{
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Event.NotifyName.ToString());
			Json->SetNumberField(TEXT("time"), Event.GetTime());
			Json->SetStringField(TEXT("notify_class"), Event.Notify ? Event.Notify->GetClass()->GetPathName() : TEXT(""));
			Json->SetStringField(TEXT("evidence"), TEXT("authored_notify_point_event"));
			Json->SetBoolField(TEXT("physical_contact_proven"), false);
			OutNotifies.Add(MakeShared<FJsonValueObject>(Json));
		}
	}

	TSharedPtr<FJsonObject> ToJsonCurve(const FCurveRead& Curve, bool bIncludeSamples,
		const TCHAR* Side)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("configured_name"), Curve.Name.ToString());
		Json->SetBoolField(TEXT("found"), Curve.Curve != nullptr);
		Json->SetStringField(TEXT("type"), TEXT("float"));
		Json->SetNumberField(TEXT("contact_threshold"), 0.5);
		Json->SetStringField(TEXT("contact_intervals_evidence"), TEXT("authored_contact_curve"));
		Json->SetBoolField(TEXT("physical_contact_proven"), false);
		Json->SetArrayField(TEXT("authored_contact_intervals"),
			ToJsonIntervals(Curve.ContactIntervals, TEXT("authored_contact_curve")));
		if (bIncludeSamples && Curve.Curve)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Curve.Values.Num());
			for (int32 Index = 0; Index < Curve.Values.Num(); ++Index)
			{
				TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetNumberField(TEXT("sample_index"), Index);
				Value->SetNumberField(TEXT("time"), Curve.Times[Index]);
				Value->SetNumberField(TEXT("value"), Curve.Values[Index]);
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			Json->SetArrayField(TEXT("sample_values"), Values);
		}
		Json->SetStringField(TEXT("side"), Side);
		return Json;
	}
}

void FMonolithLocomotionAnalysisActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("analyze_locomotion"),
		TEXT("Analyze sampled root and foot motion, candidate contact intervals, authored contact curves, and bounded animation events. Results are clip-local evidence, not gameplay-world or IK proof."),
		FMonolithActionHandler::CreateStatic(&HandleAnalyzeLocomotion),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UAnimSequence asset path to analyze"))
			.Required(TEXT("left_foot_bone"), TEXT("string"), TEXT("Required evaluated-pose left foot bone name"))
			.Required(TEXT("right_foot_bone"), TEXT("string"), TEXT("Required evaluated-pose right foot bone name"))
			.Optional(TEXT("root_bone"), TEXT("string"), TEXT("Must name the actual skeleton root at reference bone index 0 (defaults to that root); child bones are rejected"))
			.Optional(TEXT("sample_rate"), TEXT("integer"), TEXT("Sample rate in Hz, integer range 1..120 (default 30)"), TEXT("30"))
			.Optional(TEXT("start_time"), TEXT("number"), TEXT("Inclusive start time in seconds (default 0)"), TEXT("0"))
			.Optional(TEXT("end_time"), TEXT("number"), TEXT("Inclusive end time in seconds (default sequence duration)"))
			.Optional(TEXT("include_samples"), TEXT("boolean"), TEXT("Include sampled transforms, speeds, and curve values (default false)"), TEXT("false"))
			.Optional(TEXT("ground_height_threshold"), TEXT("number"), TEXT("Height above the clip-min sampled foot Z reference in cm (default 3)"), TEXT("3"))
			.Optional(TEXT("contact_speed_threshold"), TEXT("number"), TEXT("Maximum planar foot speed for geometric candidates in cm/s (default 20)"), TEXT("20"))
			.Optional(TEXT("sliding_speed_threshold"), TEXT("number"), TEXT("Planar foot speed above which candidate intervals are flagged in cm/s (default 5)"), TEXT("5"))
			.Optional(TEXT("min_contact_duration"), TEXT("number"), TEXT("Minimum contiguous contact interval duration in seconds (default 0.05)"), TEXT("0.05"))
			.Optional(TEXT("left_contact_curve"), TEXT("string"), TEXT("Optional authored left contact float curve (default contact_l)"), TEXT("contact_l"))
			.Optional(TEXT("right_contact_curve"), TEXT("string"), TEXT("Optional authored right contact float curve (default contact_r)"), TEXT("contact_r"))
			.Build());
	Registry.SetActionAnnotations(TEXT("animation"), TEXT("analyze_locomotion"),
		/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
		TEXT("Analyze animation locomotion"));
}

FMonolithActionResult FMonolithLocomotionAnalysisActions::HandleAnalyzeLocomotion(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("missing action parameters"));
	}
	FString Error;
	FString AssetPath;
	FString LeftFootName;
	FString RightFootName;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error)
		|| !ReadRequiredString(Params, TEXT("left_foot_bone"), LeftFootName, Error)
		|| !ReadRequiredString(Params, TEXT("right_foot_bone"), RightFootName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	UAnimSequence* Sequence = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AssetPath);
	if (!Sequence)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("UAnimSequence not found: %s"), *AssetPath));
	}
	const double Duration = Sequence->GetPlayLength();
	if (!FMath::IsFinite(Duration) || Duration < 0.0)
	{
		return FMonolithActionResult::Error(TEXT("sequence duration is nonfinite or negative"));
	}
	if (Sequence->AdditiveAnimType.GetValue() != EAdditiveAnimationType::AAT_None)
	{
		return FMonolithActionResult::Error(TEXT("additive sequences are not supported by animation.analyze_locomotion"));
	}

	USkeleton* Skeleton = Sequence->GetSkeleton();
	if (!Skeleton)
	{
		return FMonolithActionResult::Error(TEXT("sequence has no skeleton"));
	}
	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
	if (ReferenceSkeleton.GetNum() <= 0)
	{
		return FMonolithActionResult::Error(TEXT("sequence skeleton has no reference bones"));
	}
	const FName SkeletonRootBone = ReferenceSkeleton.GetBoneName(0);
	FString RootBoneName;
	if (!ReadOptionalString(Params, TEXT("root_bone"), SkeletonRootBone.ToString(), RootBoneName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	const FName RootBone(*RootBoneName);
	if (!MonolithLocomotionAnalysis::ValidateRootBoneIdentity(RootBone, SkeletonRootBone, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	const FName LeftFootBone(*LeftFootName);
	const FName RightFootBone(*RightFootName);
	if (LeftFootBone == RightFootBone)
	{
		return FMonolithActionResult::Error(TEXT("left_foot_bone and right_foot_bone must identify distinct bones"));
	}

	int32 SampleRate = 30;
	double StartTime = 0.0;
	double EndTime = Duration;
	double GroundHeightThreshold = 3.0;
	double ContactSpeedThreshold = 20.0;
	double SlidingSpeedThreshold = 5.0;
	double MinimumContactDuration = 0.05;
	bool bIncludeSamples = false;
	FString LeftCurveName;
	FString RightCurveName;
	if (!ReadSampleRate(Params, SampleRate, Error)
		|| !ReadNumber(Params, TEXT("start_time"), 0.0, StartTime, Error)
		|| !ReadNumber(Params, TEXT("end_time"), Duration, EndTime, Error)
		|| !ReadBoolean(Params, TEXT("include_samples"), false, bIncludeSamples, Error)
		|| !ReadNumber(Params, TEXT("ground_height_threshold"), 3.0, GroundHeightThreshold, Error)
		|| !ReadNumber(Params, TEXT("contact_speed_threshold"), 20.0, ContactSpeedThreshold, Error)
		|| !ReadNumber(Params, TEXT("sliding_speed_threshold"), 5.0, SlidingSpeedThreshold, Error)
		|| !ReadNumber(Params, TEXT("min_contact_duration"), 0.05, MinimumContactDuration, Error)
		|| !ReadOptionalString(Params, TEXT("left_contact_curve"), TEXT("contact_l"), LeftCurveName, Error)
		|| !ReadOptionalString(Params, TEXT("right_contact_curve"), TEXT("contact_r"), RightCurveName, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (EndTime > Duration || StartTime > EndTime
		|| GroundHeightThreshold < 0.0 || ContactSpeedThreshold < 0.0
		|| SlidingSpeedThreshold < 0.0 || MinimumContactDuration < 0.0)
	{
		return FMonolithActionResult::Error(TEXT("time range must be within sequence duration and all thresholds must be nonnegative"));
	}

	TArray<double> Times;
	if (!MonolithLocomotionAnalysis::BuildSampleTimes(StartTime, EndTime, SampleRate, Times, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Sequence->AuthoredSyncMarkers.Num() > MonolithLocomotionAnalysis::MaxSampleCount)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("authored sync marker count %d exceeds the %d-event limit"),
			Sequence->AuthoredSyncMarkers.Num(), MonolithLocomotionAnalysis::MaxSampleCount));
	}
	if (Sequence->Notifies.Num() > MonolithLocomotionAnalysis::MaxSampleCount)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("authored notify count %d exceeds the %d-event limit"),
			Sequence->Notifies.Num(), MonolithLocomotionAnalysis::MaxSampleCount));
	}
	const IAnimationDataModel* DataModel = Sequence->GetDataModel();
	if (!DataModel)
	{
		return FMonolithActionResult::Error(TEXT("sequence has no animation data model"));
	}

	const bool bPackageDirtyBefore = Sequence->GetOutermost() && Sequence->GetOutermost()->IsDirty();
	FAnimPoseEvaluationOptions EvaluationOptions;
	EvaluationOptions.EvaluationType = EAnimDataEvalType::Raw;
	EvaluationOptions.bShouldRetarget = true;
	EvaluationOptions.bExtractRootMotion = false;
	EvaluationOptions.bIncorporateRootMotionIntoPose = true;
	EvaluationOptions.OptionalSkeletalMesh = nullptr;
	EvaluationOptions.bRetrieveAdditiveAsFullPose = true;
	EvaluationOptions.bEvaluateCurves = false;

	TArray<FAnimPose> Poses;
	UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Sequence, Times, EvaluationOptions, Poses);
	if (Poses.Num() != Times.Num())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("pose evaluation returned %d poses for %d requested sample times"),
			Poses.Num(), Times.Num()));
	}

	int32 RootBoneIndex = INDEX_NONE;
	int32 LeftFootIndex = INDEX_NONE;
	int32 RightFootIndex = INDEX_NONE;
	FString CanonicalRootBone;
	FString CanonicalLeftFootBone;
	FString CanonicalRightFootBone;
	TArray<MonolithLocomotionAnalysis::FSample> Samples;
	Samples.SetNum(Times.Num());
	for (int32 Index = 0; Index < Poses.Num(); ++Index)
	{
		const FAnimPose& Pose = Poses[Index];
		if (!Pose.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("pose evaluation produced an invalid pose at sample %d (%.9f seconds)"),
				Index, Times[Index]));
		}

		TArray<FName> PoseBoneNames;
		UAnimPoseExtensions::GetBoneNames(Pose, PoseBoneNames);
		const int32 SampleRootIndex = PoseBoneNames.Find(RootBone);
		const int32 SampleLeftIndex = PoseBoneNames.Find(LeftFootBone);
		const int32 SampleRightIndex = PoseBoneNames.Find(RightFootBone);
		if (SampleRootIndex == INDEX_NONE || SampleLeftIndex == INDEX_NONE || SampleRightIndex == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("root/foot bone is absent from evaluated pose at sample %d (%.9f seconds); evaluated pose bones are required"),
				Index, Times[Index]));
		}
		if (Index == 0)
		{
			RootBoneIndex = SampleRootIndex;
			LeftFootIndex = SampleLeftIndex;
			RightFootIndex = SampleRightIndex;
			CanonicalRootBone = PoseBoneNames[RootBoneIndex].ToString();
			CanonicalLeftFootBone = PoseBoneNames[LeftFootIndex].ToString();
			CanonicalRightFootBone = PoseBoneNames[RightFootIndex].ToString();
		}
		else if (SampleRootIndex != RootBoneIndex || SampleLeftIndex != LeftFootIndex
			|| SampleRightIndex != RightFootIndex)
		{
			return FMonolithActionResult::Error(TEXT("evaluated pose bone identities changed across sample times"));
		}

		const FTransform RootTransform = UAnimPoseExtensions::GetBonePose(Pose, RootBone, EAnimPoseSpaces::World);
		const FTransform LeftTransform = UAnimPoseExtensions::GetBonePose(Pose, LeftFootBone, EAnimPoseSpaces::World);
		const FTransform RightTransform = UAnimPoseExtensions::GetBonePose(Pose, RightFootBone, EAnimPoseSpaces::World);
		const FTransform* Transforms[] = { &RootTransform, &LeftTransform, &RightTransform };
		for (const FTransform* Transform : Transforms)
		{
			if (!MonolithLocomotionAnalysis::IsFiniteVector(Transform->GetLocation())
				|| !MonolithLocomotionAnalysis::IsFiniteVector(Transform->GetScale3D())
				|| !MonolithLocomotionAnalysis::IsFiniteQuat(Transform->GetRotation()))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("pose evaluation produced a nonfinite transform at sample %d"), Index));
			}
		}

		MonolithLocomotionAnalysis::FSample& Sample = Samples[Index];
		Sample.Time = Times[Index];
		Sample.RootLocation = RootTransform.GetLocation();
		Sample.RootRotation = RootTransform.GetRotation();
		Sample.LeftFootLocation = LeftTransform.GetLocation();
		Sample.LeftFootRotation = LeftTransform.GetRotation();
		Sample.RightFootLocation = RightTransform.GetLocation();
		Sample.RightFootRotation = RightTransform.GetRotation();
	}

	FCurveRead LeftCurve;
	FCurveRead RightCurve;
	if (!ReadCurve(DataModel, FName(*LeftCurveName), Times, MinimumContactDuration, LeftCurve, Error)
		|| !ReadCurve(DataModel, FName(*RightCurveName), Times, MinimumContactDuration, RightCurve, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithLocomotionAnalysis::FAnalysisResult Analysis;
	if (!MonolithLocomotionAnalysis::AnalyzeSamples(Samples, GroundHeightThreshold,
		ContactSpeedThreshold, SlidingSpeedThreshold, MinimumContactDuration,
		Sequence->HasRootMotion(), Sequence->bForceRootLock, Analysis, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Analysis.bSlidingAssessmentAvailable)
	{
		if (LeftCurve.ContactIntervals.Num() > 0)
		{
			if (!MonolithLocomotionAnalysis::MeasureSliding(Samples, true, LeftCurve.ContactIntervals,
				TEXT("authored_contact_curve"), SlidingSpeedThreshold, Analysis.LeftSliding, Error))
			{
				return FMonolithActionResult::Error(Error);
			}
		}
		else if (!MonolithLocomotionAnalysis::MeasureSliding(Samples, true,
			Analysis.LeftFoot.GeometricCandidateIntervals, TEXT("geometric_candidate"),
			SlidingSpeedThreshold, Analysis.LeftSliding, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (RightCurve.ContactIntervals.Num() > 0)
		{
			if (!MonolithLocomotionAnalysis::MeasureSliding(Samples, false, RightCurve.ContactIntervals,
				TEXT("authored_contact_curve"), SlidingSpeedThreshold, Analysis.RightSliding, Error))
			{
				return FMonolithActionResult::Error(Error);
			}
		}
		else if (!MonolithLocomotionAnalysis::MeasureSliding(Samples, false,
			Analysis.RightFoot.GeometricCandidateIntervals, TEXT("geometric_candidate"),
			SlidingSpeedThreshold, Analysis.RightSliding, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}
	else
	{
		Analysis.LeftSliding.EvidenceSource = LeftCurve.ContactIntervals.Num() > 0
			? TEXT("authored_contact_curve") : TEXT("geometric_candidate");
		Analysis.RightSliding.EvidenceSource = RightCurve.ContactIntervals.Num() > 0
			? TEXT("authored_contact_curve") : TEXT("geometric_candidate");
	}

	for (int32 Index = 0; Index < Sequence->AuthoredSyncMarkers.Num(); ++Index)
	{
		if (!FMath::IsFinite(Sequence->AuthoredSyncMarkers[Index].Time))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("authored sync marker %d has nonfinite time"), Index));
		}
	}
	for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
	{
		if (!FMath::IsFinite(Sequence->Notifies[Index].GetTime()))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("authored notify %d has nonfinite time"), Index));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Markers;
	TArray<TSharedPtr<FJsonValue>> Notifies;
	AddAuthoredMarkers(Sequence, Markers);
	AddAuthoredNotifies(Sequence, Notifies);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	Result->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
	Result->SetNumberField(TEXT("duration_seconds"), Duration);
	Result->SetNumberField(TEXT("sample_count"), Samples.Num());
	Result->SetBoolField(TEXT("package_dirty_before"), bPackageDirtyBefore);
	Result->SetBoolField(TEXT("package_dirty_after"), Sequence->GetOutermost() && Sequence->GetOutermost()->IsDirty());
	Result->SetBoolField(TEXT("gameplay_world_or_ik_proven"), false);
	Result->SetStringField(TEXT("trajectory_scope"), TEXT("raw sampled animation component-space trajectory; no actor velocity, gameplay world placement, or IK evaluation"));

	TSharedPtr<FJsonObject> ParametersJson = MakeShared<FJsonObject>();
	ParametersJson->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	ParametersJson->SetStringField(TEXT("root_bone"), CanonicalRootBone);
	ParametersJson->SetStringField(TEXT("left_foot_bone"), CanonicalLeftFootBone);
	ParametersJson->SetStringField(TEXT("right_foot_bone"), CanonicalRightFootBone);
	ParametersJson->SetNumberField(TEXT("sample_rate"), SampleRate);
	ParametersJson->SetNumberField(TEXT("start_time"), StartTime);
	ParametersJson->SetNumberField(TEXT("end_time"), EndTime);
	ParametersJson->SetBoolField(TEXT("include_samples"), bIncludeSamples);
	ParametersJson->SetNumberField(TEXT("ground_height_threshold_cm"), GroundHeightThreshold);
	ParametersJson->SetNumberField(TEXT("contact_speed_threshold_cm_s"), ContactSpeedThreshold);
	ParametersJson->SetNumberField(TEXT("sliding_speed_threshold_cm_s"), SlidingSpeedThreshold);
	ParametersJson->SetNumberField(TEXT("min_contact_duration_seconds"), MinimumContactDuration);
	ParametersJson->SetStringField(TEXT("left_contact_curve"), LeftCurveName);
	ParametersJson->SetStringField(TEXT("right_contact_curve"), RightCurveName);
	Result->SetObjectField(TEXT("resolved_parameters"), ParametersJson);

	TSharedPtr<FJsonObject> EvaluationJson = MakeShared<FJsonObject>();
	EvaluationJson->SetStringField(TEXT("data"), TEXT("raw"));
	EvaluationJson->SetBoolField(TEXT("retarget"), true);
	EvaluationJson->SetBoolField(TEXT("extract_root_motion"), false);
	EvaluationJson->SetBoolField(TEXT("incorporate_root_motion_into_pose"), true);
	EvaluationJson->SetBoolField(TEXT("optional_mesh"), false);
	EvaluationJson->SetBoolField(TEXT("retrieve_additive_as_full_pose"), true);
	EvaluationJson->SetBoolField(TEXT("evaluate_curves_in_pose"), false);
	EvaluationJson->SetBoolField(TEXT("additive_sequence_supported"), false);
	EvaluationJson->SetBoolField(TEXT("sequence_root_motion_enabled"), Sequence->HasRootMotion());
	EvaluationJson->SetBoolField(TEXT("force_root_lock"), Sequence->bForceRootLock);
	Result->SetObjectField(TEXT("evaluation_settings"), EvaluationJson);

	TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();
	RootJson->SetStringField(TEXT("bone_name"), CanonicalRootBone);
	RootJson->SetNumberField(TEXT("evaluated_pose_bone_index"), RootBoneIndex);
	RootJson->SetNumberField(TEXT("planar_path_length_cm"), Analysis.RootPlanarPathLength);
	RootJson->SetNumberField(TEXT("planar_net_displacement_cm"), Analysis.RootPlanarNetDisplacement);
	RootJson->SetNumberField(TEXT("cumulative_shortest_yaw_change_degrees"), Analysis.RootCumulativeShortestYawChange);
	RootJson->SetNumberField(TEXT("net_shortest_yaw_change_degrees"), Analysis.RootNetShortestYawChange);
	RootJson->SetStringField(TEXT("trajectory_space"), TEXT("animation component space"));
	Result->SetObjectField(TEXT("root_trajectory"), RootJson);

	TSharedPtr<FJsonObject> FeetJson = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> LeftFootJson = ToJsonFootSummary(Analysis.LeftFoot, CanonicalLeftFootBone, LeftFootIndex);
	TSharedPtr<FJsonObject> RightFootJson = ToJsonFootSummary(Analysis.RightFoot, CanonicalRightFootBone, RightFootIndex);
	LeftFootJson->SetObjectField(TEXT("authored_contact_curve"), ToJsonCurve(LeftCurve, bIncludeSamples, TEXT("left")));
	RightFootJson->SetObjectField(TEXT("authored_contact_curve"), ToJsonCurve(RightCurve, bIncludeSamples, TEXT("right")));
	FeetJson->SetObjectField(TEXT("left"), LeftFootJson);
	FeetJson->SetObjectField(TEXT("right"), RightFootJson);
	FeetJson->SetStringField(TEXT("geometric_candidate_evidence"), TEXT("low height relative to each foot's clip-min sampled Z AND planar speed at or below contact_speed_threshold"));
	FeetJson->SetStringField(TEXT("height_reference"), TEXT("clip_min_sampled_foot_z, not a floor estimate"));
	FeetJson->SetBoolField(TEXT("physical_contact_proven"), false);
	Result->SetObjectField(TEXT("feet"), FeetJson);

	TSharedPtr<FJsonObject> AuthoredEventsJson = MakeShared<FJsonObject>();
	AuthoredEventsJson->SetNumberField(TEXT("sync_marker_count"), Markers.Num());
	AuthoredEventsJson->SetArrayField(TEXT("sync_markers"), Markers);
	AuthoredEventsJson->SetNumberField(TEXT("notify_count"), Notifies.Num());
	AuthoredEventsJson->SetArrayField(TEXT("notifies"), Notifies);
	AuthoredEventsJson->SetStringField(TEXT("interpretation"), TEXT("bounded authored point events; not ground-truth contact intervals"));
	Result->SetObjectField(TEXT("authored_events"), AuthoredEventsJson);

	TSharedPtr<FJsonObject> SlidingJson = MakeShared<FJsonObject>();
	SlidingJson->SetBoolField(TEXT("available"), Analysis.bSlidingAssessmentAvailable);
	SlidingJson->SetNumberField(TEXT("sliding_speed_threshold_cm_s"), SlidingSpeedThreshold);
	SlidingJson->SetBoolField(TEXT("root_motion_enabled"), Sequence->HasRootMotion());
	SlidingJson->SetBoolField(TEXT("force_root_lock"), Sequence->bForceRootLock);
	SlidingJson->SetNumberField(TEXT("sampled_root_planar_path_cm"), Analysis.RootPlanarPathLength);
	if (Analysis.bSlidingAssessmentAvailable)
	{
		SlidingJson->SetStringField(TEXT("evidence_basis"), TEXT("authored contact curve when it has qualifying intervals; otherwise geometric candidate"));
	}
	else
	{
		SlidingJson->SetStringField(TEXT("reason"), Analysis.SlidingUnavailableReason);
	}
	SlidingJson->SetObjectField(TEXT("left"), ToJsonSlidingFoot(Analysis.LeftSliding, Analysis.bSlidingAssessmentAvailable));
	SlidingJson->SetObjectField(TEXT("right"), ToJsonSlidingFoot(Analysis.RightSliding, Analysis.bSlidingAssessmentAvailable));
	SlidingJson->SetBoolField(TEXT("physical_contact_or_gameplay_sliding_proven"), false);
	Result->SetObjectField(TEXT("sliding_assessment"), SlidingJson);

	if (bIncludeSamples)
	{
		TArray<TSharedPtr<FJsonValue>> JsonSamples;
		JsonSamples.Reserve(Samples.Num());
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const MonolithLocomotionAnalysis::FSample& Sample = Samples[Index];
			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetNumberField(TEXT("time"), Sample.Time);
			Json->SetObjectField(TEXT("root_translation_cm"), ToJsonVector(Sample.RootLocation));
			Json->SetObjectField(TEXT("root_rotation_quaternion"), ToJsonQuat(Sample.RootRotation));
			Json->SetObjectField(TEXT("left_foot_translation_cm"), ToJsonVector(Sample.LeftFootLocation));
			Json->SetObjectField(TEXT("left_foot_rotation_quaternion"), ToJsonQuat(Sample.LeftFootRotation));
			Json->SetObjectField(TEXT("right_foot_translation_cm"), ToJsonVector(Sample.RightFootLocation));
			Json->SetObjectField(TEXT("right_foot_rotation_quaternion"), ToJsonQuat(Sample.RightFootRotation));
			if (Index > 0)
			{
				const MonolithLocomotionAnalysis::FSample& Previous = Samples[Index - 1];
				const double DeltaTime = Sample.Time - Previous.Time;
				Json->SetNumberField(TEXT("dt_from_previous_seconds"), DeltaTime);
				Json->SetNumberField(TEXT("left_foot_planar_speed_from_previous_cm_s"),
					FVector2D(Sample.LeftFootLocation.X - Previous.LeftFootLocation.X,
						Sample.LeftFootLocation.Y - Previous.LeftFootLocation.Y).Length() / DeltaTime);
				Json->SetNumberField(TEXT("right_foot_planar_speed_from_previous_cm_s"),
					FVector2D(Sample.RightFootLocation.X - Previous.RightFootLocation.X,
						Sample.RightFootLocation.Y - Previous.RightFootLocation.Y).Length() / DeltaTime);
			}
			if (LeftCurve.Curve)
			{
				Json->SetNumberField(TEXT("left_contact_curve_value"), LeftCurve.Values[Index]);
			}
			if (RightCurve.Curve)
			{
				Json->SetNumberField(TEXT("right_contact_curve_value"), RightCurve.Values[Index]);
			}
			JsonSamples.Add(MakeShared<FJsonValueObject>(Json));
		}
		Result->SetArrayField(TEXT("samples"), JsonSamples);
	}
	return FMonolithActionResult::Success(Result);
}
