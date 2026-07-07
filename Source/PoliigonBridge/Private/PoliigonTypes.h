// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#pragma once

#include "CoreMinimal.h"

enum class EPoliigonAssetType : uint8
{
	Texture,
	Model,
	HDRI,
	Brush,
	Unsupported
};

/** One downloadable map description (convention 1 assets). */
struct FPoliigonMapDesc
{
	FString TypeCode;            // e.g. "BaseColor", "ORM"
	TArray<FString> FileFormats; // e.g. ["jpg","png"]
};

/** Parsed result of GET /assets/details/{id}. */
struct FPoliigonAssetInfo
{
	int32 AssetId = 0;
	FString AssetName;    // machine name, e.g. "TilesTravertine001"
	FString DisplayName;  // human name, e.g. "Tiles Travertine 001"
	EPoliigonAssetType Type = EPoliigonAssetType::Unsupported;
	int32 Convention = 0; // 0 = legacy COL/NRM/GLOSS, 1 = BaseColor/Normal/ORM
	int32 Credits = 1;
	TArray<FString> Resolutions;                 // ["1K","2K","4K",...]
	TMap<FString, TArray<FString>> WorkflowMaps; // convention 0: workflow -> map type codes
	TArray<FPoliigonMapDesc> Maps;               // convention 1
	FString ThumbBaseUrl;                        // cloudflare base url; append "/300px"
	bool bDetailsLoaded = false;
};

/** One file entry from POST /assets/download. */
struct FPoliigonFileEntry
{
	FString Url;
	FString Name;
	int64 Bytes = 0;
};

/** Canonical material slots the ingester understands. */
enum class EPoliigonMapSlot : uint8
{
	BaseColor,
	Normal,
	Roughness,
	Gloss,       // legacy: 1 - roughness
	Metallic,
	AO,
	ORM,         // packed R=AO G=Roughness B=Metallic
	Displacement,
	Emission,
	Opacity,
	SSS,
	Translucency,
	Transmission,
	Unknown
};

namespace PoliigonMaps
{
	/** Maps a filename/API map token to a canonical slot. Case-sensitive API codes, matched case-insensitively for files. */
	inline EPoliigonMapSlot SlotFromToken(const FString& InToken)
	{
		const FString T = InToken.ToUpper();
		if (T == TEXT("COL") || T == TEXT("DIFF") || T == TEXT("ALBEDO") || T == TEXT("BASECOLOR") || T == TEXT("ALPHAMASKED") || T == TEXT("BASECOLOROPACITY"))
		{
			return EPoliigonMapSlot::BaseColor;
		}
		if (T == TEXT("NRM") || T == TEXT("NRM16") || T == TEXT("NORMAL"))
		{
			return EPoliigonMapSlot::Normal;
		}
		if (T == TEXT("ROUGHNESS"))
		{
			return EPoliigonMapSlot::Roughness;
		}
		if (T == TEXT("GLOSS"))
		{
			return EPoliigonMapSlot::Gloss;
		}
		if (T == TEXT("METALNESS") || T == TEXT("METALLIC"))
		{
			return EPoliigonMapSlot::Metallic;
		}
		if (T == TEXT("AO") || T == TEXT("AMBIENTOCCLUSION"))
		{
			return EPoliigonMapSlot::AO;
		}
		if (T == TEXT("ORM"))
		{
			return EPoliigonMapSlot::ORM;
		}
		if (T == TEXT("DISP") || T == TEXT("DISP16") || T == TEXT("DISPLACEMENT") || T == TEXT("BUMP") || T == TEXT("BUMP16"))
		{
			return EPoliigonMapSlot::Displacement;
		}
		if (T == TEXT("EMISSIVE") || T == TEXT("EMISSION"))
		{
			return EPoliigonMapSlot::Emission;
		}
		if (T == TEXT("MASK") || T == TEXT("OPACITY"))
		{
			return EPoliigonMapSlot::Opacity;
		}
		if (T == TEXT("SSS") || T == TEXT("SCATTERINGCOLOR"))
		{
			return EPoliigonMapSlot::SSS;
		}
		if (T == TEXT("TRANSLUCENCY"))
		{
			return EPoliigonMapSlot::Translucency;
		}
		if (T == TEXT("TRANSMISSION"))
		{
			return EPoliigonMapSlot::Transmission;
		}
		return EPoliigonMapSlot::Unknown;
	}

	/** True if the token names any known map type (used when parsing filenames). */
	inline bool IsMapToken(const FString& InToken)
	{
		if (SlotFromToken(InToken) != EPoliigonMapSlot::Unknown)
		{
			return true;
		}
		const FString T = InToken.ToUpper();
		// Known-but-ignored map tokens; still recognized so filename parsing splits correctly.
		return T == TEXT("REFL") || T == TEXT("FUZZ") || T == TEXT("IDMAP") || T == TEXT("ID")
			|| T == TEXT("OVERLAY") || T == TEXT("OVERLAY16") || T == TEXT("SHEENCOLOR")
			|| T == TEXT("DIRECTION");
	}

	/** True for resolution-ish tokens like "2K", "600", "HIRES". */
	inline bool IsResolutionToken(const FString& InToken)
	{
		const FString T = InToken.ToUpper();
		if (T.EndsWith(TEXT("K")) && T.Len() <= 3 && FChar::IsDigit(T[0]))
		{
			return true;
		}
		return T == TEXT("HIRES");
	}
}
