#pragma once

#include "CoreMinimal.h"

class UAnimMontage;
class UCurveVector;

#include "MonolithChooserTestTypes.generated.h"

UENUM(BlueprintType)
enum class EMonolithChooserTestAttackVariant : uint8
{
	Ground = 0,
	Air = 1
};

USTRUCT(BlueprintType)
struct FMonolithChooserTestAttackRowData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	float Damage = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	float StaminaCost = 15.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	TObjectPtr<UCurveVector> MovementPathOffsetCurve;
};

USTRUCT(BlueprintType)
struct FMonolithChooserTestChooserContext
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	FName MovesetId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	float AttackIndexValue = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	EMonolithChooserTestAttackVariant AttackVariant = EMonolithChooserTestAttackVariant::Ground;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	FMonolithChooserTestAttackRowData AttackRowData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	bool bIsNPC = false;
};

UCLASS(BlueprintType)
class UMonolithChooserTestContextObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	FMonolithChooserTestChooserContext ChooserContext;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Chooser|Test")
	UAnimMontage* AirAttackMontage = nullptr;
};
