// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"

#include "AnimMCPTypes.h"

class UAnimBlueprint;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class USkeleton;

DECLARE_LOG_CATEGORY_EXTERN(LogAnimMCP, Log, All);

/** Every tool must run on the game thread; bail out with a clean error otherwise. */
#define ANIMMCP_REQUIRE_GAME_THREAD() \
	if (!IsInGameThread()) \
	{ \
		return AnimMCP::Fail(TEXT("AnimMCPToolset tools must run on the game thread.")); \
	}

namespace AnimMCP
{
	// ---- Result envelope -------------------------------------------------------------------

	FAnimMCPResult Ok(const TSharedRef<FJsonObject>& Payload);
	FAnimMCPResult Fail(const FString& Message);

	/** True for "", "*" and "none": the values optional string params use to mean "not set". */
	bool IsUnset(const FString& Value);

	/** Bool pins/variables accept only true/false (the K2 validator accepts anything). */
	bool ValidateBoolText(const FEdGraphPinType& PinType, const FString& Value, FString& OutError);

	/**
	 * Normalizes a content folder for asset registry queries: unset -> "/Game", trailing slashes removed.
	 * The registry matches package paths exactly, so "/Game/Characters/" would otherwise find nothing.
	 */
	bool NormalizeFolder(const FString& InFolder, FString& OutFolder, FString& OutError);

	/** Makes sure the asset registry has scanned a folder before it is queried. */
	void EnsureFolderScanned(const FString& Folder);

	// ---- Paths and assets ------------------------------------------------------------------

	/**
	 * Converts any accepted spelling of an asset path ("/Game/A/B", "/Game/A/B.B",
	 * "/Script/Engine.Foo'/Game/A/B.B'") into a full object path "/Game/A/B.B".
	 */
	bool NormalizeObjectPath(const FString& InPath, FString& OutObjectPath, FString& OutError);

	/** True if the package lives under /Game. */
	bool IsUnderGameRoot(const FString& PackageOrObjectPath);

	/**
	 * Loads an asset of class T. If bForWrite is true the asset must live under /Game.
	 * Returns nullptr and fills OutError on failure.
	 */
	UObject* LoadAssetChecked(const FString& Path, UClass* ExpectedClass, bool bForWrite, FString& OutError);

	template <typename T>
	T* LoadAsset(const FString& Path, bool bForWrite, FString& OutError)
	{
		return Cast<T>(LoadAssetChecked(Path, T::StaticClass(), bForWrite, OutError));
	}

	/** Validates a destination for a new asset: under /Game, valid name, no collision. */
	bool ValidateNewAssetLocation(const FString& PackagePath, const FString& AssetName, FString& OutPackageName, FString& OutError);

	/** Resolves a skeleton from a Skeleton, SkeletalMesh or AnimBlueprint path. */
	USkeleton* ResolveSkeleton(const FString& Path, FString& OutError);

	// ---- Graphs, nodes, pins ---------------------------------------------------------------

	bool ParseGuid(const FString& GuidString, FGuid& OutGuid, FString& OutError);

	/** Finds a graph in the blueprint (including nested state/transition graphs) by name or GraphGuid. */
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& NameOrGuid, FString& OutError);

	/** Finds a node anywhere in the blueprint by its NodeGuid. */
	UEdGraphNode* FindNode(UBlueprint* Blueprint, const FString& NodeGuid, FString& OutError);

	template <typename T>
	T* FindNodeOfType(UBlueprint* Blueprint, const FString& NodeGuid, FString& OutError)
	{
		UEdGraphNode* Node = FindNode(Blueprint, NodeGuid, OutError);
		if (!Node)
		{
			return nullptr;
		}
		T* Typed = Cast<T>(Node);
		if (!Typed)
		{
			OutError = FString::Printf(TEXT("Node %s is a %s, expected %s."),
				*NodeGuid, *Node->GetClass()->GetName(), *T::StaticClass()->GetName());
		}
		return Typed;
	}

	/**
	 * Finds a pin by name. Direction may be "", "input" or "output".
	 * If the name matches pins in both directions and no direction was given, this fails
	 * and lists the candidates.
	 */
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, const FString& Direction, FString& OutError);

	/** First non-hidden pin of the given direction, used for pose/result wiring. */
	UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction);

	/**
	 * Creates a node of the given class in the graph the same way the editor's palette does
	 * (CreateNewGuid, PostPlacedNewNode, AllocateDefaultPins). Optional Init runs before
	 * AllocateDefaultPins so callers can configure variable/function references.
	 */
	UEdGraphNode* SpawnNode(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position, TFunctionRef<void(UEdGraphNode*)> Init);
	UEdGraphNode* SpawnNode(UEdGraph* Graph, UClass* NodeClass, const FVector2D& Position);

	/** Removes a node through FBlueprintEditorUtils so sub-graphs are cleaned up. */
	void RemoveNode(UBlueprint* Blueprint, UEdGraphNode* Node);

	/** Gets the blueprint that owns the graph the node lives in. */
	UBlueprint* GetOwningBlueprint(const UEdGraphNode* Node);

	/**
	 * Resolves a node class name ('AnimGraphNode_Slot', 'K2Node_VariableGet' or a full class path) to a class that
	 * can be placed by hand. Result/entry nodes and state machine states/transitions are rejected.
	 */
	UClass* ResolveNodeClass(const FString& Name, FString& OutError);

	// ---- Properties ------------------------------------------------------------------------

	struct FResolvedProperty
	{
		FProperty* TopProperty = nullptr;
		FProperty* LeafProperty = nullptr;
		void* LeafValue = nullptr;
	};

	/**
	 * Walks 'A.B[2].C' from Object down to the leaf value. Every segment must be editor-visible.
	 * On anim graph nodes 'Node' always means the node's runtime struct.
	 */
	bool ResolvePropertyPath(UObject* Object, const FString& Path, FResolvedProperty& Out, FString& OutError);

	/** Checks that Value parses for the property at Path without touching Object (imports into a scratch copy). */
	bool CanImportPropertyValue(UObject* Object, const FString& Path, const FString& Value, FString& OutError);

	/**
	 * Sets a property on a node the way the details panel does (PreEditChange, import, PostEditChangeProperty, ReconstructNode).
	 * The caller owns the transaction. OutReadBack receives the value as stored after the change.
	 */
	bool SetNodePropertyByPath(UEdGraphNode* Node, const FString& Path, const FString& Value, FString& OutReadBack, FString& OutError);

	// ---- Serialization ---------------------------------------------------------------------

	FString GuidToString(const FGuid& Guid);
	FString DirectionToString(EEdGraphPinDirection Direction);
	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin);
	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node, bool bIncludePins);
	TSharedRef<FJsonObject> GraphToJson(const UEdGraph* Graph);
	FString PinTypeToString(const FEdGraphPinType& PinType);

	/** Parses a type string such as "bool", "float", "vector", "object:/Script/Engine.AnimSequence". */
	bool ParsePinType(const FString& TypeString, FEdGraphPinType& OutType, FString& OutError);

	/** Validates a variable default value string for a pin type using the K2 schema. Empty is always valid. */
	bool ValidateDefaultValue(const FEdGraphPinType& PinType, const FName VarName, const FString& Value, FString& OutError);

	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<TSharedRef<FJsonObject>>& Objects);
	TArray<TSharedPtr<FJsonValue>> ToJsonArray(const TArray<FString>& Strings);
}
