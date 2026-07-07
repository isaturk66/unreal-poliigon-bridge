// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#pragma once

#include "CoreMinimal.h"
#include "PoliigonTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STileView.h"
#include "UObject/StrongObjectPtr.h"

class FPoliigonApi;
class UTexture2D;
class SSearchBox;

enum class ETileState : uint8
{
	Idle,
	Downloading,
	Ingesting,
	Done,
	Failed
};

/** One tile in the browser grid. */
struct FPoliigonBrowserItem
{
	int32 AssetId = 0;
	FPoliigonAssetInfo Info;
	ETileState State = ETileState::Idle;
	float Progress = 0.0f;
	FString Message;
	bool bOwned = false;

	TSharedPtr<FSlateBrush> ThumbBrush;
	TStrongObjectPtr<UTexture2D> ThumbTexture;
};

using FPoliigonItemPtr = TSharedPtr<FPoliigonBrowserItem>;

/** The Poliigon Bridge tab: search, grid, one-click download & ingest. */
class SPoliigonBrowser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPoliigonBrowser) {}
		SLATE_ARGUMENT(TSharedPtr<FPoliigonApi>, Api)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SPoliigonBrowser() override;

private:
	// UI construction
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildLoginPanel();
	TSharedRef<SWidget> BuildFooter();
	TSharedRef<ITableRow> OnGenerateTile(FPoliigonItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);

	// State/flow
	void OnAuthChecked(bool bValid, const FString& UserName, int32 CreditsBalance);
	void RefreshUserInfo();
	void StartLogin();
	void Logout();
	void RefreshPurchased();
	void OnSearchCommitted(const FText& Text, ETextCommit::Type CommitType);
	void RunSearch(int32 InPage);
	void OnDetailsArrived(const FPoliigonItemPtr& Item);
	void RebuildVisibleItems();
	void RequestThumb(const FPoliigonItemPtr& Item);
	void ApplyThumbFromDisk(const FPoliigonItemPtr& Item, const FString& FilePath);

	void StartDownload(FPoliigonItemPtr Item);
	void DownloadNextFile(FPoliigonItemPtr Item, TSharedRef<TArray<FPoliigonFileEntry>> Files,
		int32 Index, int64 TotalBytes, int64 DoneBytes, TSharedRef<FString> StagingDir);
	void FinishDownload(FPoliigonItemPtr Item, const FString& StagingDir);

	void SetStatus(const FString& InStatus);
	FString ThumbCacheFile(int32 AssetId) const;
	FString StagingDirFor(const FString& AssetName) const;
	bool PassesTypeFilter(const FPoliigonAssetInfo& Info) const;

	TSharedPtr<FPoliigonApi> Api;

	// All items for the current page (details may still be loading) and the filtered view.
	TArray<FPoliigonItemPtr> AllItems;
	TArray<FPoliigonItemPtr> VisibleItems;
	TSharedPtr<STileView<FPoliigonItemPtr>> TileView;
	TSharedPtr<SSearchBox> SearchBox;

	TSet<int32> PurchasedIds;
	FString CurrentQuery;
	int32 CurrentPage = 1;
	int32 TotalPages = 0;
	int32 TotalHits = 0;

	// Toolbar state
	TArray<TSharedPtr<FString>> SizeOptions;
	TSharedPtr<FString> SelectedSize;
	TArray<TSharedPtr<FString>> TypeOptions; // All / Materials / Models
	TSharedPtr<FString> SelectedType;

	bool bAuthValid = false;
	bool bAuthChecked = false;
	bool bLoginInProgress = false;
	bool bFreeOnly = false;
	FString UserName;
	int32 CreditsBalance = 0;
	FString Status;
	int32 PendingDetailRequests = 0;
};
