// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
// API contract extracted from poliigon_core/api.py of the official (GPL) Blender addon.
#pragma once

#include "CoreMinimal.h"
#include "PoliigonTypes.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Minimal native client for the Poliigon API v2.
 *
 * Endpoints (base https://apiv2.poliigon.com/api/v2):
 *   POST /initiate/login   {platform, meta}                 -> {login_url, login_token}
 *   POST /validate/login   {platform, login_token, meta}    -> {access_token} once user finished web login
 *   GET  /me                                                 (auth check)
 *   GET  /assets/list/ids                                    -> {asset_ids:[...]} purchased assets
 *   GET  /assets/details/{id}?language_code=en               -> asset dict
 *   POST /assets/purchase/{id}                                (acquire asset on current plan)
 *   POST /assets/download  {assets:[...], response_type:json} -> {payload:[{files:[{url,name,bytes}]}]}
 * Search runs against Poliigon's public Algolia index (search-only key shipped in their addons).
 *
 * All callbacks fire on the game thread (FHttpModule default).
 */
class FPoliigonApi : public TSharedFromThis<FPoliigonApi>
{
public:
	FPoliigonApi();
	~FPoliigonApi();

	// --- Auth -----------------------------------------------------------
	bool HasToken() const { return !Token.IsEmpty(); }
	void LoadToken();
	void ClearToken();

	/**
	 * Verifies the stored token against POST /me.
	 * On success also returns the display name and total spendable credit
	 * balance (subscription + on-demand; 0 for free accounts).
	 */
	void CheckAuth(TFunction<void(bool bValid, FString UserName, int32 CreditsBalance)> OnDone);

	/** Opens the Poliigon website login and polls until validated (or timeout/cancel). */
	void StartWebsiteLogin(TFunction<void(bool bOk, FString Error)> OnDone);
	void CancelLogin();
	bool IsLoginPending() const { return LoginTickerHandle.IsValid(); }

	// --- Browse ---------------------------------------------------------
	/** bFreeOnly filters to zero-credit assets (server-side when the index supports it, with a fallback). */
	void SearchAssets(const FString& Query, int32 PageOneBased, int32 PageSize, bool bFreeOnly,
		TFunction<void(bool bOk, TArray<int32> AssetIds, int32 TotalPages, int32 TotalHits, FString Error)> OnDone);

	void GetAssetDetails(int32 AssetId,
		TFunction<void(bool bOk, FPoliigonAssetInfo Info, FString Error)> OnDone);

	void FetchPurchasedIds(TFunction<void(bool bOk, TSet<int32> Ids)> OnDone);

	void DownloadThumb(const FString& ThumbBaseUrl, int32 Pixels, const FString& DestFile,
		TFunction<void(bool bOk)> OnDone);

	// --- Acquire / download ---------------------------------------------
	void PurchaseAsset(int32 AssetId, TFunction<void(bool bOk, FString Error)> OnDone);

	void GetDownloadFiles(const FPoliigonAssetInfo& Info, const FString& RequestedSize, bool bIncludeDisplacement,
		TFunction<void(bool bOk, TArray<FPoliigonFileEntry> Files, FString Error)> OnDone);

	void DownloadFileToDisk(const FString& Url, const FString& DestPath,
		TFunction<void(uint64 BytesReceived)> OnProgress,
		TFunction<void(bool bOk, FString Error)> OnDone);

	/** Picks the best size available on the asset for the requested size. */
	static FString ResolveSize(const FPoliigonAssetInfo& Info, const FString& RequestedSize);

private:
	FString TokenFilePath() const;
	void SaveToken() const;

	FHttpRequestRef MakeRequest(const FString& Url, const FString& Verb, bool bAuth) const;
	static TSharedPtr<FJsonObject> ParseJson(const FHttpResponsePtr& Response);
	static TSharedPtr<FJsonObject> MakeMeta();
	static FString WithFormatSuffix(const FString& Path);
	static bool ParseAssetInfo(const TSharedPtr<FJsonObject>& Root, FPoliigonAssetInfo& Out);

	void PollValidateLogin();
	void FinishLogin(bool bOk, const FString& Error);
	void SearchAssetsInternal(const FString& Query, int32 PageOneBased, int32 PageSize,
		bool bFreeOnly, bool bUseNumericFilter,
		TFunction<void(bool, TArray<int32>, int32, int32, FString)> OnDone);

	FString ApiUrl = TEXT("https://apiv2.poliigon.com/api/v2");
	FString Token;

	// Website login state
	FString LoginToken;
	double LoginDeadline = 0.0;
	bool bValidateInFlight = false;
	FTSTicker::FDelegateHandle LoginTickerHandle;
	TFunction<void(bool, FString)> LoginCallback;
};
