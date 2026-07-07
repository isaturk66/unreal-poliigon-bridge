// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PoliigonBridgeSettings.generated.h"

/** Project Settings -> Plugins -> Poliigon Bridge */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Poliigon Bridge"))
class UPoliigonBridgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPoliigonBridgeSettings()
	{
		CategoryName = TEXT("Plugins");
		SectionName = TEXT("Poliigon Bridge");
	}

	/** Content folder all ingested assets are placed under. */
	UPROPERTY(EditAnywhere, config, Category = "Ingest", meta = (DisplayName = "Content Root"))
	FString ContentRoot = TEXT("/Game/Poliigon");

	/** Default texture resolution to download (falls back to nearest available). */
	UPROPERTY(EditAnywhere, config, Category = "Download", meta = (GetOptions = "GetResolutionOptions"))
	FString DefaultResolution = TEXT("2K");

	/** Also download and import displacement maps (wired to the master's Nanite displacement pin). */
	UPROPERTY(EditAnywhere, config, Category = "Download")
	bool bDownloadDisplacement = true;

	/** Keep raw downloaded files in Saved/PoliigonBridge/Staging after ingest (acts as a re-import cache). */
	UPROPERTY(EditAnywhere, config, Category = "Download")
	bool bKeepStagingFiles = true;

	/** Results per page in the browser. */
	UPROPERTY(EditAnywhere, config, Category = "Browser", meta = (ClampMin = 8, ClampMax = 100))
	int32 PageSize = 24;

	UFUNCTION()
	static TArray<FString> GetResolutionOptions()
	{
		return { TEXT("1K"), TEXT("2K"), TEXT("4K"), TEXT("8K") };
	}
};
