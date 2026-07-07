// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#include "PoliigonIngest.h"
#include "PoliigonBridgeSettings.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionDesaturation.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "SceneTypes.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPoliigonIngest, Log, All);

#define LOCTEXT_NAMESPACE "PoliigonBridge"

namespace
{
	// Parameter names on the master material. MICs set these.
	const FName PARAM_BASECOLOR(TEXT("BaseColor Map"));
	const FName PARAM_NORMAL(TEXT("Normal Map"));
	const FName PARAM_ORM(TEXT("ORM Map"));
	const FName PARAM_ROUGHNESS(TEXT("Roughness Map"));
	const FName PARAM_METALLIC(TEXT("Metallic Map"));
	const FName PARAM_AO(TEXT("AO Map"));
	const FName PARAM_EMISSION(TEXT("Emission Map"));
	const FName PARAM_OPACITY(TEXT("Opacity Map"));
	const FName PARAM_DISPLACEMENT(TEXT("Displacement Map"));
	const FName SWITCH_USE_ORM(TEXT("Use ORM"));
	const FName SWITCH_INVERT_ROUGH(TEXT("Invert Roughness (Gloss)"));
	const FName SWITCH_HAS_METALLIC(TEXT("Has Metallic"));
	const FName SWITCH_HAS_AO(TEXT("Has AO"));
	const FName SWITCH_HAS_EMISSION(TEXT("Has Emission"));
	const FName SWITCH_HAS_OPACITY(TEXT("Has Opacity"));
	const FName SWITCH_HAS_DISPLACEMENT(TEXT("Has Displacement"));
	const FName SWITCH_WORLD_UV(TEXT("Use World Aligned UVs"));
	const FName SWITCH_MACRO(TEXT("Use Macro Variation"));

	const TCHAR* MASTER_NAME = TEXT("M_PoliigonMaster");

	bool IsTextureFile(const FString& Path)
	{
		const FString Ext = FPaths::GetExtension(Path).ToLower();
		return Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("png")
			|| Ext == TEXT("tif") || Ext == TEXT("tiff") || Ext == TEXT("exr");
	}

	/** Splits "TilesTravertine001_COL_2K" into prefix + slot. Returns false if no map token found. */
	bool ParseTextureFileName(const FString& FilePath, EPoliigonMapSlot& OutSlot, FString& OutPrefix, bool& bOut16Bit)
	{
		const FString Base = FPaths::GetBaseFilename(FilePath);
		TArray<FString> Tokens;
		Base.ParseIntoArray(Tokens, TEXT("_"), true);
		for (int32 i = 0; i < Tokens.Num(); ++i)
		{
			if (PoliigonMaps::IsMapToken(Tokens[i]))
			{
				OutSlot = PoliigonMaps::SlotFromToken(Tokens[i]);
				bOut16Bit = Tokens[i].EndsWith(TEXT("16"));
				TArray<FString> PrefixTokens;
				for (int32 j = 0; j < i; ++j)
				{
					PrefixTokens.Add(Tokens[j]);
				}
				OutPrefix = FString::Join(PrefixTokens, TEXT("_"));
				return OutSlot != EPoliigonMapSlot::Unknown && !OutPrefix.IsEmpty();
			}
		}
		return false;
	}

	FString SanitizeAssetName(const FString& In)
	{
		FString Out = In;
		for (TCHAR& C : Out)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
			{
				C = TEXT('_');
			}
		}
		return Out;
	}

	template <typename T>
	T* NewExpression(UMaterial* Material, int32 X, int32 Y)
	{
		T* Expression = Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass()));
		if (Expression)
		{
			Expression->MaterialExpressionEditorX = X;
			Expression->MaterialExpressionEditorY = Y;
		}
		return Expression;
	}

	UTexture* LoadEngineTexture(const TArray<FString>& Paths)
	{
		for (const FString& Path : Paths)
		{
			if (UTexture* Tex = LoadObject<UTexture>(nullptr, *Path))
			{
				return Tex;
			}
		}
		return nullptr;
	}

	/**
	 * Creates (or loads) a tiny solid-color texture asset whose compression settings
	 * match a given sampler type. Needed because engine stock textures (WhiteSquareTexture
	 * etc.) are sRGB TC_Default and cause "sampler type is Masks, should be Color"
	 * compile errors when used as defaults on Masks/LinearGrayscale parameters.
	 */
	UTexture2D* GetOrCreateSolidTexture(const FString& Name, const FString& PackagePath,
		TextureCompressionSettings Compression, bool bSRGB, FColor Pixel)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *Name, *Name);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *ObjectPath))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(*(PackagePath / Name));
		if (!Package)
		{
			return nullptr;
		}
		UTexture2D* Texture = NewObject<UTexture2D>(Package, FName(*Name), RF_Public | RF_Standalone);

		const int32 Size = 4;
		TArray<uint8> Pixels;
		Pixels.SetNumUninitialized(Size * Size * 4);
		for (int32 i = 0; i < Size * Size; ++i)
		{
			Pixels[i * 4 + 0] = Pixel.B;
			Pixels[i * 4 + 1] = Pixel.G;
			Pixels[i * 4 + 2] = Pixel.R;
			Pixels[i * 4 + 3] = Pixel.A;
		}
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Pixels.GetData());
		Texture->CompressionSettings = Compression;
		Texture->SRGB = bSRGB;
		Texture->UpdateResource();
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Texture);
		return Texture;
	}

	/** Deterministic wiring via the typed input member — no pin-name matching. */
	void Wire(UMaterialExpression* From, FExpressionInput& Input)
	{
		if (From)
		{
			Input.Connect(0, From);
		}
	}

	/**
	 * Wires an input on an expression whose C++ type is only known at runtime
	 * (the Substrate slab) by looking up the FExpressionInput UPROPERTY by its
	 * *property* name — stable API, unlike editor pin display names.
	 */
	bool WireByProp(UMaterialExpression* From, UMaterialExpression* To, const TCHAR* PropertyName)
	{
		if (!From || !To)
		{
			return false;
		}
		FStructProperty* Prop = FindFProperty<FStructProperty>(To->GetClass(), PropertyName);
		if (!Prop || Prop->Struct != FExpressionInput::StaticStruct())
		{
			UE_LOG(LogPoliigonIngest, Warning, TEXT("Master graph: no FExpressionInput property '%s' on %s"),
				PropertyName, *To->GetClass()->GetName());
			return false;
		}
		Prop->ContainerPtrToValuePtr<FExpressionInput>(To)->Connect(0, From);
		return true;
	}
}

// --- Master material ----------------------------------------------------------

UMaterial* FPoliigonIngest::EnsureMasterMaterial(FString& OutError)
{
	const UPoliigonBridgeSettings* Settings = GetDefault<UPoliigonBridgeSettings>();
	const FString PackagePath = Settings->ContentRoot / TEXT("Core");
	const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, MASTER_NAME, MASTER_NAME);

	if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *ObjectPath))
	{
		return Existing;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(
		AssetTools.CreateAsset(MASTER_NAME, PackagePath, UMaterial::StaticClass(), Factory));
	if (!Material)
	{
		OutError = TEXT("Failed to create master material.");
		return nullptr;
	}

	if (!BuildMasterGraph(Material))
	{
		OutError = TEXT("Failed to build master material graph.");
		return nullptr;
	}

	Material->PostEditChange();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	SavePackageFor(Material);
	UE_LOG(LogPoliigonIngest, Log, TEXT("Created master material at %s"), *ObjectPath);
	return Material;
}

bool FPoliigonIngest::BuildMasterGraph(UMaterial* Material)
{
	using ELib = UMaterialEditingLibrary;

	auto MakeScalar = [&](const TCHAR* Name, float Default, int32 X, int32 Y)
	{
		auto* Param = NewExpression<UMaterialExpressionScalarParameter>(Material, X, Y);
		Param->ParameterName = Name;
		Param->DefaultValue = Default;
		return Param;
	};

	auto MakeSwitch = [&](const FName& Name, int32 X, int32 Y, UMaterialExpression* True,
		UMaterialExpression* False) -> UMaterialExpressionStaticSwitchParameter*
	{
		auto* Switch = NewExpression<UMaterialExpressionStaticSwitchParameter>(Material, X, Y);
		Switch->ParameterName = Name;
		Switch->DefaultValue = false;
		Wire(True, Switch->A);
		Wire(False, Switch->B);
		return Switch;
	};

	// ---- UV chain: (mesh UVs | world-planar XY) -> rotate -> tile -> offset ----
	auto* UV = NewExpression<UMaterialExpressionTextureCoordinate>(Material, -2700, 0);
	auto* WorldPos = NewExpression<UMaterialExpressionWorldPosition>(Material, -2700, 130);
	auto* WorldXY = NewExpression<UMaterialExpressionComponentMask>(Material, -2550, 130);
	WorldXY->R = 1; WorldXY->G = 1; WorldXY->B = 0; WorldXY->A = 0;
	Wire(WorldPos, WorldXY->Input);
	auto* WorldTileSize = MakeScalar(TEXT("World Tile Size (cm)"), 100.0f, -2700, 260);
	auto* WorldUV = NewExpression<UMaterialExpressionDivide>(Material, -2400, 150);
	Wire(WorldXY, WorldUV->A);
	Wire(WorldTileSize, WorldUV->B);
	// Top-down planar projection — ideal for floors and large tiling surfaces.
	auto* UVSource = MakeSwitch(SWITCH_WORLD_UV, -2250, 40, WorldUV, UV);

	auto* RotationParam = MakeScalar(TEXT("Rotation"), 0.0f, -2400, 300); // in turns (0..1)
	auto* UVRotator = NewExpression<UMaterialExpressionRotator>(Material, -2050, 60);
	UVRotator->CenterX = 0.5f;
	UVRotator->CenterY = 0.5f;
	UVRotator->Speed = 6.2831853f; // one turn per unit
	Wire(UVSource, UVRotator->Coordinate);
	Wire(RotationParam, UVRotator->Time);

	auto* TilingX = MakeScalar(TEXT("Tiling X"), 1.0f, -2050, 220);
	auto* TilingY = MakeScalar(TEXT("Tiling Y"), 1.0f, -2050, 300);
	auto* TilingVec = NewExpression<UMaterialExpressionAppendVector>(Material, -1900, 240);
	Wire(TilingX, TilingVec->A);
	Wire(TilingY, TilingVec->B);
	auto* UVTiled = NewExpression<UMaterialExpressionMultiply>(Material, -1780, 100);
	Wire(UVRotator, UVTiled->A);
	Wire(TilingVec, UVTiled->B);

	auto* OffsetX = MakeScalar(TEXT("Offset X"), 0.0f, -1780, 260);
	auto* OffsetY = MakeScalar(TEXT("Offset Y"), 0.0f, -1780, 340);
	auto* OffsetVec = NewExpression<UMaterialExpressionAppendVector>(Material, -1650, 280);
	Wire(OffsetX, OffsetVec->A);
	Wire(OffsetY, OffsetVec->B);
	auto* UVScaled = NewExpression<UMaterialExpressionAdd>(Material, -1520, 120);
	Wire(UVTiled, UVScaled->A);
	Wire(OffsetVec, UVScaled->B);

	UTexture* WhiteTex = LoadEngineTexture({ TEXT("/Engine/EngineResources/WhiteSquareTexture") });
	UTexture* NormalTexDefault = LoadEngineTexture({
		TEXT("/Engine/EngineMaterials/DefaultNormal"),
		TEXT("/Engine/EngineMaterials/FlatNormal"),
		TEXT("/Engine/EngineMaterials/BaseFlattenedNormalMap") });

	// Defaults with compression matching their sampler type — mismatched defaults
	// (e.g. sRGB WhiteSquareTexture on a Masks sampler) are hard compile errors.
	const FString CorePath = GetDefault<UPoliigonBridgeSettings>()->ContentRoot / TEXT("Core");
	UTexture2D* WhiteMasks = GetOrCreateSolidTexture(
		TEXT("T_PoliigonDefault_ORM"), CorePath, TC_Masks, false, FColor::White);
	UTexture2D* WhiteGray = GetOrCreateSolidTexture(
		TEXT("T_PoliigonDefault_Gray"), CorePath, TC_Grayscale, false, FColor::White);
	SavePackageFor(WhiteMasks);
	SavePackageFor(WhiteGray);

	auto MakeTexParam = [&](const FName& Name, EMaterialSamplerType SamplerType, UTexture* Default, int32 Y)
	{
		auto* Param = NewExpression<UMaterialExpressionTextureSampleParameter2D>(Material, -1250, Y);
		Param->ParameterName = Name;
		Param->SamplerType = SamplerType;
		if (Default)
		{
			Param->Texture = Default;
		}
		Wire(UVScaled, Param->Coordinates);
		return Param;
	};

	auto* BaseColorTex = MakeTexParam(PARAM_BASECOLOR, SAMPLERTYPE_Color, WhiteTex, -300);
	auto* NormalTex = MakeTexParam(PARAM_NORMAL, SAMPLERTYPE_Normal, NormalTexDefault, -100);
	auto* OrmTex = MakeTexParam(PARAM_ORM, SAMPLERTYPE_Masks, WhiteMasks, 100);
	auto* RoughTex = MakeTexParam(PARAM_ROUGHNESS, SAMPLERTYPE_LinearGrayscale, WhiteGray, 300);
	auto* MetalTex = MakeTexParam(PARAM_METALLIC, SAMPLERTYPE_LinearGrayscale, WhiteGray, 500);
	auto* AOTex = MakeTexParam(PARAM_AO, SAMPLERTYPE_LinearGrayscale, WhiteGray, 700);
	auto* EmisTex = MakeTexParam(PARAM_EMISSION, SAMPLERTYPE_Color, WhiteTex, 900);
	auto* OpacTex = MakeTexParam(PARAM_OPACITY, SAMPLERTYPE_LinearGrayscale, WhiteGray, 1100);

	auto* Const0 = NewExpression<UMaterialExpressionConstant>(Material, -1000, 1300);
	Const0->R = 0.0f;
	auto* Const1 = NewExpression<UMaterialExpressionConstant>(Material, -1000, 1380);
	Const1->R = 1.0f;

	// ---- Base color grade: desaturate -> contrast -> tint (+ optional macro variation)
	auto* DesatAmount = MakeScalar(TEXT("Desaturation"), 0.0f, -1050, -430);
	auto* Desat = NewExpression<UMaterialExpressionDesaturation>(Material, -900, -360);
	Wire(BaseColorTex, Desat->Input);
	Wire(DesatAmount, Desat->Fraction);
	auto* ContrastParam = MakeScalar(TEXT("Contrast"), 1.0f, -900, -230);
	auto* ContrastPow = NewExpression<UMaterialExpressionPower>(Material, -750, -330);
	Wire(Desat, ContrastPow->Base);
	Wire(ContrastParam, ContrastPow->Exponent);
	auto* Tint = NewExpression<UMaterialExpressionVectorParameter>(Material, -750, -190);
	Tint->ParameterName = TEXT("Tint");
	Tint->DefaultValue = FLinearColor::White;
	auto* Tinted = NewExpression<UMaterialExpressionMultiply>(Material, -600, -310);
	Wire(ContrastPow, Tinted->A);
	Wire(Tint, Tinted->B);

	// Macro variation: re-sample base color at low frequency; its luminance breaks visible tiling.
	auto* MacroScale = MakeScalar(TEXT("Macro Variation Scale"), 0.05f, -1250, -700);
	auto* MacroUV = NewExpression<UMaterialExpressionMultiply>(Material, -1100, -650);
	Wire(UVScaled, MacroUV->A);
	Wire(MacroScale, MacroUV->B);
	auto* MacroSample = NewExpression<UMaterialExpressionTextureSampleParameter2D>(Material, -950, -700);
	MacroSample->ParameterName = PARAM_BASECOLOR; // same parameter, follows the instance override
	MacroSample->SamplerType = SAMPLERTYPE_Color;
	if (WhiteTex)
	{
		MacroSample->Texture = WhiteTex;
	}
	Wire(MacroUV, MacroSample->Coordinates);
	auto* MacroGray = NewExpression<UMaterialExpressionDesaturation>(Material, -800, -690);
	Wire(MacroSample, MacroGray->Input);
	Wire(Const1, MacroGray->Fraction);
	auto* Const2 = NewExpression<UMaterialExpressionConstant>(Material, -800, -590);
	Const2->R = 2.0f;
	auto* MacroX2 = NewExpression<UMaterialExpressionMultiply>(Material, -680, -660);
	Wire(MacroGray, MacroX2->A);
	Wire(Const2, MacroX2->B);
	auto* MacroIntensity = MakeScalar(TEXT("Macro Variation Intensity"), 0.5f, -680, -560);
	auto* MacroFactor = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -540, -630);
	MacroFactor->ConstA = 1.0f;
	Wire(MacroX2, MacroFactor->B);
	Wire(MacroIntensity, MacroFactor->Alpha);
	auto* MacroMul = NewExpression<UMaterialExpressionMultiply>(Material, -420, -420);
	Wire(Tinted, MacroMul->A);
	Wire(MacroFactor, MacroMul->B);
	auto* BaseFinal = MakeSwitch(SWITCH_MACRO, -280, -370, MacroMul, Tinted);

	// ---- Normal strength: lerp toward flat, renormalize
	auto* NormalStrength = MakeScalar(TEXT("Normal Strength"), 1.0f, -1050, -110);
	auto* FlatNormal = NewExpression<UMaterialExpressionConstant3Vector>(Material, -1050, -40);
	FlatNormal->Constant = FLinearColor(0.0f, 0.0f, 1.0f);
	auto* NormalLerp = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -880, -90);
	Wire(FlatNormal, NormalLerp->A);
	Wire(NormalTex, NormalLerp->B);
	Wire(NormalStrength, NormalLerp->Alpha);
	auto* NormalFinal = NewExpression<UMaterialExpressionNormalize>(Material, -720, -90);
	Wire(NormalLerp, NormalFinal->VectorInput);

	// Roughness = UseORM ? ORM.G : (Invert ? 1-Rough : Rough)
	auto* OneMinusRough = NewExpression<UMaterialExpressionOneMinus>(Material, -1000, 300);
	Wire(RoughTex, OneMinusRough->Input);
	auto* SwInvertRough = MakeSwitch(SWITCH_INVERT_ROUGH, -800, 300, OneMinusRough, RoughTex);
	auto* OrmG = NewExpression<UMaterialExpressionComponentMask>(Material, -1000, 150);
	OrmG->R = 0; OrmG->G = 1; OrmG->B = 0; OrmG->A = 0;
	Wire(OrmTex, OrmG->Input);
	auto* Roughness = MakeSwitch(SWITCH_USE_ORM, -600, 250, OrmG, SwInvertRough);

	// Roughness remap: lerp(Min, Max, roughness) — defaults are passthrough.
	auto* RoughMin = MakeScalar(TEXT("Roughness Min"), 0.0f, -600, 340);
	auto* RoughMax = MakeScalar(TEXT("Roughness Max"), 1.0f, -600, 410);
	auto* RoughFinal = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -420, 300);
	Wire(RoughMin, RoughFinal->A);
	Wire(RoughMax, RoughFinal->B);
	Wire(Roughness, RoughFinal->Alpha);

	// Metallic = UseORM ? ORM.B : (HasMetallic ? Metal : 0)
	auto* SwHasMetal = MakeSwitch(SWITCH_HAS_METALLIC, -800, 500, MetalTex, Const0);
	auto* OrmB = NewExpression<UMaterialExpressionComponentMask>(Material, -1000, 550);
	OrmB->R = 0; OrmB->G = 0; OrmB->B = 1; OrmB->A = 0;
	Wire(OrmTex, OrmB->Input);
	auto* Metallic = MakeSwitch(SWITCH_USE_ORM, -600, 500, OrmB, SwHasMetal);

	// AO = UseORM ? ORM.R : (HasAO ? AO : 1)
	auto* SwHasAO = MakeSwitch(SWITCH_HAS_AO, -800, 700, AOTex, Const1);
	auto* OrmR = NewExpression<UMaterialExpressionComponentMask>(Material, -1000, 750);
	OrmR->R = 1; OrmR->G = 0; OrmR->B = 0; OrmR->A = 0;
	Wire(OrmTex, OrmR->Input);
	auto* AO = MakeSwitch(SWITCH_USE_ORM, -600, 700, OrmR, SwHasAO);

	// AO intensity: lerp(1, AO, intensity)
	auto* AOIntensity = MakeScalar(TEXT("AO Intensity"), 1.0f, -600, 790);
	auto* AOFinal = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -420, 700);
	Wire(Const1, AOFinal->A);
	Wire(AO, AOFinal->B);
	Wire(AOIntensity, AOFinal->Alpha);

	// Emissive = HasEmission ? Emis * EmissiveTint * Strength : 0
	auto* EmisStrength = MakeScalar(TEXT("Emission Strength"), 1.0f, -1050, 980);
	auto* EmissiveTint = NewExpression<UMaterialExpressionVectorParameter>(Material, -1050, 1060);
	EmissiveTint->ParameterName = TEXT("Emissive Tint");
	EmissiveTint->DefaultValue = FLinearColor::White;
	auto* EmisTintMul = NewExpression<UMaterialExpressionMultiply>(Material, -900, 920);
	Wire(EmisTex, EmisTintMul->A);
	Wire(EmissiveTint, EmisTintMul->B);
	auto* EmisMul = NewExpression<UMaterialExpressionMultiply>(Material, -780, 900);
	Wire(EmisTintMul, EmisMul->A);
	Wire(EmisStrength, EmisMul->B);
	auto* Emissive = MakeSwitch(SWITCH_HAS_EMISSION, -600, 900, EmisMul, Const0);

	// OpacityMask = HasOpacity ? Opac : 1
	auto* OpacityMask = MakeSwitch(SWITCH_HAS_OPACITY, -600, 1100, OpacTex, Const1);

	// ---- Displacement: adjustable around mid-gray, wired to the Nanite displacement pin.
	// (To see it, enable Tessellation on this material and use Nanite meshes.)
	auto* DispTex = MakeTexParam(PARAM_DISPLACEMENT, SAMPLERTYPE_LinearGrayscale, WhiteGray, 1300);
	auto* DispIntensity = MakeScalar(TEXT("Displacement Intensity"), 1.0f, -1050, 1500);
	auto* DispAdj = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -880, 1420);
	DispAdj->ConstA = 0.5f;
	Wire(DispTex, DispAdj->B);
	Wire(DispIntensity, DispAdj->Alpha);
	auto* ConstHalf = NewExpression<UMaterialExpressionConstant>(Material, -880, 1560);
	ConstHalf->R = 0.5f;
	auto* Displacement = MakeSwitch(SWITCH_HAS_DISPLACEMENT, -600, 1440, DispAdj, ConstHalf);

	// --- Try an explicit Substrate slab first ---------------------------------
	bool bSubstrateWired = false;
	if (UClass* SlabClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionSubstrateSlabBSDF")))
	{
		UMaterialExpression* Slab = ELib::CreateMaterialExpression(Material, SlabClass);
		if (Slab)
		{
			Slab->MaterialExpressionEditorX = -150;
			Slab->MaterialExpressionEditorY = 0;

			// Metalness workflow -> slab: DiffuseAlbedo = Base*(1-M), F0 = lerp(0.04, Base, M)
			auto* OneMinusMetal = NewExpression<UMaterialExpressionOneMinus>(Material, -450, 380);
			Wire(Metallic, OneMinusMetal->Input);
			auto* DiffuseAlbedo = NewExpression<UMaterialExpressionMultiply>(Material, -350, -250);
			Wire(BaseFinal, DiffuseAlbedo->A);
			Wire(OneMinusMetal, DiffuseAlbedo->B);
			auto* F0 = NewExpression<UMaterialExpressionLinearInterpolate>(Material, -350, -120);
			F0->ConstA = 0.04f;
			Wire(BaseFinal, F0->B);
			Wire(Metallic, F0->Alpha);

			bool bAllConnected = true;
			bAllConnected &= WireByProp(DiffuseAlbedo, Slab, TEXT("DiffuseAlbedo"));
			bAllConnected &= WireByProp(F0, Slab, TEXT("F0"));
			bAllConnected &= WireByProp(RoughFinal, Slab, TEXT("Roughness"));
			bAllConnected &= WireByProp(NormalFinal, Slab, TEXT("Normal"));
			WireByProp(Emissive, Slab, TEXT("EmissiveColor"));

			if (bAllConnected && ELib::ConnectMaterialProperty(Slab, TEXT(""), MP_FrontMaterial))
			{
				ELib::ConnectMaterialProperty(AOFinal, TEXT(""), MP_AmbientOcclusion);
				ELib::ConnectMaterialProperty(OpacityMask, TEXT(""), MP_OpacityMask);
				bSubstrateWired = true;
				UE_LOG(LogPoliigonIngest, Log, TEXT("Master material wired with explicit Substrate slab."));
			}
		}
	}

	// --- Fallback: legacy attribute pins (auto-converted to a Substrate slab when r.Substrate=1)
	if (!bSubstrateWired)
	{
		bool bOk = true;
		bOk &= ELib::ConnectMaterialProperty(BaseFinal, TEXT(""), MP_BaseColor);
		bOk &= ELib::ConnectMaterialProperty(NormalFinal, TEXT(""), MP_Normal);
		bOk &= ELib::ConnectMaterialProperty(RoughFinal, TEXT(""), MP_Roughness);
		bOk &= ELib::ConnectMaterialProperty(Metallic, TEXT(""), MP_Metallic);
		ELib::ConnectMaterialProperty(AOFinal, TEXT(""), MP_AmbientOcclusion);
		ELib::ConnectMaterialProperty(Emissive, TEXT(""), MP_EmissiveColor);
		ELib::ConnectMaterialProperty(OpacityMask, TEXT(""), MP_OpacityMask);
		UE_LOG(LogPoliigonIngest, Log, TEXT("Master material wired with legacy pins (Substrate auto-conversion)."));
		if (!bOk)
		{
			return false;
		}
	}

	// Nanite displacement pin (both paths). Harmless if tessellation is off; enable
	// Tessellation on this material + Nanite meshes to see real displacement.
	ELib::ConnectMaterialProperty(Displacement, TEXT(""), MP_Displacement);

	return true;
}

// --- Texture import -------------------------------------------------------------

UTexture2D* FPoliigonIngest::ImportTexture(const FString& FilePath, const FString& DestPackagePath, EPoliigonMapSlot Slot)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->AddToRoot();
	Task->Filename = FilePath;
	Task->DestinationPath = DestPackagePath;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;

	TArray<UAssetImportTask*> Tasks{ Task };
	AssetTools.ImportAssetTasks(Tasks);

	UTexture2D* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Texture = Cast<UTexture2D>(Object);
		if (Texture)
		{
			break;
		}
	}
	Task->RemoveFromRoot();
	if (!Texture)
	{
		UE_LOG(LogPoliigonIngest, Warning, TEXT("Failed to import texture %s"), *FilePath);
		return nullptr;
	}

	switch (Slot)
	{
	case EPoliigonMapSlot::BaseColor:
	case EPoliigonMapSlot::Emission:
	case EPoliigonMapSlot::SSS:
	case EPoliigonMapSlot::Translucency:
		Texture->SRGB = true;
		Texture->CompressionSettings = TC_Default;
		break;
	case EPoliigonMapSlot::Normal:
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Normalmap;
		Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
		// Poliigon "Generic" normals are OpenGL (Y+); Unreal expects DirectX (Y-).
		Texture->bFlipGreenChannel = true;
		break;
	case EPoliigonMapSlot::ORM:
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Masks;
		break;
	default: // grayscale masks: roughness/gloss/metallic/AO/opacity/displacement
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Grayscale;
		break;
	}
	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	return Texture;
}

// --- Texture set collection ---------------------------------------------------

TArray<FPoliigonIngest::FTextureSet> FPoliigonIngest::CollectTextureSets(const FString& StagingDir)
{
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *StagingDir, TEXT("*.*"), true, false);
	Files.Sort();

	TMap<FString, FTextureSet> Sets;
	TMap<FString, TMap<EPoliigonMapSlot, bool>> SlotIs16Bit; // prefix -> slot -> current file is 16-bit variant

	for (const FString& File : Files)
	{
		if (!IsTextureFile(File))
		{
			continue;
		}
		EPoliigonMapSlot Slot = EPoliigonMapSlot::Unknown;
		FString Prefix;
		bool b16Bit = false;
		if (!ParseTextureFileName(File, Slot, Prefix, b16Bit))
		{
			continue;
		}
		FTextureSet& Set = Sets.FindOrAdd(Prefix);
		Set.Prefix = Prefix;
		TMap<EPoliigonMapSlot, bool>& SlotBits = SlotIs16Bit.FindOrAdd(Prefix);
		if (const FString* Existing = Set.Files.Find(Slot))
		{
			// Prefer 8-bit over 16-bit duplicates (NRM vs NRM16, DISP vs DISP16).
			const bool bExisting16 = SlotBits.FindRef(Slot);
			if (!bExisting16 || b16Bit)
			{
				continue;
			}
		}
		Set.Files.Add(Slot, File);
		SlotBits.Add(Slot, b16Bit);
	}

	TArray<FTextureSet> Out;
	Sets.GenerateValueArray(Out);
	return Out;
}

// --- Material instance ---------------------------------------------------------

UMaterialInstanceConstant* FPoliigonIngest::CreateInstanceForSet(
	const FTextureSet& Set, const FString& InstanceName, const FString& DestPackagePath,
	UMaterial* Master, FString& OutError)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Import all textures of the set first.
	TMap<EPoliigonMapSlot, UTexture2D*> Textures;
	for (const TPair<EPoliigonMapSlot, FString>& Pair : Set.Files)
	{
		if (Pair.Key == EPoliigonMapSlot::Unknown)
		{
			continue;
		}
		if (UTexture2D* Tex = ImportTexture(Pair.Value, DestPackagePath, Pair.Key))
		{
			Textures.Add(Pair.Key, Tex);
		}
	}
	if (Textures.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No textures imported for set %s"), *Set.Prefix);
		return nullptr;
	}

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Master;
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
		AssetTools.CreateAsset(InstanceName, DestPackagePath, UMaterialInstanceConstant::StaticClass(), Factory));
	if (!Instance)
	{
		// Asset may already exist from a previous run — load and update it.
		const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *DestPackagePath, *InstanceName, *InstanceName);
		Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	}
	if (!Instance)
	{
		OutError = FString::Printf(TEXT("Failed to create material instance %s"), *InstanceName);
		return nullptr;
	}

	auto SetTex = [&](const FName& Param, EPoliigonMapSlot Slot)
	{
		if (UTexture2D* const* Tex = Textures.Find(Slot))
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Param, *Tex);
		}
	};
	SetTex(PARAM_BASECOLOR, EPoliigonMapSlot::BaseColor);
	SetTex(PARAM_NORMAL, EPoliigonMapSlot::Normal);
	SetTex(PARAM_ORM, EPoliigonMapSlot::ORM);
	SetTex(PARAM_METALLIC, EPoliigonMapSlot::Metallic);
	SetTex(PARAM_AO, EPoliigonMapSlot::AO);
	SetTex(PARAM_EMISSION, EPoliigonMapSlot::Emission);
	SetTex(PARAM_OPACITY, EPoliigonMapSlot::Opacity);
	SetTex(PARAM_DISPLACEMENT, EPoliigonMapSlot::Displacement);

	// Roughness slot: a real roughness map, or a gloss map + invert switch.
	const bool bHasORM = Textures.Contains(EPoliigonMapSlot::ORM);
	const bool bHasRough = Textures.Contains(EPoliigonMapSlot::Roughness);
	const bool bHasGloss = Textures.Contains(EPoliigonMapSlot::Gloss);
	if (bHasRough)
	{
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
			Instance, PARAM_ROUGHNESS, Textures[EPoliigonMapSlot::Roughness]);
	}
	else if (bHasGloss)
	{
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
			Instance, PARAM_ROUGHNESS, Textures[EPoliigonMapSlot::Gloss]);
	}

	auto SetSwitch = [&](const FName& Name, bool bValue)
	{
		Instance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(Name), bValue);
	};
	SetSwitch(SWITCH_USE_ORM, bHasORM);
	SetSwitch(SWITCH_INVERT_ROUGH, !bHasRough && bHasGloss);
	SetSwitch(SWITCH_HAS_METALLIC, Textures.Contains(EPoliigonMapSlot::Metallic));
	SetSwitch(SWITCH_HAS_AO, Textures.Contains(EPoliigonMapSlot::AO));
	SetSwitch(SWITCH_HAS_EMISSION, Textures.Contains(EPoliigonMapSlot::Emission));
	SetSwitch(SWITCH_HAS_OPACITY, Textures.Contains(EPoliigonMapSlot::Opacity));
	SetSwitch(SWITCH_HAS_DISPLACEMENT, Textures.Contains(EPoliigonMapSlot::Displacement));

	if (Textures.Contains(EPoliigonMapSlot::Opacity))
	{
		Instance->BasePropertyOverrides.bOverride_BlendMode = true;
		Instance->BasePropertyOverrides.BlendMode = BLEND_Masked;
	}

	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->PostEditChange();
	Instance->MarkPackageDirty();

	// Save textures + instance.
	for (const TPair<EPoliigonMapSlot, UTexture2D*>& Pair : Textures)
	{
		SavePackageFor(Pair.Value);
	}
	SavePackageFor(Instance);
	return Instance;
}

// --- Per-type ingest -------------------------------------------------------------

FPoliigonIngest::FResult FPoliigonIngest::IngestTextureAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir)
{
	FResult Result;
	const UPoliigonBridgeSettings* Settings = GetDefault<UPoliigonBridgeSettings>();
	const FString AssetFolder = SanitizeAssetName(Info.AssetName);
	const FString DestPath = Settings->ContentRoot / TEXT("Materials") / AssetFolder;

	FString Error;
	UMaterial* Master = EnsureMasterMaterial(Error);
	if (!Master)
	{
		Result.Error = Error;
		return Result;
	}

	TArray<FTextureSet> Sets = CollectTextureSets(StagingDir);
	if (Sets.Num() == 0)
	{
		Result.Error = TEXT("No recognizable texture maps found in the download.");
		return Result;
	}

	for (const FTextureSet& Set : Sets)
	{
		const FString InstanceName = FString::Printf(TEXT("MI_%s"), *SanitizeAssetName(Set.Prefix));
		UMaterialInstanceConstant* Instance = CreateInstanceForSet(Set, InstanceName, DestPath, Master, Error);
		if (!Instance)
		{
			Result.Error = Error;
			return Result;
		}
		Result.CreatedAssetPaths.Add(Instance->GetPathName());
	}
	Result.bOk = true;
	return Result;
}

FPoliigonIngest::FResult FPoliigonIngest::IngestModelAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir)
{
	FResult Result;
	const UPoliigonBridgeSettings* Settings = GetDefault<UPoliigonBridgeSettings>();
	const FString AssetFolder = SanitizeAssetName(Info.AssetName);
	const FString DestPath = Settings->ContentRoot / TEXT("Models") / AssetFolder;
	const FString TexturesPath = DestPath / TEXT("Textures");

	FString Error;
	UMaterial* Master = EnsureMasterMaterial(Error);
	if (!Master)
	{
		Result.Error = Error;
		return Result;
	}

	// 1) Build material instances from the shipped textures.
	TArray<FTextureSet> Sets = CollectTextureSets(StagingDir);
	TArray<TPair<FString, UMaterialInstanceConstant*>> Instances; // set prefix -> MIC
	for (const FTextureSet& Set : Sets)
	{
		const FString InstanceName = FString::Printf(TEXT("MI_%s"), *SanitizeAssetName(Set.Prefix));
		if (UMaterialInstanceConstant* Instance = CreateInstanceForSet(Set, InstanceName, TexturesPath, Master, Error))
		{
			Instances.Emplace(Set.Prefix, Instance);
			Result.CreatedAssetPaths.Add(Instance->GetPathName());
		}
	}

	// 2) Import FBX meshes (prefer LOD0 when LOD-suffixed variants ship).
	TArray<FString> FbxFiles;
	IFileManager::Get().FindFilesRecursive(FbxFiles, *StagingDir, TEXT("*.fbx"), true, false);
	FbxFiles.Sort();
	TArray<FString> ToImport;
	for (const FString& Fbx : FbxFiles)
	{
		const FString Base = FPaths::GetBaseFilename(Fbx).ToUpper();
		if (Base.Contains(TEXT("_LOD")) && !Base.EndsWith(TEXT("_LOD0")))
		{
			continue;
		}
		if (Base.EndsWith(TEXT("_SOURCE")))
		{
			continue;
		}
		ToImport.Add(Fbx);
	}
	if (ToImport.Num() == 0)
	{
		ToImport = FbxFiles;
	}
	if (ToImport.Num() == 0)
	{
		Result.Error = TEXT("No FBX files found in the model download.");
		return Result;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	TArray<UStaticMesh*> Meshes;
	for (const FString& Fbx : ToImport)
	{
		UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
		ImportUI->bImportMesh = true;
		ImportUI->bImportAsSkeletal = false;
		ImportUI->bImportAnimations = false;
		ImportUI->bImportMaterials = false;
		ImportUI->bImportTextures = false;
		ImportUI->MeshTypeToImport = FBXIT_StaticMesh;
		ImportUI->StaticMeshImportData->bCombineMeshes = true;
		ImportUI->StaticMeshImportData->bAutoGenerateCollision = true;

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->AddToRoot();
		Task->Filename = Fbx;
		Task->DestinationPath = DestPath;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		Task->Options = ImportUI;

		TArray<UAssetImportTask*> Tasks{ Task };
		AssetTools.ImportAssetTasks(Tasks);
		for (UObject* Object : Task->GetObjects())
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
			{
				Meshes.Add(Mesh);
			}
		}
		Task->RemoveFromRoot();
	}
	if (Meshes.Num() == 0)
	{
		Result.Error = TEXT("FBX import produced no static meshes.");
		return Result;
	}

	// 3) Bind material slots to instances by fuzzy name match.
	auto Normalize = [](const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	};

	for (UStaticMesh* Mesh : Meshes)
	{
		const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
		for (int32 SlotIdx = 0; SlotIdx < Slots.Num(); ++SlotIdx)
		{
			UMaterialInstanceConstant* Best = nullptr;
			if (Instances.Num() == 1)
			{
				Best = Instances[0].Value;
			}
			else if (Instances.Num() > 1)
			{
				const FString SlotName = Normalize(Slots[SlotIdx].ImportedMaterialSlotName.ToString());
				int32 BestScore = -1;
				for (const TPair<FString, UMaterialInstanceConstant*>& Pair : Instances)
				{
					const FString SetName = Normalize(Pair.Key);
					int32 Score = -1;
					if (!SlotName.IsEmpty() && (SetName.Contains(SlotName) || SlotName.Contains(SetName)))
					{
						Score = FMath::Min(SetName.Len(), SlotName.Len());
					}
					if (Score > BestScore)
					{
						BestScore = Score;
						Best = Pair.Value;
					}
				}
				if (BestScore < 0)
				{
					Best = Instances[0].Value; // no name overlap at all: fall back to first set
				}
			}
			if (Best)
			{
				Mesh->SetMaterial(SlotIdx, Best);
			}
		}
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		SavePackageFor(Mesh);
		Result.CreatedAssetPaths.Add(Mesh->GetPathName());
	}

	Result.bOk = true;
	return Result;
}

FPoliigonIngest::FResult FPoliigonIngest::IngestAsset(const FPoliigonAssetInfo& Info, const FString& StagingDir)
{
	check(IsInGameThread());
	FScopedSlowTask SlowTask(1.0f,
		FText::Format(LOCTEXT("IngestTask", "Importing {0} from Poliigon..."), FText::FromString(Info.DisplayName)));
	SlowTask.MakeDialog();
	SlowTask.EnterProgressFrame(1.0f);

	if (Info.Type == EPoliigonAssetType::Model)
	{
		return IngestModelAsset(Info, StagingDir);
	}
	return IngestTextureAsset(Info, StagingDir);
}

void FPoliigonIngest::SavePackageFor(UObject* Object)
{
	if (!Object)
	{
		return;
	}
	UPackage* Package = Object->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	UPackage::SavePackage(Package, nullptr, *Filename, Args);
}

#undef LOCTEXT_NAMESPACE
