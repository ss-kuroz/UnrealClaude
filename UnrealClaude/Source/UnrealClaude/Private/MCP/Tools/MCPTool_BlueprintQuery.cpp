// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_BlueprintQuery.h"
#include "BlueprintUtils.h"
#include "BlueprintGraphEditor.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "K2Node_Variable.h"
#include "K2Node_CallFunction.h"

FMCPToolResult FMCPTool_BlueprintQuery::Execute(const TSharedRef<FJsonObject>& Params)
{
	// Get operation type
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("list"))
	{
		return ExecuteList(Params);
	}
	else if (Operation == TEXT("inspect"))
	{
		return ExecuteInspect(Params);
	}
	else if (Operation == TEXT("get_graph"))
	{
		return ExecuteGetGraph(Params);
	}
	else if (Operation == TEXT("get_nodes"))
	{
		return ExecuteGetNodes(Params);
	}
	else if (Operation == TEXT("get_variables"))
	{
		return ExecuteGetVariables(Params);
	}
	else if (Operation == TEXT("get_functions"))
	{
		return ExecuteGetFunctions(Params);
	}
	else if (Operation == TEXT("get_node_pins"))
	{
		return ExecuteGetNodePins(Params);
	}
	else if (Operation == TEXT("search_nodes"))
	{
		return ExecuteSearchNodes(Params);
	}
	else if (Operation == TEXT("find_references"))
	{
		return ExecuteFindReferences(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid operations: 'list', 'inspect', 'get_graph', 'get_nodes', 'get_variables', 'get_functions', 'get_node_pins', 'search_nodes', 'find_references'"), *Operation));
}

// --- Shared helpers ---

UBlueprint* FMCPTool_BlueprintQuery::LoadAndValidateBlueprint(const TSharedRef<FJsonObject>& Params)
{
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		LastError = Error.GetValue();
		return nullptr;
	}

	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		LastError = FMCPToolResult::Error(ValidationError);
		return nullptr;
	}

	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		LastError = FMCPToolResult::Error(LoadError);
		return nullptr;
	}

	return Blueprint;
}

TArray<UEdGraph*> FMCPTool_BlueprintQuery::CollectGraphs(UBlueprint* Blueprint, const FString& GraphName)
{
	TArray<UEdGraph*> Graphs;
	if (!GraphName.IsEmpty())
	{
		FString FindError;
		UEdGraph* Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, true, FindError);
		if (!Graph) Graph = FBlueprintGraphEditor::FindGraph(Blueprint, GraphName, false, FindError);
		if (Graph) Graphs.Add(Graph);
	}
	else
	{
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);
	}
	return Graphs;
}

UEdGraphNode* FMCPTool_BlueprintQuery::FindNodeInGraphs(
	const TArray<UEdGraph*>& Graphs, const FString& NodeId, FString& OutGraphName)
{
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;

		// Try MCP ID first
		UEdGraphNode* Node = FBlueprintGraphEditor::FindNodeById(Graph, NodeId);
		if (Node)
		{
			OutGraphName = Graph->GetName();
			return Node;
		}

		// Fallback: try matching NodeGuid
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid.ToString() == NodeId)
			{
				OutGraphName = Graph->GetName();
				return N;
			}
		}
	}
	return nullptr;
}

// --- Existing operations ---

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteList(const TSharedRef<FJsonObject>& Params)
{
	// Extract filters
	FString PathFilter = ExtractOptionalString(Params, TEXT("path_filter"), TEXT("/Game/"));
	FString TypeFilter = ExtractOptionalString(Params, TEXT("type_filter"));
	FString NameFilter = ExtractOptionalString(Params, TEXT("name_filter"));
	int32 Limit = ExtractOptionalNumber<int32>(Params, TEXT("limit"), 25);

	// Clamp limit
	Limit = FMath::Clamp(Limit, 1, 1000);

	// Validate path filter
	FString ValidationError;
	if (!PathFilter.IsEmpty() && !FMCPParamValidator::ValidateBlueprintPath(PathFilter, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Query AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Build filter
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
	}

	// Get assets
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssets(Filter, AssetDataList);

	// Process results
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 Count = 0;
	int32 TotalMatching = 0;

	for (const FAssetData& AssetData : AssetDataList)
	{
		// Get parent class name for filtering
		FString ParentClassName;
		FAssetDataTagMapSharedView::FFindTagResult ParentClassTag = AssetData.TagsAndValues.FindTag(FName("ParentClass"));
		if (ParentClassTag.IsSet())
		{
			ParentClassName = ParentClassTag.GetValue();
		}

		// Apply type filter
		if (!TypeFilter.IsEmpty())
		{
			if (!ParentClassName.Contains(TypeFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply name filter
		if (!NameFilter.IsEmpty())
		{
			if (!AssetData.AssetName.ToString().Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TotalMatching++;

		// Check limit
		if (Count >= Limit)
		{
			continue;
		}

		// Get Blueprint type
		FString BlueprintType = TEXT("Normal");
		FAssetDataTagMapSharedView::FFindTagResult TypeTag = AssetData.TagsAndValues.FindTag(FName("BlueprintType"));
		if (TypeTag.IsSet())
		{
			BlueprintType = TypeTag.GetValue();
		}

		// Build result object
		TSharedPtr<FJsonObject> BPJson = MakeShared<FJsonObject>();
		BPJson->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		BPJson->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
		BPJson->SetStringField(TEXT("blueprint_type"), BlueprintType);

		// Clean up parent class name (remove prefix)
		if (!ParentClassName.IsEmpty())
		{
			FString CleanParentName = ParentClassName;
			int32 LastDotIndex;
			if (CleanParentName.FindLastChar(TEXT('.'), LastDotIndex))
			{
				CleanParentName = CleanParentName.Mid(LastDotIndex + 1);
			}
			// Remove trailing '_C' from generated class names
			if (CleanParentName.EndsWith(TEXT("_C")))
			{
				CleanParentName = CleanParentName.LeftChop(2);
			}
			BPJson->SetStringField(TEXT("parent_class"), CleanParentName);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(BPJson));
		Count++;
	}

	// Build response
	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetArrayField(TEXT("blueprints"), ResultsArray);
	ResponseData->SetNumberField(TEXT("count"), Count);
	ResponseData->SetNumberField(TEXT("total_matching"), TotalMatching);

	if (TotalMatching > Count)
	{
		ResponseData->SetBoolField(TEXT("truncated"), true);
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Found %d Blueprints (showing %d)"), TotalMatching, Count),
		ResponseData
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteInspect(const TSharedRef<FJsonObject>& Params)
{
	// Get Blueprint path
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	// Validate path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get options
	bool bIncludeVariables = ExtractOptionalBool(Params, TEXT("include_variables"), false);
	bool bIncludeFunctions = ExtractOptionalBool(Params, TEXT("include_functions"), false);
	bool bIncludeGraphs = ExtractOptionalBool(Params, TEXT("include_graphs"), false);

	// Serialize Blueprint info
	TSharedPtr<FJsonObject> BlueprintInfo = FBlueprintUtils::SerializeBlueprintInfo(
		Blueprint,
		bIncludeVariables,
		bIncludeFunctions,
		bIncludeGraphs
	);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Blueprint info for: %s"), *Blueprint->GetName()),
		BlueprintInfo
	);
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetGraph(const TSharedRef<FJsonObject>& Params)
{
	// Get Blueprint path
	FString BlueprintPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, Error))
	{
		return Error.GetValue();
	}

	// Validate path
	FString ValidationError;
	if (!FMCPParamValidator::ValidateBlueprintPath(BlueprintPath, ValidationError))
	{
		return FMCPToolResult::Error(ValidationError);
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = FBlueprintUtils::LoadBlueprint(BlueprintPath, LoadError);
	if (!Blueprint)
	{
		return FMCPToolResult::Error(LoadError);
	}

	// Get graph info
	TSharedPtr<FJsonObject> GraphInfo = FBlueprintUtils::GetGraphInfo(Blueprint);

	// Add Blueprint name for context
	GraphInfo->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	GraphInfo->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Graph info for: %s"), *Blueprint->GetName()),
		GraphInfo
	);
}

// --- New operation stubs (to be implemented in subsequent tasks) ---

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetNodes(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("get_nodes: Not yet implemented"));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetVariables(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("get_variables: Not yet implemented"));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetFunctions(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("get_functions: Not yet implemented"));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteGetNodePins(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("get_node_pins: Not yet implemented"));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteSearchNodes(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("search_nodes: Not yet implemented"));
}

FMCPToolResult FMCPTool_BlueprintQuery::ExecuteFindReferences(const TSharedRef<FJsonObject>& Params)
{
	return FMCPToolResult::Error(TEXT("find_references: Not yet implemented"));
}
