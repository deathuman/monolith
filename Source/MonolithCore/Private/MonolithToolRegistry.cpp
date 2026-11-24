#include "MonolithToolRegistry.h"
#include "MonolithUIRecovery.h"
#include "HAL/ThreadSafeCounter.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithFuzzyMatch.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/SecureHash.h"

#include <limits>

namespace MonolithMetadataGeneration
{
	enum class ECanonicalTag : uint8
	{
		Null = 0x00,
		Boolean = 0x01,
		Number = 0x02,
		String = 0x03,
		Array = 0x04,
		Object = 0x05
	};

	static void AppendUInt64BigEndian(TArray<uint8>& OutBytes, uint64 Value)
	{
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			OutBytes.Add(static_cast<uint8>((Value >> Shift) & 0xff));
		}
	}

	/** Each value is tag + uint64 big-endian payload byte length + payload. */
	static void AppendToken(TArray<uint8>& OutBytes, ECanonicalTag Tag, const uint8* Payload, uint64 PayloadLength)
	{
		OutBytes.Add(static_cast<uint8>(Tag));
		AppendUInt64BigEndian(OutBytes, PayloadLength);
		if (PayloadLength > 0)
		{
			OutBytes.Append(Payload, static_cast<int32>(PayloadLength));
		}
	}

	static void AppendStringToken(TArray<uint8>& OutBytes, const FString& Value)
	{
		// Include FString's terminator in the explicit source range so the conversion strips only
		// that final terminator and preserves embedded null characters, including a trailing one.
		FTCHARToUTF8 Utf8(*Value, Value.Len() + 1);
		AppendToken(OutBytes, ECanonicalTag::String,
			reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<uint64>(Utf8.Length()));
	}

	static FString CopyJsonObjectKey(const FString& Key)
	{
		return Key;
	}

	static FString CopyJsonObjectKey(const UE::FSharedString& Key)
	{
		return FString(Key.ToView());
	}

	static FString JsonPathMember(const FString& Parent, const FString& Member)
	{
		return Parent.IsEmpty() ? Member : Parent + TEXT(".") + Member;
	}

	static bool EncodeJsonObject(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		const FString& Path,
		FString& OutError);

	static bool EncodeJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		TArray<uint8>& OutBytes,
		const FString& Path,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: null value pointer."), *Path);
			return false;
		}

		TArray<uint8> Payload;
		switch (Value->Type)
		{
		case EJson::Null:
			AppendToken(OutBytes, ECanonicalTag::Null, nullptr, 0);
			return true;

		case EJson::Boolean:
			Payload.Add(Value->AsBool() ? 1 : 0);
			AppendToken(OutBytes, ECanonicalTag::Boolean, Payload.GetData(), Payload.Num());
			return true;

		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			if (!FMath::IsFinite(Number))
			{
				OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: non-finite number."), *Path);
				return false;
			}
			static_assert(sizeof(double) == sizeof(uint64) && std::numeric_limits<double>::is_iec559,
				"Metadata number encoding requires IEEE-754 binary64.");
			uint64 Bits = 0;
			FMemory::Memcpy(&Bits, &Number, sizeof(Bits));
			AppendUInt64BigEndian(Payload, Bits);
			AppendToken(OutBytes, ECanonicalTag::Number, Payload.GetData(), Payload.Num());
			return true;
		}

		case EJson::String:
		{
			FString StringValue;
			if (!Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: unreadable string value."), *Path);
				return false;
			}
			AppendStringToken(OutBytes, StringValue);
			return true;
		}

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Value->TryGetArray(Values) || !Values)
			{
				OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: unreadable array."), *Path);
				return false;
			}
			AppendUInt64BigEndian(Payload, static_cast<uint64>(Values->Num()));
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				TArray<uint8> EncodedValue;
				if (!EncodeJsonValue((*Values)[Index], EncodedValue,
					FString::Printf(TEXT("%s[%d]"), *Path, Index), OutError))
				{
					return false;
				}
				Payload.Append(EncodedValue);
			}
			AppendToken(OutBytes, ECanonicalTag::Array, Payload.GetData(), Payload.Num());
			return true;
		}

		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: unreadable object."), *Path);
				return false;
			}
			return EncodeJsonObject(*Object, OutBytes, Path, OutError);
		}

		case EJson::None:
		default:
			OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: unsupported JSON value type %d."),
				*Path, static_cast<int32>(Value->Type));
			return false;
		}
	}

	static bool EncodeJsonObject(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		const FString& Path,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: null object."), *Path);
			return false;
		}

		struct FEncodedField
		{
			FString Key;
			TSharedPtr<FJsonValue> Value;
		};
		TArray<FEncodedField> Fields;
		Fields.Reserve(Object->Values.Num());
		for (const auto& Pair : Object->Values)
		{
			FEncodedField Field;
			Field.Key = CopyJsonObjectKey(Pair.Key);
			Field.Value = Pair.Value;
			Fields.Add(MoveTemp(Field));
		}
		Fields.Sort([](const FEncodedField& Left, const FEncodedField& Right)
		{
			return Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive) < 0;
		});

		TArray<uint8> Payload;
		AppendUInt64BigEndian(Payload, static_cast<uint64>(Fields.Num()));
		for (const FEncodedField& Field : Fields)
		{
			AppendStringToken(Payload, Field.Key);
			if (!EncodeJsonValue(Field.Value, Payload, JsonPathMember(Path, Field.Key), OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("Invalid metadata JSON at %s: missing object value."),
						*JsonPathMember(Path, Field.Key));
				}
				return false;
			}
		}
		AppendToken(OutBytes, ECanonicalTag::Object, Payload.GetData(), Payload.Num());
		return true;
	}

	static TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object);

	static TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return nullptr;
		}

		switch (Value->Type)
		{
		case EJson::Null:
			return MakeShared<FJsonValueNull>();
		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(Value->AsBool());
		case EJson::Number:
			return MakeShared<FJsonValueNumber>(Value->AsNumber());
		case EJson::String:
			return MakeShared<FJsonValueString>(Value->AsString());
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Value->TryGetArray(Values) || !Values)
			{
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Cloned;
			Cloned.Reserve(Values->Num());
			for (const TSharedPtr<FJsonValue>& Child : *Values)
			{
				Cloned.Add(CloneJsonValue(Child));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Cloned));
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				return nullptr;
			}
			return MakeShared<FJsonValueObject>(CloneJsonObject(*Object));
		}
		case EJson::None:
		default:
			return nullptr;
		}
	}

	static TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Cloned = MakeShared<FJsonObject>();
		for (const auto& Pair : Object->Values)
		{
			Cloned->SetField(CopyJsonObjectKey(Pair.Key), CloneJsonValue(Pair.Value));
		}
		return Cloned;
	}

	static FString HashToLowerHex(const TArray<uint8>& Bytes)
	{
		uint8 Hash[FSHA1::DigestSize];
		FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num()), Hash);
		FString Hex;
		Hex.Reserve(FSHA1::DigestSize * 2);
		for (uint8 Byte : Hash)
		{
			Hex += FString::Printf(TEXT("%02x"), static_cast<uint32>(Byte));
		}
		return Hex;
	}

	/**
	 * Canonical structure: a typed JSON root containing ordered action and active-dispatcher
	 * arrays. Every value is tag + uint64 big-endian payload-byte length + payload; strings
	 * are UTF-8, object keys are case-sensitive sorted, arrays retain order, booleans use
	 * one byte, numbers use finite IEEE-754 binary64 bits in big-endian order, and null,
	 * arrays, and objects have distinct tags. Container payloads start with a uint64 item
	 * count. The root includes descriptions, categories, schemas/aliases and annotations.
	 * The structural SHA-1 is lower-case hex. The final SHA-1 input is the same typed
	 * encoding of a versioned domain string, that structural hex string, and an array of
	 * handler-bound booleans ordered by namespace/action; its wire form is `v1:` plus the
	 * 40 lower-case hex digits. This is an identity digest, not a security check.
	 */
	static bool BuildStructuralDigest(
		const TArray<FMonolithActionInfo>& InActions,
		const TMap<FString, FMonolithDispatcherAnnotations>& DispatcherAnnotations,
		FString& OutDigest,
		FString& OutError)
	{
		TArray<FMonolithActionInfo> Actions = InActions;
		Actions.Sort([](const FMonolithActionInfo& Left, const FMonolithActionInfo& Right)
		{
			const int32 NamespaceOrder = Left.Namespace.Compare(Right.Namespace, ESearchCase::CaseSensitive);
			return NamespaceOrder == 0
				? Left.Action.Compare(Right.Action, ESearchCase::CaseSensitive) < 0
				: NamespaceOrder < 0;
		});

		TSet<FString> ActiveNamespaceSet;
		TArray<TSharedPtr<FJsonValue>> ActionValues;
		ActionValues.Reserve(Actions.Num());
		for (const FMonolithActionInfo& Info : Actions)
		{
			ActiveNamespaceSet.Add(Info.Namespace);
			TSharedPtr<FJsonObject> ActionObject = MakeShared<FJsonObject>();
			ActionObject->SetStringField(TEXT("namespace"), Info.Namespace);
			ActionObject->SetStringField(TEXT("action"), Info.Action);
			ActionObject->SetStringField(TEXT("description"), Info.Description);
			ActionObject->SetStringField(TEXT("category"), Info.Category);
			if (Info.ParamSchema.IsValid())
			{
				ActionObject->SetObjectField(TEXT("param_schema"), Info.ParamSchema);
			}
			else
			{
				ActionObject->SetField(TEXT("param_schema"), MakeShared<FJsonValueNull>());
			}

			TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
			Annotations->SetBoolField(TEXT("read_only"), Info.bReadOnlyHint);
			Annotations->SetBoolField(TEXT("destructive"), Info.bDestructiveHint);
			Annotations->SetBoolField(TEXT("idempotent"), Info.bIdempotentHint);
			Annotations->SetStringField(TEXT("title"), Info.Title);
			ActionObject->SetObjectField(TEXT("annotations"), Annotations);
			ActionValues.Add(MakeShared<FJsonValueObject>(ActionObject));
		}

		TArray<FString> ActiveNamespaces = ActiveNamespaceSet.Array();
		ActiveNamespaces.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
		});
		TArray<TSharedPtr<FJsonValue>> DispatcherValues;
		DispatcherValues.Reserve(ActiveNamespaces.Num());
		for (const FString& Namespace : ActiveNamespaces)
		{
			const FMonolithDispatcherAnnotations* Found = DispatcherAnnotations.Find(Namespace);
			const FMonolithDispatcherAnnotations Annotations = Found
				? *Found : FMonolithDispatcherAnnotations{};
			TSharedPtr<FJsonObject> DispatcherObject = MakeShared<FJsonObject>();
			DispatcherObject->SetStringField(TEXT("namespace"), Namespace);
			DispatcherObject->SetBoolField(TEXT("read_only"), Annotations.bReadOnlyHint);
			DispatcherObject->SetBoolField(TEXT("destructive"), Annotations.bDestructiveHint);
			DispatcherObject->SetBoolField(TEXT("idempotent"), Annotations.bIdempotentHint);
			DispatcherObject->SetStringField(TEXT("title"), Annotations.Title);
			DispatcherValues.Add(MakeShared<FJsonValueObject>(DispatcherObject));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("format"), TEXT("monolith-metadata-structural-v1"));
		Root->SetArrayField(TEXT("actions"), ActionValues);
		Root->SetArrayField(TEXT("dispatchers"), DispatcherValues);
		TArray<uint8> CanonicalBytes;
		if (!EncodeJsonObject(Root, CanonicalBytes, TEXT("metadata"), OutError))
		{
			OutDigest.Reset();
			return false;
		}
		OutDigest = HashToLowerHex(CanonicalBytes);
		return true;
	}
}

// =============================================================================
//  FMonolithParamSchema — K2 alias rewriting + K3 unknown-key detection
// =============================================================================

bool FMonolithParamSchema::ApplyAliases(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutCollision)
{
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	for (const auto& Pair : Schema->Values)
	{
		const FString Canonical = MonolithKeyToString(Pair.Key);

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if (!(*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) || !AliasArr)
		{
			continue;
		}

		const bool bCanonicalPresent = Params->HasField(Canonical);

		for (const TSharedPtr<FJsonValue>& AliasVal : *AliasArr)
		{
			FString Alias;
			if (!AliasVal.IsValid() || !AliasVal->TryGetString(Alias))
			{
				continue;
			}

			if (!Params->HasField(Alias))
			{
				continue;
			}

			if (bCanonicalPresent)
			{
				OutCollision = FString::Printf(
					TEXT("Param collision: both canonical '%s' and alias '%s' supplied. Use only one. — supply either the canonical param OR its alias, never both."),
					*Canonical, *Alias);
				return false;
			}

			// Rewrite alias -> canonical (preserve value).
			TSharedPtr<FJsonValue> Val = Params->TryGetField(Alias);
			if (Val.IsValid())
			{
				Params->SetField(Canonical, Val);
			}
			Params->RemoveField(Alias);
			break; // Only one alias rewrite per canonical.
		}
	}

	return true;
}

TArray<FString> FMonolithParamSchema::FindUnknownKeys(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Unknown;
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return Unknown;
	}

	// Build the set of allowed keys: canonical names + their declared aliases.
	TSet<FString> Allowed;
	for (const auto& Pair : Schema->Values)
	{
		Allowed.Add(MonolithKeyToString(Pair.Key));

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr)
		{
			for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
			{
				FString A;
				if (AV.IsValid() && AV->TryGetString(A))
				{
					Allowed.Add(A);
				}
			}
		}
	}

	// Legacy wbp_path/asset_path back-compat: allow asset_path everywhere.
	Allowed.Add(TEXT("asset_path"));

	// Survivor B (plan §3.B) — universal response-shaping params. Allow these
	// on EVERY action so the K3 STRICT_PARAMS=1 path does not hard-fail on
	// `_fields` / `_omit` / `_compact_json`. The post-filter at the bottom
	// of ExecuteAction consumes + acts on them.
	Allowed.Add(TEXT("_fields"));
	Allowed.Add(TEXT("_omit"));
	Allowed.Add(TEXT("_compact_json"));
	// Phase 1.1 (RI ergonomics handover #3) — nested-payload shaping params.
	// `_row_fields` filters each row of the unique top-level list payload;
	// `_path_fields` retains only matching dotted leaves. Same allowlist
	// treatment as the original three.
	Allowed.Add(TEXT("_row_fields"));
	Allowed.Add(TEXT("_path_fields"));

	for (const auto& Pair : Params->Values)
	{
		const FString PairKeyStr = MonolithKeyToString(Pair.Key);
		if (!Allowed.Contains(PairKeyStr))
		{
			Unknown.Add(PairKeyStr);
		}
	}

	return Unknown;
}

bool FMonolithParamSchema::IsStrictParamsEnabled()
{
	const FString Val = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	return Val == TEXT("1");
}

// =============================================================================
//  FMonolithToolRegistry
// =============================================================================

FMonolithToolRegistry& FMonolithToolRegistry::Get()
{
	static FMonolithToolRegistry Instance;
	return Instance;
}

void FMonolithToolRegistry::RegisterAction(
	const FString& Namespace,
	const FString& Action,
	const FString& Description,
	const FMonolithActionHandler& Handler,
	const TSharedPtr<FJsonObject>& ParamSchema,
	const FString& Category)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);

	if (Actions.Contains(Key))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Overwriting existing action: %s"), *Key);
	}

	FRegisteredAction RegAction;
	RegAction.Info.Namespace = Namespace;
	RegAction.Info.Action = Action;
	RegAction.Info.Description = Description;
	RegAction.Info.Category = Category;
	RegAction.Info.ParamSchema = MonolithMetadataGeneration::CloneJsonObject(ParamSchema);
	RegAction.Handler = Handler;

	Actions.Add(Key, MoveTemp(RegAction));
	NamespaceActions.FindOrAdd(Namespace).AddUnique(Key);
	InvalidateMetadataStructuralDigest();

	UE_LOG(LogMonolith, Verbose, TEXT("Registered action: %s — %s"), *Key, *Description);
}

void FMonolithToolRegistry::UnregisterNamespace(const FString& Namespace)
{
	FScopeLock Lock(&RegistryLock);

	if (TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			Actions.Remove(Key);
		}
		UE_LOG(LogMonolith, Log, TEXT("Unregistered namespace: %s (%d actions)"), *Namespace, Keys->Num());
		NamespaceActions.Remove(Namespace);
		InvalidateMetadataStructuralDigest();
	}
}

namespace
{
	FThreadSafeCounter MonolithActionDepth;
	TAtomic<bool> bMonolithUIRecoveryOnly(false);
	struct FScopedMonolithAction
	{
		FScopedMonolithAction() { MonolithActionDepth.Increment(); }
		~FScopedMonolithAction() { MonolithActionDepth.Decrement(); }
	};
}

bool IsMonolithActionExecuting() { return MonolithActionDepth.GetValue() > 0; }
bool SetMonolithUIRecoveryOnly(bool bEnabled) { return bMonolithUIRecoveryOnly.Exchange(bEnabled); }

FMonolithActionResult FMonolithToolRegistry::ExecuteAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	if (bMonolithUIRecoveryOnly.Load())
	{
		const bool bRecoveryAction = Namespace == TEXT("editor") &&
			(Action == TEXT("get_slate_snapshot") || Action == TEXT("capture_slate_window")
			 || Action == TEXT("interact_slate") || Action == TEXT("get_slate_operation"));
		if (!bRecoveryAction && !(Namespace == TEXT("describe") && Action == TEXT("action_schema")))
			return FMonolithActionResult::Error(TEXT("Modal recovery is active; only Slate observation, operation status and queued UI recovery are available."));
	}
	FScopedMonolithAction ExecutionScope;
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);
	FRegisteredAction* RegAction = Actions.Find(Key);

	if (!RegAction)
	{
		// Survivor C (plan §3.C) — fuzzy-match top-3 suggestions on Unknown
		// action / Unknown namespace. CRITICAL: snapshot the relevant keys
		// under RegistryLock, then DROP the lock before sweeping Levenshtein.
		// Holding the lock during the O(N·|key|·|action|) sweep would block
		// every concurrent tools/list and tools/call (plan §10 Threading).
		const bool bKnownNamespace = NamespaceActions.Contains(Namespace);
		TArray<FString> CandidateKeys;
		FString FuzzyNeedle;
		FString SuggestionKind; // "action" | "namespace" — drives JSON key in suggestions
		if (bKnownNamespace)
		{
			// Action typo within a known namespace. Snapshot the bare action
			// names for this namespace and Levenshtein against `Action`.
			if (const TArray<FString>* NsKeys = NamespaceActions.Find(Namespace))
			{
				CandidateKeys.Reserve(NsKeys->Num());
				for (const FString& FullKey : *NsKeys)
				{
					// FullKey is "namespace.action" — slice the action half.
					int32 DotIdx = INDEX_NONE;
					if (FullKey.FindChar(TEXT('.'), DotIdx))
					{
						CandidateKeys.Add(FullKey.Mid(DotIdx + 1));
					}
					else
					{
						CandidateKeys.Add(FullKey);
					}
				}
			}
			FuzzyNeedle = Action;
			SuggestionKind = TEXT("action");
		}
		else
		{
			// Namespace typo. Snapshot all known namespace names.
			NamespaceActions.GetKeys(CandidateKeys);
			FuzzyNeedle = Namespace;
			SuggestionKind = TEXT("namespace");
		}

		// Drop the lock BEFORE the Levenshtein sweep. The snapshot is now ours.
		Lock.Unlock();

		TArray<MonolithFuzzyMatchDetail::FFuzzyCandidate> Top3 =
			MonolithFuzzyMatchDetail::ScoreFuzzyMatches(FuzzyNeedle, CandidateKeys, /*TopN=*/3);

		// Build the suggestions JSON payload regardless of whether any matched —
		// an empty array on the wire is still informative (the agent learns the
		// needle is novel, not just misspelled).
		TArray<TSharedPtr<FJsonValue>> SuggestionArray;
		for (const MonolithFuzzyMatchDetail::FFuzzyCandidate& C : Top3)
		{
			TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
			SObj->SetStringField(SuggestionKind, C.Key);
			SObj->SetNumberField(TEXT("score"), C.Score);
			SuggestionArray.Add(MakeShared<FJsonValueObject>(SObj));
		}

		TSharedPtr<FJsonObject> ErrorDataObj = MakeShared<FJsonObject>();
		ErrorDataObj->SetArrayField(TEXT("suggestions"), SuggestionArray);
		ErrorDataObj->SetStringField(TEXT("kind"), SuggestionKind);

		FString MsgSuffix;
		if (Top3.Num() > 0)
		{
			TArray<FString> Names;
			for (const auto& C : Top3) { Names.Add(C.Key); }
			MsgSuffix = FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Names, TEXT(", ")));
		}

		if (bKnownNamespace)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown action: %s.%s — call monolith_discover(\"%s\") to enumerate valid actions in this namespace.%s"),
					*Namespace, *Action, *Namespace, *MsgSuffix),
				FMonolithJsonUtils::ErrMethodNotFound
			).WithErrorData(ErrorDataObj);
		}
		else
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown namespace: %s — call monolith_discover() to enumerate valid namespaces.%s"),
					*Namespace, *MsgSuffix),
				FMonolithJsonUtils::ErrMethodNotFound
			).WithErrorData(ErrorDataObj);
		}
	}

	if (!RegAction->Handler.IsBound())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action handler not bound: %s — this is a Monolith bug; the action is registered but its handler delegate is null. Report at github.com/tumourlove/monolith."), *Key),
			FMonolithJsonUtils::ErrInternalError
		);
	}

	const FMonolithActionInfo& ActionInfo = RegAction->Info;
	TSharedPtr<FJsonObject> EffectiveParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();

	// K2 — alias rewriting BEFORE the required-param check.
	if (ActionInfo.ParamSchema.IsValid())
	{
		FString Collision;
		if (!FMonolithParamSchema::ApplyAliases(ActionInfo.ParamSchema, EffectiveParams, Collision))
		{
			return FMonolithActionResult::Error(Collision, FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// K4 — string-encoded complex-param recovery. Some MCP clients (Claude
	// Code among them) serialize array/object argument values to JSON-encoded
	// strings. batch_execute has long carried a local string-parse fallback for
	// its `operations` param ("Claude Code quirk"); every other array/object
	// param silently arrived as a string and failed its handler's typed field
	// lookup ("<param> array is required" with the value right there). Recover
	// centrally: when the schema declares array/object and the incoming value
	// is a string that parses as that JSON kind, replace it with the parsed
	// value. Strings that don't parse pass through untouched, so a legitimate
	// string value can never be corrupted.
	if (ActionInfo.ParamSchema.IsValid())
	{
		for (const auto& SchemaPair : ActionInfo.ParamSchema->Values)
		{
			const TSharedPtr<FJsonObject>* ParamDefPtr = nullptr;
			if (!SchemaPair.Value->TryGetObject(ParamDefPtr) || !ParamDefPtr) continue;

			FString DeclaredType;
			(*ParamDefPtr)->TryGetStringField(TEXT("type"), DeclaredType);
			const bool bWantsArray  = DeclaredType == TEXT("array");
			const bool bWantsObject = DeclaredType == TEXT("object");
			if (!bWantsArray && !bWantsObject) continue;

			const FString KeyStr = MonolithKeyToString(SchemaPair.Key);
			FString StrVal;
			if (!EffectiveParams->TryGetStringField(KeyStr, StrVal) || StrVal.IsEmpty()) continue;

			if (bWantsArray)
			{
				TArray<TSharedPtr<FJsonValue>> ParsedArr;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrVal);
				if (FJsonSerializer::Deserialize(Reader, ParsedArr))
				{
					EffectiveParams->SetArrayField(KeyStr, ParsedArr);
				}
			}
			else
			{
				TSharedPtr<FJsonObject> ParsedObj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrVal);
				if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
				{
					EffectiveParams->SetObjectField(KeyStr, ParsedObj);
				}
			}
		}
	}

	// Validate required params from schema before dispatching.
	//
	// asset_path is validated like any other required param (issue #157). A small
	// set of legacy, undeclared aliases is still honoured, each scoped to the one
	// namespace whose handlers actually read it in place of asset_path:
	//   - niagara: system_path (the GetAssetPath / NA_GetAssetPath / NL_GetAssetPath
	//              helpers fall back to it)
	//   - project: package_path (find_references / get_asset_details /
	//              get_saved_asset_state fall back to it)
	// When asset_path is absent and the namespace's alias is present, the alias
	// value is rewritten onto asset_path (same shape as the K2 rewrite above), so
	// every handler in that namespace sees it, including ones that only read
	// asset_path. Outside those namespaces the alias is NOT accepted: the loaders
	// there read only asset_path, and letting it through would reproduce the
	// "<Thing> not found: " empty-path error this check exists to prevent.
	if (ActionInfo.ParamSchema.IsValid())
	{
		struct FAssetPathAlias
		{
			const TCHAR* Namespace;
			const TCHAR* Alias;
		};
		static const FAssetPathAlias AssetPathAliases[] = {
			{ TEXT("niagara"), TEXT("system_path") },
			{ TEXT("project"), TEXT("package_path") },
		};

		TArray<FString> Missing;
		for (const auto& Pair : ActionInfo.ParamSchema->Values)
		{
			const FString PairKeyStr = MonolithKeyToString(Pair.Key);

			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
			{
				bool bRequired = false;
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (bRequired && !EffectiveParams->HasField(PairKeyStr))
				{
					// Legacy wbp_path / asset_path aliasing: accept asset_path as substitute for wbp_path
					// (only fires for schemas not migrated to K2 aliases).
					if (PairKeyStr == TEXT("wbp_path") && EffectiveParams->HasField(TEXT("asset_path")))
						continue;

					if (PairKeyStr == TEXT("asset_path"))
					{
						TArray<FString> AcceptedAliases;
						bool bSatisfied = false;
						for (const FAssetPathAlias& Entry : AssetPathAliases)
						{
							if (!Namespace.Equals(Entry.Namespace, ESearchCase::IgnoreCase))
							{
								continue;
							}
							AcceptedAliases.Add(Entry.Alias);
							if (!bSatisfied && EffectiveParams->HasField(Entry.Alias))
							{
								const TSharedPtr<FJsonValue> AliasValue = EffectiveParams->TryGetField(Entry.Alias);
								if (AliasValue.IsValid())
								{
									EffectiveParams->SetField(TEXT("asset_path"), AliasValue);
									EffectiveParams->RemoveField(Entry.Alias);
									bSatisfied = true;
								}
							}
						}
						if (bSatisfied)
						{
							continue;
						}
						if (AcceptedAliases.Num() > 0)
						{
							Missing.Add(FString::Printf(TEXT("asset_path (or alias: %s)"),
								*FString::Join(AcceptedAliases, TEXT(", "))));
							continue;
						}
					}

					Missing.Add(PairKeyStr);
				}
			}
		}
		if (Missing.Num() > 0)
		{
			TArray<FString> Provided;
			for (const auto& P : EffectiveParams->Values) Provided.Add(MonolithKeyToString(P.Key));
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required param(s): [%s]. Provided keys: [%s] — inspect the action's parameter schema via monolith_discover(\"<namespace>\") and supply all required fields."),
					*FString::Join(Missing, TEXT(", ")),
					*FString::Join(Provided, TEXT(", "))),
				FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// Survivor D (plan §3.D) — schema-tagged path-kind handling.
	// Runs AFTER K2 alias rewrite (so the key the schema sees matches the
	// param name in EffectiveParams) and BEFORE K3 unknown-key check.
	//
	//   - Kind == AssetPath: `\` rewritten to `/` (silent fix-up + warning).
	//   - Kind == DiskPath:  backslash detected → warning, NO rewrite.
	//                        (Per RI ergonomics handover #5, 2026-05-29: the
	//                        DiskPath indexes store paths with forward slashes,
	//                        but DiskPath legitimately COULD address a real OS
	//                        path so we never silently rewrite — we just warn
	//                        loudly. The trap was silent-empty-on-backslash.)
	//   - All other Kinds (Other, GameplayTag) pass through untouched.
	//
	// Warnings appended to the same K3 warnings[] channel by the post-handler
	// block below.
	TArray<FString> PathParamWarnings;
	if (ActionInfo.ParamSchema.IsValid())
	{
		for (const auto& SchemaPair : ActionInfo.ParamSchema->Values)
		{
			const TSharedPtr<FJsonObject>* ParamDefPtr = nullptr;
			if (!SchemaPair.Value->TryGetObject(ParamDefPtr) || !ParamDefPtr || !ParamDefPtr->IsValid())
			{
				continue;
			}
			FString KindStr;
			if (!(*ParamDefPtr)->TryGetStringField(TEXT("kind"), KindStr))
			{
				continue; // No kind tag → Other (default) → no rewrite.
			}
			const EMonolithParamKind Kind = MonolithParamKind::FromString(KindStr);
			if (Kind != EMonolithParamKind::AssetPath && Kind != EMonolithParamKind::DiskPath)
			{
				continue;
			}

			const FString ParamName = MonolithKeyToString(SchemaPair.Key);
			FString Value;
			if (!EffectiveParams->TryGetStringField(ParamName, Value))
			{
				continue;
			}
			if (!Value.Contains(TEXT("\\")))
			{
				continue;
			}

			if (Kind == EMonolithParamKind::AssetPath)
			{
				FString Rewritten = Value.Replace(TEXT("\\"), TEXT("/"));
				EffectiveParams->SetStringField(ParamName, Rewritten);
				PathParamWarnings.Add(FString::Printf(
					TEXT("Normalised backslashes in 'asset_path' param '%s' — future calls should use forward slashes."),
					*ParamName));
			}
			else // DiskPath
			{
				FString Suggested = Value.Replace(TEXT("\\"), TEXT("/"));
				PathParamWarnings.Add(FString::Printf(
					TEXT("DiskPath param '%s' contains backslashes — paths in this index are stored with forward slashes ('/'), so a query for '%s' will likely return zero results. Convert to '%s'."),
					*ParamName, *Value, *Suggested));
			}
		}
	}

	// K3 — unknown-key detection (after required-check, before dispatch).
	TArray<FString> Unknown;
	if (ActionInfo.ParamSchema.IsValid())
	{
		Unknown = FMonolithParamSchema::FindUnknownKeys(ActionInfo.ParamSchema, EffectiveParams);

		if (Unknown.Num() > 0)
		{
			for (const FString& K : Unknown)
			{
				UE_LOG(LogMonolith, Warning,
					TEXT("Unknown param '%s' for action '%s:%s' (typo? not in schema)"),
					*K, *Namespace, *Action);
			}

			if (FMonolithParamSchema::IsStrictParamsEnabled())
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("STRICT_PARAMS=1: rejected action '%s:%s' due to unknown params: [%s] — unset STRICT_PARAMS or remove the unknown params from the call."),
						*Namespace, *Action, *FString::Join(Unknown, TEXT(", "))),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	// Release lock before executing handler (handlers may take time)
	FMonolithActionHandler HandlerCopy = RegAction->Handler;
	Lock.Unlock();

	FMonolithActionResult ActionResult = HandlerCopy.Execute(EffectiveParams);

	// Collect ALL post-handler warnings into a single channel, then attach once.
	// Sources, in order:
	//   1. K3 unknown-param soft-warn (pre-existing behaviour)
	//   2. Survivor D — AssetPath \→/ rewrite warnings (plan §3.D)
	//   3. Survivor B — response-shaping warnings (plan §3.B, e.g. _fields/_omit collision)
	if (ActionResult.bSuccess && ActionResult.Result.IsValid())
	{
		TArray<FString> AllWarnings;
		AllWarnings.Append(PathParamWarnings);
		for (const FString& K : Unknown)
		{
			AllWarnings.Add(FString::Printf(TEXT("Unknown param '%s' for action '%s:%s'"), *K, *Namespace, *Action));
		}

		// Survivor B post-filter — mutates ActionResult.Result in-place and may
		// append its own warnings (e.g., mutually-exclusive _fields + _omit).
		// Runs BEFORE attaching the warnings array so its warnings get included
		// in the final emit; runs AFTER warning collection so the filter cannot
		// strip the warnings[] key out from under us via _fields whitelist.
		// (We attach warnings to ActionResult.Result AFTER ApplyResponseShaping.)
		ApplyResponseShaping(ActionResult.Result, EffectiveParams, AllWarnings);

		if (AllWarnings.Num() > 0 && ActionResult.Result.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> Existing;
			const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
			if (ActionResult.Result->TryGetArrayField(TEXT("warnings"), Found) && Found)
			{
				Existing = *Found;
			}
			for (const FString& W : AllWarnings)
			{
				Existing.Add(MakeShared<FJsonValueString>(W));
			}
			ActionResult.Result->SetArrayField(TEXT("warnings"), Existing);
		}
	}

	return ActionResult;
}

TArray<FString> FMonolithToolRegistry::GetNamespaces() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;
	NamespaceActions.GetKeys(Result);
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetActions(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				FMonolithActionInfo Info = RegAction->Info;
				Info.ParamSchema = MonolithMetadataGeneration::CloneJsonObject(RegAction->Info.ParamSchema);
				Result.Add(MoveTemp(Info));
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetAllActions() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;
	for (const auto& Pair : Actions)
	{
		FMonolithActionInfo Info = Pair.Value.Info;
		Info.ParamSchema = MonolithMetadataGeneration::CloneJsonObject(Pair.Value.Info.ParamSchema);
		Result.Add(MoveTemp(Info));
	}
	return Result;
}

bool FMonolithToolRegistry::HasAction(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Contains(MakeKey(Namespace, Action));
}

int32 FMonolithToolRegistry::GetActionCount() const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Num();
}

void FMonolithToolRegistry::InvalidateMetadataStructuralDigest()
{
	bMetadataStructuralDigestDirty = true;
}

bool FMonolithToolRegistry::ComputeMetadataStructuralDigest(FString& OutDigest, FString& OutError) const
{
	TArray<FMonolithActionInfo> Infos;
	Infos.Reserve(Actions.Num());
	for (const TPair<FString, FRegisteredAction>& Pair : Actions)
	{
		Infos.Add(Pair.Value.Info);
	}
#if WITH_DEV_AUTOMATION_TESTS
	++MetadataStructuralDigestComputationCount;
#endif
	return MonolithMetadataGeneration::BuildStructuralDigest(
		Infos, DispatcherAnnotations, OutDigest, OutError);
}

FMonolithMetadataSnapshot FMonolithToolRegistry::GetMetadataSnapshot() const
{
	FScopeLock Lock(&RegistryLock);
	FMonolithMetadataSnapshot Snapshot;
	Snapshot.TotalActions = Actions.Num();
	Snapshot.NamespaceCount = NamespaceActions.Num();

	if (bMetadataStructuralDigestDirty)
	{
		CachedMetadataStructuralDigest.Reset();
		CachedMetadataStructuralDigestError.Reset();
		bMetadataStructuralDigestValid = ComputeMetadataStructuralDigest(
			CachedMetadataStructuralDigest, CachedMetadataStructuralDigestError);
		bMetadataStructuralDigestDirty = false;
	}

	TArray<const FRegisteredAction*> OrderedActions;
	OrderedActions.Reserve(Actions.Num());
	for (const TPair<FString, FRegisteredAction>& Pair : Actions)
	{
		OrderedActions.Add(&Pair.Value);
	}
	// TArray::Sort dereferences raw-pointer elements before invoking its predicate.
	OrderedActions.Sort([](const FRegisteredAction& Left, const FRegisteredAction& Right)
	{
		const int32 NamespaceOrder = Left.Info.Namespace.Compare(Right.Info.Namespace, ESearchCase::CaseSensitive);
		return NamespaceOrder == 0
			? Left.Info.Action.Compare(Right.Info.Action, ESearchCase::CaseSensitive) < 0
			: NamespaceOrder < 0;
	});

	TArray<uint8> BindingPayload;
	MonolithMetadataGeneration::AppendUInt64BigEndian(BindingPayload,
		static_cast<uint64>(OrderedActions.Num()));
	for (const FRegisteredAction* RegisteredAction : OrderedActions)
	{
		const bool bBound = RegisteredAction->Handler.IsBound();
#if WITH_DEV_AUTOMATION_TESTS
		++MetadataHandlerBindingSampleCount;
#endif
		const uint8 BindingByte = bBound ? 1 : 0;
		MonolithMetadataGeneration::AppendToken(BindingPayload,
			MonolithMetadataGeneration::ECanonicalTag::Boolean, &BindingByte, sizeof(BindingByte));
		if (!bBound)
		{
			Snapshot.UnavailableActions.Add(
				RegisteredAction->Info.Namespace + TEXT(".") + RegisteredAction->Info.Action);
		}
	}
	Snapshot.UnavailableActions.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});

	if (!bMetadataStructuralDigestValid)
	{
		Snapshot.MetadataGenerationError = CachedMetadataStructuralDigestError;
		return Snapshot;
	}

	TArray<uint8> FinalInput;
	MonolithMetadataGeneration::AppendStringToken(FinalInput,
		FString(TEXT("monolith-metadata-generation-final-v1")));
	MonolithMetadataGeneration::AppendStringToken(FinalInput, CachedMetadataStructuralDigest);
	MonolithMetadataGeneration::AppendToken(FinalInput,
		MonolithMetadataGeneration::ECanonicalTag::Array, BindingPayload.GetData(), BindingPayload.Num());
	Snapshot.MetadataGeneration = FString(TEXT("v1:")) +
		MonolithMetadataGeneration::HashToLowerHex(FinalInput);
	return Snapshot;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FMonolithToolRegistry::ComputeStructuralDigestForTesting(
	const TArray<FMonolithActionInfo>& InActions,
	const TMap<FString, FMonolithDispatcherAnnotations>& InDispatcherAnnotations,
	FString& OutDigest,
	FString& OutError)
{
	OutDigest.Reset();
	OutError.Reset();
	return MonolithMetadataGeneration::BuildStructuralDigest(
		InActions, InDispatcherAnnotations, OutDigest, OutError);
}

uint64 FMonolithToolRegistry::GetStructuralDigestComputationCountForTesting() const
{
	FScopeLock Lock(&RegistryLock);
	return MetadataStructuralDigestComputationCount;
}

uint64 FMonolithToolRegistry::GetHandlerBindingSampleCountForTesting() const
{
	FScopeLock Lock(&RegistryLock);
	return MetadataHandlerBindingSampleCount;
}
#endif

void FMonolithToolRegistry::SetDispatcherAnnotations(
	const FString& Namespace,
	const FMonolithDispatcherAnnotations& Annotations)
{
	FScopeLock Lock(&RegistryLock);
	DispatcherAnnotations.Add(Namespace, Annotations);
	InvalidateMetadataStructuralDigest();
}

FMonolithDispatcherAnnotations FMonolithToolRegistry::GetDispatcherAnnotations(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	if (const FMonolithDispatcherAnnotations* Found = DispatcherAnnotations.Find(Namespace))
	{
		return *Found;
	}
	return FMonolithDispatcherAnnotations{};
}

void FMonolithToolRegistry::SetActionAnnotations(
	const FString& Namespace,
	const FString& Action,
	bool bReadOnly,
	bool bDestructive,
	bool bIdempotent,
	const FString& Title)
{
	FScopeLock Lock(&RegistryLock);
	FString Key = MakeKey(Namespace, Action);
	if (FRegisteredAction* RegAction = Actions.Find(Key))
	{
		RegAction->Info.bReadOnlyHint = bReadOnly;
		RegAction->Info.bDestructiveHint = bDestructive;
		RegAction->Info.bIdempotentHint = bIdempotent;
		RegAction->Info.Title = Title;
		InvalidateMetadataStructuralDigest();
	}
}
