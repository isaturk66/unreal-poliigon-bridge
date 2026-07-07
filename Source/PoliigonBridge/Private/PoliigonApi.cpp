// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#include "PoliigonApi.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogPoliigonApi, Log, All);

namespace
{
	const TCHAR* PLUGIN_VERSION = TEXT("0.1.0");
	const TCHAR* PLATFORM_NAME = TEXT("addon-unreal"); // recognized platform id, see addon api.py
	const TCHAR* SOFTWARE_NAME = TEXT("unreal");

	// Public search-only Algolia credentials shipped inside every Poliigon addon.
	const TCHAR* ALGOLIA_URL = TEXT("https://ci6l4fta7s-dsn.algolia.net/1/indexes/assets/query");
	const TCHAR* ALGOLIA_KEY = TEXT("77a376fa69270eee8aa24f82d400b3d3");
	const TCHAR* ALGOLIA_APP = TEXT("CI6L4FTA7S");

	// Convention 1 format preference (matches addon SUPPORTED_TEX_FORMATS).
	const TCHAR* FORMAT_PRIORITY[] = { TEXT("jpg"), TEXT("png"), TEXT("tiff"), TEXT("exr"), TEXT("hdr") };

	FString JsonToString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	bool LooksLikePreviewFile(const FString& FileName)
	{
		const FString Base = FPaths::GetBaseFilename(FileName).ToLower();
		static const TCHAR* Suffixes[] = {
			TEXT("_sphere"), TEXT("_cube"), TEXT("_flat"), TEXT("_cylinder"),
			TEXT("_atlas"), TEXT("_fabric")
		};
		if (Base.Contains(TEXT("preview")))
		{
			return true;
		}
		for (const TCHAR* Suffix : Suffixes)
		{
			if (Base.EndsWith(Suffix))
			{
				return true;
			}
		}
		return false;
	}
}

FPoliigonApi::FPoliigonApi()
{
	LoadToken();
}

FPoliigonApi::~FPoliigonApi()
{
	if (LoginTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LoginTickerHandle);
		LoginTickerHandle.Reset();
	}
}

// --- Token persistence ----------------------------------------------------

FString FPoliigonApi::TokenFilePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PoliigonBridge"), TEXT("token.txt"));
}

void FPoliigonApi::LoadToken()
{
	FString Loaded;
	if (FFileHelper::LoadFileToString(Loaded, *TokenFilePath()))
	{
		Token = Loaded.TrimStartAndEnd();
	}
}

void FPoliigonApi::SaveToken() const
{
	FFileHelper::SaveStringToFile(Token, *TokenFilePath());
}

void FPoliigonApi::ClearToken()
{
	Token.Empty();
	IFileManager::Get().Delete(*TokenFilePath());
}

// --- Request helpers --------------------------------------------------------

FHttpRequestRef FPoliigonApi::MakeRequest(const FString& Url, const FString& Verb, bool bAuth) const
{
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Verb);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetTimeout(60.0f);
	if (bAuth && !Token.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	}
	return Request;
}

TSharedPtr<FJsonObject> FPoliigonApi::ParseJson(const FHttpResponsePtr& Response)
{
	if (!Response.IsValid())
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	FJsonSerializer::Deserialize(Reader, Root);
	if (!Root.IsValid())
	{
		return nullptr;
	}
	// Poliigon API v2 nests the payload under a "results" key on (almost) every
	// response; mirror the addon's _flatten_results() and merge it up.
	const TSharedPtr<FJsonObject>* Results = nullptr;
	if (Root->TryGetObjectField(TEXT("results"), Results) && Results->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
		{
			if (Pair.Key != TEXT("results") && !(*Results)->HasField(Pair.Key))
			{
				(*Results)->SetField(Pair.Key, Pair.Value);
			}
		}
		return *Results;
	}
	return Root;
}

TSharedPtr<FJsonObject> FPoliigonApi::MakeMeta()
{
	// The addon attaches meta to every POST; optin=false disables all tracking fields.
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetBoolField(TEXT("optin"), false);
	Meta->SetBoolField(TEXT("mp"), false);
	return Meta;
}

FString FPoliigonApi::WithFormatSuffix(const FString& Path)
{
	// api.py appends format=plain to every authenticated request.
	return Path + (Path.Contains(TEXT("?")) ? TEXT("&format=plain") : TEXT("?format=plain"));
}

// --- Auth -------------------------------------------------------------------

void FPoliigonApi::CheckAuth(TFunction<void(bool, FString, int32)> OnDone)
{
	if (Token.IsEmpty())
	{
		OnDone(false, FString(), 0);
		return;
	}
	// The addon calls /me as POST (api.py get_user_info).
	FHttpRequestRef Request = MakeRequest(ApiUrl + WithFormatSuffix(TEXT("/me")), TEXT("POST"), true);
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("meta"), MakeMeta());
	Request->SetContentAsString(JsonToString(Payload));
	TSharedRef<FPoliigonApi> Self = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[Self, OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const bool bValid = bConnected && Response.IsValid() && Response->GetResponseCode() == 200;
			if (!bValid && Response.IsValid() && Response->GetResponseCode() == 401)
			{
				Self->ClearToken();
			}
			FString UserName;
			int32 Credits = 0;
			if (bValid)
			{
				const TSharedPtr<FJsonObject> Body = ParseJson(Response);
				if (Body.IsValid())
				{
					const TSharedPtr<FJsonObject>* UserObj = nullptr;
					if (Body->TryGetObjectField(TEXT("user"), UserObj))
					{
						(*UserObj)->TryGetStringField(TEXT("name"), UserName);
					}
					const TSharedPtr<FJsonObject>* CreditsObj = nullptr;
					if (Body->TryGetObjectField(TEXT("credits"), CreditsObj))
					{
						double Sub = 0, OnDemand = 0;
						(*CreditsObj)->TryGetNumberField(TEXT("subscription_balance"), Sub);
						(*CreditsObj)->TryGetNumberField(TEXT("ondemand_balance"), OnDemand);
						Credits = static_cast<int32>(Sub + OnDemand);
					}
				}
			}
			OnDone(bValid, UserName, Credits);
		});
	Request->ProcessRequest();
}

void FPoliigonApi::StartWebsiteLogin(TFunction<void(bool, FString)> OnDone)
{
	if (IsLoginPending())
	{
		OnDone(false, TEXT("A login is already in progress."));
		return;
	}
	LoginCallback = MoveTemp(OnDone);

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("platform"), PLATFORM_NAME);
	const TSharedPtr<FJsonObject> Meta = MakeMeta();
	Meta->SetStringField(TEXT("addon_version"), PLUGIN_VERSION);
	Meta->SetStringField(TEXT("software_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Meta->SetStringField(TEXT("software_name"), SOFTWARE_NAME);
	Payload->SetObjectField(TEXT("meta"), Meta);

	FHttpRequestRef Request = MakeRequest(ApiUrl + TEXT("/initiate/login"), TEXT("POST"), false);
	Request->SetContentAsString(JsonToString(Payload));

	TSharedRef<FPoliigonApi> Self = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[Self](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const TSharedPtr<FJsonObject> Body = bConnected ? ParseJson(Response) : nullptr;
			FString LoginUrl, NewLoginToken;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("login_url"), LoginUrl);
				Body->TryGetStringField(TEXT("login_token"), NewLoginToken);
			}
			if (LoginUrl.IsEmpty() || NewLoginToken.IsEmpty())
			{
				UE_LOG(LogPoliigonApi, Error,
					TEXT("initiate/login failed. Connected=%d HTTP=%d Body=%s"),
					bConnected ? 1 : 0,
					Response.IsValid() ? Response->GetResponseCode() : 0,
					Response.IsValid() ? *Response->GetContentAsString().Left(2000) : TEXT("<none>"));
				Self->FinishLogin(false, TEXT("Failed to initiate website login (see Output Log, filter LogPoliigonApi)."));
				return;
			}
			UE_LOG(LogPoliigonApi, Log, TEXT("initiate/login ok, opening browser."));
			Self->LoginToken = NewLoginToken;
			Self->LoginDeadline = FPlatformTime::Seconds() + 300.0;

			FPlatformProcess::LaunchURL(*LoginUrl, nullptr, nullptr);

			// Poll /validate/login every 2 seconds until the user completes web login.
			Self->LoginTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakSelf = TWeakPtr<FPoliigonApi>(Self)](float) -> bool
				{
					if (const TSharedPtr<FPoliigonApi> Pinned = WeakSelf.Pin())
					{
						Pinned->PollValidateLogin();
						return true; // keep ticking; FinishLogin removes the ticker
					}
					return false;
				}), 2.0f);
		});
	Request->ProcessRequest();
}

void FPoliigonApi::PollValidateLogin()
{
	if (FPlatformTime::Seconds() > LoginDeadline)
	{
		FinishLogin(false, TEXT("Login timed out after 5 minutes."));
		return;
	}
	if (bValidateInFlight)
	{
		return;
	}
	bValidateInFlight = true;

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("platform"), PLATFORM_NAME);
	Payload->SetStringField(TEXT("login_token"), LoginToken);
	Payload->SetObjectField(TEXT("meta"), MakeMeta());

	FHttpRequestRef Request = MakeRequest(ApiUrl + TEXT("/validate/login"), TEXT("POST"), false);
	Request->SetContentAsString(JsonToString(Payload));

	TSharedRef<FPoliigonApi> Self = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[Self](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			Self->bValidateInFlight = false;
			if (!bConnected || !Response.IsValid())
			{
				return; // transient; keep polling
			}
			const TSharedPtr<FJsonObject> Body = ParseJson(Response);
			if (!Body.IsValid())
			{
				return;
			}
			FString AccessToken, Status, Message;
			Body->TryGetStringField(TEXT("access_token"), AccessToken);
			Body->TryGetStringField(TEXT("status"), Status);
			Body->TryGetStringField(TEXT("message"), Message);
			const bool bSuccess = !AccessToken.IsEmpty()
				&& (Status.ToLower() == TEXT("success") || Message.ToLower() == TEXT("successful login") || !AccessToken.IsEmpty());
			if (bSuccess)
			{
				UE_LOG(LogPoliigonApi, Log, TEXT("validate/login succeeded."));
				Self->Token = AccessToken;
				Self->SaveToken();
				Self->FinishLogin(true, FString());
			}
			else
			{
				// Still pending (typically 401 until the user finishes in the browser).
				UE_LOG(LogPoliigonApi, Verbose, TEXT("validate/login pending. HTTP=%d Body=%s"),
					Response->GetResponseCode(), *Response->GetContentAsString().Left(500));
			}
		});
	Request->ProcessRequest();
}

void FPoliigonApi::FinishLogin(bool bOk, const FString& Error)
{
	if (LoginTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LoginTickerHandle);
		LoginTickerHandle.Reset();
	}
	LoginToken.Empty();
	if (LoginCallback)
	{
		TFunction<void(bool, FString)> Callback = MoveTemp(LoginCallback);
		LoginCallback = nullptr;
		Callback(bOk, Error);
	}
}

void FPoliigonApi::CancelLogin()
{
	if (IsLoginPending())
	{
		FinishLogin(false, TEXT("Login cancelled."));
	}
}

// --- Browse -----------------------------------------------------------------

void FPoliigonApi::SearchAssets(const FString& Query, int32 PageOneBased, int32 PageSize, bool bFreeOnly,
	TFunction<void(bool, TArray<int32>, int32, int32, FString)> OnDone)
{
	SearchAssetsInternal(Query, PageOneBased, PageSize, bFreeOnly, /*bUseNumericFilter=*/true, MoveTemp(OnDone));
}

void FPoliigonApi::SearchAssetsInternal(const FString& Query, int32 PageOneBased, int32 PageSize,
	bool bFreeOnly, bool bUseNumericFilter,
	TFunction<void(bool, TArray<int32>, int32, int32, FString)> OnDone)
{
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("query"), Query);
	{
		TArray<TSharedPtr<FJsonValue>> Attrs;
		Attrs.Add(MakeShared<FJsonValueString>(TEXT("AssetID")));
		Payload->SetArrayField(TEXT("attributesToRetrieve"), Attrs);
	}
	Payload->SetNumberField(TEXT("page"), FMath::Max(PageOneBased - 1, 0)); // Algolia is 0-based
	Payload->SetNumberField(TEXT("hitsPerPage"), PageSize);
	{
		TArray<TSharedPtr<FJsonValue>> Facets;
		Facets.Add(MakeShared<FJsonValueString>(TEXT("ReleaseStatus:-Unpublished")));
		Facets.Add(MakeShared<FJsonValueString>(TEXT("ReleaseStatus:-Staging")));
		Payload->SetArrayField(TEXT("facetFilters"), Facets);
	}
	if (bFreeOnly && bUseNumericFilter)
	{
		// Server-side free filter; the index may not allow it (falls back below).
		TArray<TSharedPtr<FJsonValue>> Numeric;
		Numeric.Add(MakeShared<FJsonValueString>(TEXT("Credit=0")));
		Payload->SetArrayField(TEXT("numericFilters"), Numeric);
	}

	FHttpRequestRef Request = MakeRequest(ALGOLIA_URL, TEXT("POST"), false);
	Request->SetHeader(TEXT("x-algolia-api-key"), ALGOLIA_KEY);
	Request->SetHeader(TEXT("x-algolia-application-id"), ALGOLIA_APP);
	Request->SetContentAsString(JsonToString(Payload));

	TSharedRef<FPoliigonApi> Self = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[Self, OnDone, Query, PageOneBased, PageSize, bFreeOnly, bUseNumericFilter]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const bool bHttpOk = bConnected && Response.IsValid() && Response->GetResponseCode() == 200;
			if (!bHttpOk && bFreeOnly && bUseNumericFilter)
			{
				// Index rejected the numeric filter — retry unfiltered; the UI filters by credits client-side.
				UE_LOG(LogPoliigonApi, Warning,
					TEXT("Algolia numericFilters Credit=0 rejected (HTTP %d), retrying unfiltered."),
					Response.IsValid() ? Response->GetResponseCode() : 0);
				Self->SearchAssetsInternal(Query, PageOneBased, PageSize, bFreeOnly, false, OnDone);
				return;
			}
			const TSharedPtr<FJsonObject> Body = bHttpOk ? ParseJson(Response) : nullptr;
			if (!Body.IsValid())
			{
				OnDone(false, {}, 0, 0, TEXT("Search request failed."));
				return;
			}
			TArray<int32> Ids;
			const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
			if (Body->TryGetArrayField(TEXT("hits"), Hits))
			{
				for (const TSharedPtr<FJsonValue>& Hit : *Hits)
				{
					const TSharedPtr<FJsonObject>* HitObj;
					if (Hit->TryGetObject(HitObj))
					{
						double IdNum = 0;
						if ((*HitObj)->TryGetNumberField(TEXT("AssetID"), IdNum))
						{
							Ids.Add(static_cast<int32>(IdNum));
						}
					}
				}
			}
			int32 TotalPages = 0, TotalHits = 0;
			double Num = 0;
			if (Body->TryGetNumberField(TEXT("nbPages"), Num)) { TotalPages = static_cast<int32>(Num); }
			if (Body->TryGetNumberField(TEXT("nbHits"), Num)) { TotalHits = static_cast<int32>(Num); }
			OnDone(true, MoveTemp(Ids), TotalPages, TotalHits, FString());
		});
	Request->ProcessRequest();
}

bool FPoliigonApi::ParseAssetInfo(const TSharedPtr<FJsonObject>& Root, FPoliigonAssetInfo& Out)
{
	if (!Root.IsValid())
	{
		return false;
	}
	// The asset dict is either the body itself or nested under "data".
	TSharedPtr<FJsonObject> Asset = Root;
	if (!Root->HasField(TEXT("AssetID")))
	{
		const TSharedPtr<FJsonObject>* Data;
		if (Root->TryGetObjectField(TEXT("data"), Data) && (*Data)->HasField(TEXT("AssetID")))
		{
			Asset = *Data;
		}
		else
		{
			return false;
		}
	}

	double IdNum = 0;
	Asset->TryGetNumberField(TEXT("AssetID"), IdNum);
	Out.AssetId = static_cast<int32>(IdNum);
	Asset->TryGetStringField(TEXT("AssetName"), Out.AssetName);
	Asset->TryGetStringField(TEXT("Name"), Out.DisplayName);
	if (Out.DisplayName.IsEmpty())
	{
		Out.DisplayName = Out.AssetName;
	}

	FString TypeStr;
	Asset->TryGetStringField(TEXT("Type"), TypeStr);
	TypeStr = TypeStr.ToLower();
	if (TypeStr.Contains(TEXT("texture"))) { Out.Type = EPoliigonAssetType::Texture; }
	else if (TypeStr.Contains(TEXT("model"))) { Out.Type = EPoliigonAssetType::Model; }
	else if (TypeStr.Contains(TEXT("hdri"))) { Out.Type = EPoliigonAssetType::HDRI; }
	else if (TypeStr.Contains(TEXT("brush"))) { Out.Type = EPoliigonAssetType::Brush; }
	else { Out.Type = EPoliigonAssetType::Unsupported; }

	double ConvNum = 0;
	if (Asset->TryGetNumberField(TEXT("Convention"), ConvNum))
	{
		Out.Convention = static_cast<int32>(ConvNum);
	}
	else
	{
		FString ConvStr;
		if (Asset->TryGetStringField(TEXT("Convention"), ConvStr))
		{
			Out.Convention = FCString::Atoi(*ConvStr);
		}
	}

	double CreditNum = 1;
	Asset->TryGetNumberField(TEXT("Credit"), CreditNum);
	Out.Credits = static_cast<int32>(CreditNum);

	const TArray<TSharedPtr<FJsonValue>>* Resolutions = nullptr;
	if (Asset->TryGetArrayField(TEXT("Resolutions"), Resolutions))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Resolutions)
		{
			FString Size;
			if (Value->TryGetString(Size))
			{
				Out.Resolutions.Add(Size);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* MapsArray = nullptr;
	if (Asset->TryGetArrayField(TEXT("Maps"), MapsArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *MapsArray)
		{
			const TSharedPtr<FJsonObject>* MapObj;
			if (!Value->TryGetObject(MapObj))
			{
				continue;
			}
			if (Out.Convention >= 1)
			{
				// convention 1: [{type:"BaseColor", file_formats:["jpg",...]}]
				FPoliigonMapDesc Desc;
				(*MapObj)->TryGetStringField(TEXT("type"), Desc.TypeCode);
				const TArray<TSharedPtr<FJsonValue>>* Formats = nullptr;
				if ((*MapObj)->TryGetArrayField(TEXT("file_formats"), Formats))
				{
					for (const TSharedPtr<FJsonValue>& Fmt : *Formats)
					{
						FString FmtStr;
						if (Fmt->TryGetString(FmtStr))
						{
							Desc.FileFormats.Add(FmtStr);
						}
					}
				}
				if (!Desc.TypeCode.IsEmpty())
				{
					Out.Maps.Add(MoveTemp(Desc));
				}
			}
			else
			{
				// convention 0: [{workflow:"METALNESS", maps:["COL","NRM",...]}]
				FString Workflow = TEXT("REGULAR");
				(*MapObj)->TryGetStringField(TEXT("workflow"), Workflow);
				TArray<FString>& Codes = Out.WorkflowMaps.FindOrAdd(Workflow);
				const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
				if ((*MapObj)->TryGetArrayField(TEXT("maps"), Names))
				{
					for (const TSharedPtr<FJsonValue>& Name : *Names)
					{
						FString Code;
						if (Name->TryGetString(Code))
						{
							Codes.AddUnique(Code);
						}
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Previews = nullptr;
	if (Asset->TryGetArrayField(TEXT("Previews"), Previews))
	{
		double BestPos = TNumericLimits<double>::Max();
		for (const TSharedPtr<FJsonValue>& Value : *Previews)
		{
			const TSharedPtr<FJsonObject>* Thumb;
			if (!Value->TryGetObject(Thumb))
			{
				continue;
			}
			FString BaseUrl;
			(*Thumb)->TryGetStringField(TEXT("baseUrl"), BaseUrl);
			if (BaseUrl.IsEmpty())
			{
				continue;
			}
			double Pos = TNumericLimits<double>::Max() - 1.0;
			(*Thumb)->TryGetNumberField(TEXT("position"), Pos);
			if (Pos < BestPos)
			{
				BestPos = Pos;
				Out.ThumbBaseUrl = BaseUrl;
			}
		}
	}

	Out.bDetailsLoaded = true;
	return Out.AssetId != 0;
}

void FPoliigonApi::GetAssetDetails(int32 AssetId, TFunction<void(bool, FPoliigonAssetInfo, FString)> OnDone)
{
	const FString Path = FString::Printf(TEXT("/assets/details/%d?language_code=en"), AssetId);
	FHttpRequestRef Request = MakeRequest(ApiUrl + WithFormatSuffix(Path), TEXT("GET"), true);
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, AssetId](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FPoliigonAssetInfo Info;
			const TSharedPtr<FJsonObject> Body = bConnected ? ParseJson(Response) : nullptr;
			if (ParseAssetInfo(Body, Info))
			{
				OnDone(true, MoveTemp(Info), FString());
			}
			else
			{
				OnDone(false, MoveTemp(Info),
					FString::Printf(TEXT("Failed to fetch details for asset %d (HTTP %d)."),
						AssetId, Response.IsValid() ? Response->GetResponseCode() : 0));
			}
		});
	Request->ProcessRequest();
}

void FPoliigonApi::FetchPurchasedIds(TFunction<void(bool, TSet<int32>)> OnDone)
{
	FHttpRequestRef Request = MakeRequest(ApiUrl + WithFormatSuffix(TEXT("/assets/list/ids")), TEXT("GET"), true);
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			TSet<int32> Ids;
			const TSharedPtr<FJsonObject> Body = bConnected ? ParseJson(Response) : nullptr;
			if (!Body.IsValid())
			{
				OnDone(false, MoveTemp(Ids));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* IdArray = nullptr;
			if (Body->TryGetArrayField(TEXT("asset_ids"), IdArray))
			{
				for (const TSharedPtr<FJsonValue>& Value : *IdArray)
				{
					double Num = 0;
					FString Str;
					if (Value->TryGetNumber(Num))
					{
						Ids.Add(static_cast<int32>(Num));
					}
					else if (Value->TryGetString(Str))
					{
						Ids.Add(FCString::Atoi(*Str));
					}
				}
			}
			OnDone(true, MoveTemp(Ids));
		});
	Request->ProcessRequest();
}

void FPoliigonApi::DownloadThumb(const FString& ThumbBaseUrl, int32 Pixels, const FString& DestFile,
	TFunction<void(bool)> OnDone)
{
	if (ThumbBaseUrl.IsEmpty())
	{
		OnDone(false);
		return;
	}
	const FString Url = FString::Printf(TEXT("%s/%dpx"), *ThumbBaseUrl, Pixels);
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(60.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, DestFile](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				OnDone(false);
				return;
			}
			OnDone(FFileHelper::SaveArrayToFile(Response->GetContent(), *DestFile));
		});
	Request->ProcessRequest();
}

// --- Acquire / download -------------------------------------------------------

void FPoliigonApi::PurchaseAsset(int32 AssetId, TFunction<void(bool, FString)> OnDone)
{
	const FString Path = FString::Printf(TEXT("/assets/purchase/%d"), AssetId);
	FHttpRequestRef Request = MakeRequest(ApiUrl + WithFormatSuffix(Path), TEXT("POST"), true);
	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetObjectField(TEXT("meta"), MakeMeta());
	Request->SetContentAsString(JsonToString(Payload));
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;
			if (bConnected && (Code == 200 || Code == 201))
			{
				OnDone(true, FString());
				return;
			}
			FString Message;
			const TSharedPtr<FJsonObject> Body = bConnected ? ParseJson(Response) : nullptr;
			if (Body.IsValid())
			{
				Body->TryGetStringField(TEXT("message"), Message);
				FString ErrCode;
				if (Body->TryGetStringField(TEXT("error_code"), ErrCode) && ErrCode == TEXT("insufficient-credits"))
				{
					Message = TEXT("Not enough credits on your plan for this asset.");
				}
			}
			if (Message.IsEmpty())
			{
				Message = FString::Printf(TEXT("Purchase failed (HTTP %d)."), Code);
			}
			UE_LOG(LogPoliigonApi, Error, TEXT("assets/purchase failed. HTTP=%d Body=%s"), Code,
				Response.IsValid() ? *Response->GetContentAsString().Left(2000) : TEXT("<none>"));
			OnDone(false, Message);
		});
	Request->ProcessRequest();
}

FString FPoliigonApi::ResolveSize(const FPoliigonAssetInfo& Info, const FString& RequestedSize)
{
	if (Info.Resolutions.Num() == 0 || Info.Resolutions.Contains(RequestedSize))
	{
		return RequestedSize;
	}
	static const TCHAR* Ladder[] = { TEXT("1K"), TEXT("2K"), TEXT("3K"), TEXT("4K"), TEXT("6K"), TEXT("8K"), TEXT("16K") };
	const int32 LadderCount = static_cast<int32>(UE_ARRAY_COUNT(Ladder));
	int32 WantIdx = INDEX_NONE;
	for (int32 i = 0; i < LadderCount; ++i)
	{
		if (RequestedSize == Ladder[i]) { WantIdx = i; break; }
	}
	if (WantIdx != INDEX_NONE)
	{
		// Prefer stepping down, then up.
		for (int32 Offset = 0; Offset < LadderCount; ++Offset)
		{
			if (WantIdx - Offset >= 0 && Info.Resolutions.Contains(Ladder[WantIdx - Offset]))
			{
				return Ladder[WantIdx - Offset];
			}
			if (WantIdx + Offset < LadderCount && Info.Resolutions.Contains(Ladder[WantIdx + Offset]))
			{
				return Ladder[WantIdx + Offset];
			}
		}
	}
	return Info.Resolutions[0];
}

void FPoliigonApi::GetDownloadFiles(const FPoliigonAssetInfo& Info, const FString& RequestedSize, bool bIncludeDisplacement,
	TFunction<void(bool, TArray<FPoliigonFileEntry>, FString)> OnDone)
{
	const FString Size = ResolveSize(Info, RequestedSize);

	const TSharedRef<FJsonObject> AssetObj = MakeShared<FJsonObject>();
	AssetObj->SetNumberField(TEXT("id"), Info.AssetId);
	AssetObj->SetStringField(TEXT("name"), Info.AssetName);
	{
		TArray<TSharedPtr<FJsonValue>> Sizes;
		Sizes.Add(MakeShared<FJsonValueString>(Size));
		AssetObj->SetArrayField(TEXT("sizes"), Sizes);
	}

	if (Info.Type == EPoliigonAssetType::Model)
	{
		TArray<TSharedPtr<FJsonValue>> Softwares;
		Softwares.Add(MakeShared<FJsonValueString>(TEXT("Generic")));
		AssetObj->SetArrayField(TEXT("softwares"), Softwares);
	}
	else if (Info.Convention >= 1)
	{
		// convention 1: explicit maps list [{type, format}]
		auto FindMap = [&Info](const FString& Code) -> const FPoliigonMapDesc*
		{
			return Info.Maps.FindByPredicate([&Code](const FPoliigonMapDesc& D) { return D.TypeCode == Code; });
		};
		auto PickFormat = [](const FPoliigonMapDesc& Desc) -> FString
		{
			for (const TCHAR* Fmt : FORMAT_PRIORITY)
			{
				if (Desc.FileFormats.Contains(Fmt))
				{
					return Fmt;
				}
			}
			return Desc.FileFormats.Num() > 0 ? Desc.FileFormats[0] : TEXT("jpg");
		};

		TArray<const FPoliigonMapDesc*> Wanted;
		if (const FPoliigonMapDesc* D = FindMap(TEXT("BaseColor"))) { Wanted.Add(D); }
		else if (const FPoliigonMapDesc* D2 = FindMap(TEXT("BaseColorOpacity"))) { Wanted.Add(D2); }
		if (const FPoliigonMapDesc* D = FindMap(TEXT("Normal"))) { Wanted.Add(D); }
		if (const FPoliigonMapDesc* ORM = FindMap(TEXT("ORM")))
		{
			Wanted.Add(ORM);
		}
		else
		{
			if (const FPoliigonMapDesc* D = FindMap(TEXT("Roughness"))) { Wanted.Add(D); }
			if (const FPoliigonMapDesc* D = FindMap(TEXT("Metallic"))) { Wanted.Add(D); }
			if (const FPoliigonMapDesc* D = FindMap(TEXT("AmbientOcclusion"))) { Wanted.Add(D); }
		}
		if (const FPoliigonMapDesc* D = FindMap(TEXT("Emission"))) { Wanted.Add(D); }
		if (const FPoliigonMapDesc* D = FindMap(TEXT("Opacity"))) { Wanted.Add(D); }
		if (const FPoliigonMapDesc* D = FindMap(TEXT("Transmission"))) { Wanted.Add(D); }
		if (bIncludeDisplacement)
		{
			if (const FPoliigonMapDesc* D = FindMap(TEXT("Displacement"))) { Wanted.Add(D); }
		}

		TArray<TSharedPtr<FJsonValue>> MapList;
		for (const FPoliigonMapDesc* Desc : Wanted)
		{
			const TSharedRef<FJsonObject> MapObj = MakeShared<FJsonObject>();
			MapObj->SetStringField(TEXT("type"), Desc->TypeCode);
			MapObj->SetStringField(TEXT("format"), PickFormat(*Desc));
			MapList.Add(MakeShared<FJsonValueObject>(MapObj));
		}
		AssetObj->SetArrayField(TEXT("maps"), MapList);
	}
	else
	{
		// convention 0: workflow + filtered type codes
		FString Workflow = TEXT("METALNESS");
		const TArray<FString>* Codes = Info.WorkflowMaps.Find(Workflow);
		if (!Codes)
		{
			for (const TPair<FString, TArray<FString>>& Pair : Info.WorkflowMaps)
			{
				Workflow = Pair.Key;
				Codes = &Pair.Value;
				break;
			}
		}
		TArray<FString> Selected;
		if (Codes)
		{
			auto Has = [Codes](const TCHAR* Code) { return Codes->Contains(Code); };
			if (Has(TEXT("COL"))) { Selected.Add(TEXT("COL")); }
			else if (Has(TEXT("DIFF"))) { Selected.Add(TEXT("DIFF")); }
			else if (Has(TEXT("ALBEDO"))) { Selected.Add(TEXT("ALBEDO")); }
			else if (Has(TEXT("ALPHAMASKED"))) { Selected.Add(TEXT("ALPHAMASKED")); }
			if (Has(TEXT("NRM"))) { Selected.Add(TEXT("NRM")); }
			else if (Has(TEXT("NRM16"))) { Selected.Add(TEXT("NRM16")); }
			if (Has(TEXT("ROUGHNESS"))) { Selected.Add(TEXT("ROUGHNESS")); }
			else if (Has(TEXT("GLOSS"))) { Selected.Add(TEXT("GLOSS")); }
			if (Has(TEXT("METALNESS"))) { Selected.Add(TEXT("METALNESS")); }
			if (Has(TEXT("AO"))) { Selected.Add(TEXT("AO")); }
			if (Has(TEXT("EMISSION"))) { Selected.Add(TEXT("EMISSION")); }
			else if (Has(TEXT("EMISSIVE"))) { Selected.Add(TEXT("EMISSIVE")); }
			if (Has(TEXT("MASK"))) { Selected.Add(TEXT("MASK")); }
			if (Has(TEXT("TRANSMISSION"))) { Selected.Add(TEXT("TRANSMISSION")); }
			if (bIncludeDisplacement)
			{
				if (Has(TEXT("DISP"))) { Selected.Add(TEXT("DISP")); }
				else if (Has(TEXT("DISP16"))) { Selected.Add(TEXT("DISP16")); }
			}
		}
		TArray<TSharedPtr<FJsonValue>> Workflows, TypeCodes;
		Workflows.Add(MakeShared<FJsonValueString>(Workflow));
		for (const FString& Code : Selected)
		{
			TypeCodes.Add(MakeShared<FJsonValueString>(Code));
		}
		AssetObj->SetArrayField(TEXT("workflows"), Workflows);
		AssetObj->SetArrayField(TEXT("type_codes"), TypeCodes);
	}

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	{
		TArray<TSharedPtr<FJsonValue>> Assets;
		Assets.Add(MakeShared<FJsonValueObject>(AssetObj));
		Payload->SetArrayField(TEXT("assets"), Assets);
	}
	Payload->SetStringField(TEXT("response_type"), TEXT("json"));
	Payload->SetObjectField(TEXT("meta"), MakeMeta());

	FHttpRequestRef Request = MakeRequest(ApiUrl + WithFormatSuffix(TEXT("/assets/download")), TEXT("POST"), true);
	Request->SetContentAsString(JsonToString(Payload));
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			TArray<FPoliigonFileEntry> Files;
			const TSharedPtr<FJsonObject> Body = bConnected ? ParseJson(Response) : nullptr;
			if (!Body.IsValid())
			{
				OnDone(false, MoveTemp(Files),
					FString::Printf(TEXT("Download request failed (HTTP %d)."),
						Response.IsValid() ? Response->GetResponseCode() : 0));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* PayloadArray = nullptr;
			if (Body->TryGetArrayField(TEXT("payload"), PayloadArray))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *PayloadArray)
				{
					const TSharedPtr<FJsonObject>* EntryObj;
					if (!Entry->TryGetObject(EntryObj))
					{
						continue;
					}
					const TArray<TSharedPtr<FJsonValue>>* FileArray = nullptr;
					if (!(*EntryObj)->TryGetArrayField(TEXT("files"), FileArray))
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& FileValue : *FileArray)
					{
						const TSharedPtr<FJsonObject>* FileObj;
						if (!FileValue->TryGetObject(FileObj))
						{
							continue;
						}
						FPoliigonFileEntry File;
						(*FileObj)->TryGetStringField(TEXT("url"), File.Url);
						(*FileObj)->TryGetStringField(TEXT("name"), File.Name);
						double Bytes = 0;
						(*FileObj)->TryGetNumberField(TEXT("bytes"), Bytes);
						File.Bytes = static_cast<int64>(Bytes);
						if (!File.Url.IsEmpty() && !File.Name.IsEmpty() && !LooksLikePreviewFile(File.Name))
						{
							Files.Add(MoveTemp(File));
						}
					}
					break; // first entry that had a files list, matching the addon
				}
			}
			if (Files.Num() == 0)
			{
				FString Message;
				Body->TryGetStringField(TEXT("message"), Message);
				UE_LOG(LogPoliigonApi, Error, TEXT("assets/download returned no files. HTTP=%d Body=%s"),
					Response.IsValid() ? Response->GetResponseCode() : 0,
					Response.IsValid() ? *Response->GetContentAsString().Left(2000) : TEXT("<none>"));
				OnDone(false, MoveTemp(Files),
					Message.IsEmpty() ? TEXT("No files in download response (asset may not be purchased).") : Message);
				return;
			}
			OnDone(true, MoveTemp(Files), FString());
		});
	Request->ProcessRequest();
}

void FPoliigonApi::DownloadFileToDisk(const FString& Url, const FString& DestPath,
	TFunction<void(uint64)> OnProgress, TFunction<void(bool, FString)> OnDone)
{
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(600.0f);
	if (OnProgress)
	{
		Request->OnRequestProgress64().BindLambda(
			[OnProgress](FHttpRequestPtr, uint64 /*BytesSent*/, uint64 BytesReceived)
			{
				OnProgress(BytesReceived);
			});
	}
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, DestPath](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				OnDone(false, FString::Printf(TEXT("Download failed (HTTP %d): %s"),
					Response.IsValid() ? Response->GetResponseCode() : 0, *FPaths::GetCleanFilename(DestPath)));
				return;
			}
			if (!FFileHelper::SaveArrayToFile(Response->GetContent(), *DestPath))
			{
				OnDone(false, FString::Printf(TEXT("Failed to write %s"), *DestPath));
				return;
			}
			OnDone(true, FString());
		});
	Request->ProcessRequest();
}
