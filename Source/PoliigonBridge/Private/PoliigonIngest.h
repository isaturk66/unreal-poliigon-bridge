// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#pragma once

#include "CoreMinimal.h"
#include "PoliigonTypes.h"

class UMaterial;
class UMaterialInstanceConstant;
class UTexture2D;

/**
 * Turns a staging folder of downloaded Poliigon files into engine assets:
 *  - textures imported with correct compression/sRGB per map type
 *  - one Material Instance Constant per texture set, parented to a generated
 *    master material (explicit Substrate slab graph when Substrate expressions
 *    are available in the engine, legacy attribute pins otherwise — the latter
 *    still compiles as a Substrate single slab when r.Substrate=1)
 *  - FBX models imported and their slots bound to the generated instances
 *
 * Must be called on the game thread.
 */
class FPoliigonIngest
{
public:
	struct FResult
	{
		bool bOk = false;
		FString Error;
		TArray<FString> CreatedAssetPaths; // object paths of MICs/meshes for content browser sync
	};

	static FResult IngestAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir);

private:
	struct FTextureSet
	{
		FString Prefix;                          // e.g. "TilesTravertine001"
		TMap<EPoliigonMapSlot, FString> Files;   // slot -> absolute file path
	};

	static UMaterial* EnsureMasterMaterial(FString& OutError);
	static bool BuildMasterGraph(UMaterial* Material);

	static TArray<FTextureSet> CollectTextureSets(const FString& StagingDir);
	static UTexture2D* ImportTexture(const FString& FilePath, const FString& DestPackagePath, EPoliigonMapSlot Slot);
	static UMaterialInstanceConstant* CreateInstanceForSet(
		const FTextureSet& Set, const FString& InstanceName, const FString& DestPackagePath,
		UMaterial* Master, FString& OutError);

	static FResult IngestTextureAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir);
	static FResult IngestModelAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir);

	static void SavePackageFor(UObject* Object);
};
