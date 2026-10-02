// Copyright (c) AnimMCPToolset contributors. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "JsonObjectWrapper.h"

#include "AnimMCPTypes.generated.h"

/**
 * Uniform envelope returned by every AnimMCPToolset tool.
 * Check 'Success' first. On success 'Result' holds the tool-specific JSON payload and 'Error' is empty.
 * On failure 'Result' is an empty object and 'Error' explains what went wrong and how to fix the call.
 */
USTRUCT(BlueprintType)
struct FAnimMCPResult
{
	GENERATED_BODY()

	/** True if the tool completed its work. False if nothing was changed because of an error. */
	UPROPERTY(BlueprintReadWrite, Category = "AnimMCP")
	bool Success = false;

	/** Tool-specific JSON payload. Empty object on failure. */
	UPROPERTY(BlueprintReadWrite, Category = "AnimMCP")
	FJsonObjectWrapper Result;

	/** Human-readable error message. Empty on success. */
	UPROPERTY(BlueprintReadWrite, Category = "AnimMCP")
	FString Error;
};
