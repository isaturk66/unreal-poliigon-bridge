// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#include "SPoliigonBrowser.h"
#include "PoliigonApi.h"
#include "PoliigonIngest.h"
#include "PoliigonBridgeSettings.h"

#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SOverlay.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"

#define LOCTEXT_NAMESPACE "PoliigonBridge"

namespace
{
	const int32 THUMB_PIXELS = 300;

	bool DecodeImageToTexture(const FString& FilePath, TStrongObjectPtr<UTexture2D>& OutTexture)
	{
		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
		{
			return false;
		}
		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const EImageFormat Format = ImageWrapperModule.DetectImageFormat(FileData.GetData(), FileData.Num());
		if (Format == EImageFormat::Invalid)
		{
			return false;
		}
		const TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileData.GetData(), FileData.Num()))
		{
			return false;
		}
		TArray<uint8> Raw;
		if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			return false;
		}
		const int32 Width = Wrapper->GetWidth();
		const int32 Height = Wrapper->GetHeight();
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!Texture)
		{
			return false;
		}
		void* MipData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(MipData, Raw.GetData(), Raw.Num());
		Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
		Texture->UpdateResource();
		OutTexture = TStrongObjectPtr<UTexture2D>(Texture);
		return true;
	}
}

void SPoliigonBrowser::Construct(const FArguments& InArgs)
{
	Api = InArgs._Api;

	for (const FString& Size : UPoliigonBridgeSettings::GetResolutionOptions())
	{
		SizeOptions.Add(MakeShared<FString>(Size));
	}
	const FString DefaultSize = GetDefault<UPoliigonBridgeSettings>()->DefaultResolution;
	SelectedSize = SizeOptions[1]; // "2K"
	for (const TSharedPtr<FString>& Option : SizeOptions)
	{
		if (*Option == DefaultSize)
		{
			SelectedSize = Option;
		}
	}

	TypeOptions.Add(MakeShared<FString>(TEXT("All")));
	TypeOptions.Add(MakeShared<FString>(TEXT("Materials")));
	TypeOptions.Add(MakeShared<FString>(TEXT("Models")));
	SelectedType = TypeOptions[0];

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			BuildHeader()
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8, 4)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(TileView, STileView<FPoliigonItemPtr>)
				.ListItemsSource(&VisibleItems)
				.OnGenerateTile(this, &SPoliigonBrowser::OnGenerateTile)
				.ItemWidth(176)
				.ItemHeight(230)
				.SelectionMode(ESelectionMode::None)
				.Visibility_Lambda([this]() { return bAuthValid ? EVisibility::Visible : EVisibility::Collapsed; })
			]
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return bAuthValid ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					BuildLoginPanel()
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 8)
		[
			BuildFooter()
		]
	];

	// Validate any stored token, then load.
	SetStatus(TEXT("Checking sign-in..."));
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	Api->CheckAuth([WeakSelf](bool bValid, FString InUserName, int32 InCredits)
	{
		if (const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin())
		{
			Self->OnAuthChecked(bValid, InUserName, InCredits);
		}
	});
}

SPoliigonBrowser::~SPoliigonBrowser()
{
	if (Api.IsValid())
	{
		Api->CancelLogin();
	}
}

// --- UI ------------------------------------------------------------------------

TSharedRef<SWidget> SPoliigonBrowser::BuildHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Poliigon"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SAssignNew(SearchBox, SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search materials and models (Enter)"))
			.OnTextCommitted(this, &SPoliigonBrowser::OnSearchCommitted)
			.IsEnabled_Lambda([this]() { return bAuthValid; })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&SizeOptions)
			.InitiallySelectedItem(SelectedSize)
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue, ESelectInfo::Type)
			{
				if (NewValue.IsValid()) { SelectedSize = NewValue; }
			})
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
			{
				return SNew(STextBlock).Text(FText::FromString(*Option));
			})
			[
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(SelectedSize.IsValid() ? *SelectedSize : TEXT("2K"));
				})
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&TypeOptions)
			.InitiallySelectedItem(SelectedType)
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue, ESelectInfo::Type)
			{
				if (NewValue.IsValid())
				{
					SelectedType = NewValue;
					RebuildVisibleItems();
				}
			})
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option)
			{
				return SNew(STextBlock).Text(FText::FromString(*Option));
			})
			[
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(SelectedType.IsValid() ? *SelectedType : TEXT("All"));
				})
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SCheckBox)
			.IsEnabled_Lambda([this]() { return bAuthValid; })
			.IsChecked_Lambda([this]() { return bFreeOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
			{
				bFreeOnly = (NewState == ECheckBoxState::Checked);
				RunSearch(1);
			})
			[
				SNew(STextBlock).Text(LOCTEXT("FreeOnly", "Free only"))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				if (!bAuthValid)
				{
					return FText::GetEmpty();
				}
				const FString Who = UserName.IsEmpty() ? TEXT("Signed in") : UserName;
				return FText::FromString(FString::Printf(TEXT("%s  |  %d credits"), *Who, CreditsBalance));
			})
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.Text_Lambda([this]() { return bAuthValid ? LOCTEXT("SignOut", "Sign out") : LOCTEXT("SignIn", "Sign in"); })
			.OnClicked_Lambda([this]()
			{
				if (bAuthValid) { Logout(); }
				else { StartLogin(); }
				return FReply::Handled();
			})
		];
}

TSharedRef<SWidget> SPoliigonBrowser::BuildLoginPanel()
{
	return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 8)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("LoginTitle", "Sign in to browse and download Poliigon assets."))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(SButton)
			.IsEnabled_Lambda([this]() { return !bLoginInProgress; })
			.Text_Lambda([this]()
			{
				return bLoginInProgress
					? LOCTEXT("LoginWait", "Waiting for browser sign-in...")
					: LOCTEXT("LoginButton", "Sign in with Poliigon");
			})
			.OnClicked_Lambda([this]() { StartLogin(); return FReply::Handled(); })
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 8, 0, 0)
		[
			SNew(SButton)
			.Visibility_Lambda([this]() { return bLoginInProgress ? EVisibility::Visible : EVisibility::Collapsed; })
			.Text(LOCTEXT("LoginCancel", "Cancel"))
			.OnClicked_Lambda([this]()
			{
				Api->CancelLogin();
				bLoginInProgress = false;
				return FReply::Handled();
			})
		]
	];
}

TSharedRef<SWidget> SPoliigonBrowser::BuildFooter()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("PrevPage", "<"))
			.IsEnabled_Lambda([this]() { return bAuthValid && CurrentPage > 1; })
			.OnClicked_Lambda([this]() { RunSearch(CurrentPage - 1); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0)
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("Page %d / %d  (%d results)"),
					CurrentPage, FMath::Max(TotalPages, 1), TotalHits));
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("NextPage", ">"))
			.IsEnabled_Lambda([this]() { return bAuthValid && CurrentPage < TotalPages; })
			.OnClicked_Lambda([this]() { RunSearch(CurrentPage + 1); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return FText::FromString(Status); })
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

TSharedRef<ITableRow> SPoliigonBrowser::OnGenerateTile(FPoliigonItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const auto StateText = [Item]() -> FText
	{
		switch (Item->State)
		{
		case ETileState::Downloading:
			return FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Item->Progress * 100.0f)));
		case ETileState::Ingesting:
			return LOCTEXT("TileImporting", "Importing...");
		case ETileState::Done:
			return LOCTEXT("TileDone", "Imported");
		case ETileState::Failed:
			return LOCTEXT("TileFailed", "Failed");
		default:
			break;
		}
		const TCHAR* TypeName = Item->Info.Type == EPoliigonAssetType::Model ? TEXT("Model") : TEXT("Material");
		if (Item->bOwned)
		{
			return FText::FromString(TypeName);
		}
		if (Item->Info.Credits == 0)
		{
			return FText::FromString(FString::Printf(TEXT("%s - FREE"), TypeName));
		}
		return FText::FromString(FString::Printf(TEXT("%s - %d cr"), TypeName, Item->Info.Credits));
	};

	return SNew(STableRow<FPoliigonItemPtr>, OwnerTable)
	.Padding(4)
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(4)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).WidthOverride(160).HeightOverride(160)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image_Lambda([Item]() -> const FSlateBrush*
						{
							return Item->ThumbBrush.IsValid()
								? Item->ThumbBrush.Get()
								: FAppStyle::GetBrush("Brushes.Recessed");
						})
					]
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(2)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Owned", "OWNED"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FLinearColor(0.2f, 0.9f, 0.3f))
						.Visibility_Lambda([Item]()
						{
							return Item->bOwned ? EVisibility::Visible : EVisibility::Collapsed;
						})
					]
					+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Bottom)
					[
						SNew(SProgressBar)
						.Percent_Lambda([Item]() { return TOptional<float>(Item->Progress); })
						.Visibility_Lambda([Item]()
						{
							return Item->State == ETileState::Downloading || Item->State == ETileState::Ingesting
								? EVisibility::Visible : EVisibility::Collapsed;
						})
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(STextBlock)
				.Text_Lambda([Item]() { return FText::FromString(Item->Info.DisplayName); })
				.ToolTipText_Lambda([Item]()
				{
					return FText::FromString(Item->Message.IsEmpty() ? Item->Info.AssetName : Item->Message);
				})
				.WrapTextAt(155.0f)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda(StateText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity_Lambda([Item]()
					{
						switch (Item->State)
						{
						case ETileState::Done: return FSlateColor(FLinearColor(0.2f, 0.9f, 0.3f));
						case ETileState::Failed: return FSlateColor(FLinearColor(0.9f, 0.25f, 0.2f));
						default:
							if (!Item->bOwned && Item->Info.Credits == 0)
							{
								return FSlateColor(FLinearColor(0.2f, 0.9f, 0.3f)); // FREE in green
							}
							return FSlateColor::UseSubduedForeground();
						}
					})
					.ToolTipText_Lambda([Item]() { return FText::FromString(Item->Message); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text_Lambda([Item]()
					{
						return Item->State == ETileState::Done ? LOCTEXT("Reimport", "Redo") : LOCTEXT("Get", "Get");
					})
					.IsEnabled_Lambda([Item]()
					{
						return Item->State == ETileState::Idle
							|| Item->State == ETileState::Failed
							|| Item->State == ETileState::Done;
					})
					.OnClicked_Lambda([this, Item]()
					{
						StartDownload(Item);
						return FReply::Handled();
					})
				]
			]
		]
	];
}

// --- Auth / search flow ----------------------------------------------------------

void SPoliigonBrowser::OnAuthChecked(bool bValid, const FString& InUserName, int32 InCredits)
{
	bAuthChecked = true;
	bAuthValid = bValid;
	if (bValid)
	{
		UserName = InUserName;
		CreditsBalance = InCredits;
		SetStatus(TEXT("Signed in."));
		RefreshPurchased();
		RunSearch(1);
	}
	else
	{
		SetStatus(TEXT("Not signed in."));
	}
}

void SPoliigonBrowser::RefreshUserInfo()
{
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	Api->CheckAuth([WeakSelf](bool bValid, FString InUserName, int32 InCredits)
	{
		const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
		if (Self.IsValid() && bValid)
		{
			Self->UserName = InUserName;
			Self->CreditsBalance = InCredits;
		}
	});
}

void SPoliigonBrowser::StartLogin()
{
	if (bLoginInProgress)
	{
		return;
	}
	bLoginInProgress = true;
	SetStatus(TEXT("Complete the sign-in in your web browser..."));
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	Api->StartWebsiteLogin([WeakSelf](bool bOk, FString Error)
	{
		if (const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin())
		{
			Self->bLoginInProgress = false;
			if (bOk)
			{
				// Pull name + credit balance, then unlock the UI.
				Self->Api->CheckAuth([WeakSelf](bool bValid, FString InUserName, int32 InCredits)
				{
					if (const TSharedPtr<SPoliigonBrowser> InnerSelf = WeakSelf.Pin())
					{
						InnerSelf->OnAuthChecked(bValid, InUserName, InCredits);
					}
				});
			}
			else
			{
				Self->SetStatus(FString::Printf(TEXT("Sign-in failed: %s"), *Error));
			}
		}
	});
}

void SPoliigonBrowser::Logout()
{
	Api->ClearToken();
	bAuthValid = false;
	UserName.Empty();
	CreditsBalance = 0;
	AllItems.Empty();
	VisibleItems.Empty();
	if (TileView.IsValid())
	{
		TileView->RequestListRefresh();
	}
	SetStatus(TEXT("Signed out."));
}

void SPoliigonBrowser::RefreshPurchased()
{
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	Api->FetchPurchasedIds([WeakSelf](bool bOk, TSet<int32> Ids)
	{
		if (const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin())
		{
			if (bOk)
			{
				Self->PurchasedIds = MoveTemp(Ids);
				for (const FPoliigonItemPtr& Item : Self->AllItems)
				{
					Item->bOwned = Self->PurchasedIds.Contains(Item->AssetId);
				}
			}
		}
	});
}

void SPoliigonBrowser::OnSearchCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		CurrentQuery = Text.ToString();
		RunSearch(1);
	}
}

void SPoliigonBrowser::RunSearch(int32 InPage)
{
	if (!bAuthValid)
	{
		return;
	}
	CurrentPage = FMath::Max(InPage, 1);
	SetStatus(TEXT("Searching..."));
	AllItems.Empty();
	VisibleItems.Empty();
	if (TileView.IsValid())
	{
		TileView->RequestListRefresh();
	}

	const int32 PageSize = GetDefault<UPoliigonBridgeSettings>()->PageSize;
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	const int32 RequestPage = CurrentPage;
	Api->SearchAssets(CurrentQuery, CurrentPage, PageSize, bFreeOnly,
		[WeakSelf, RequestPage](bool bOk, TArray<int32> Ids, int32 InTotalPages, int32 InTotalHits, FString Error)
		{
			const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Self->CurrentPage != RequestPage)
			{
				return;
			}
			if (!bOk)
			{
				Self->SetStatus(FString::Printf(TEXT("Search failed: %s"), *Error));
				return;
			}
			Self->TotalPages = InTotalPages;
			Self->TotalHits = InTotalHits;
			Self->SetStatus(FString::Printf(TEXT("Loading %d assets..."), Ids.Num()));
			Self->PendingDetailRequests = Ids.Num();
			for (const int32 Id : Ids)
			{
				FPoliigonItemPtr Item = MakeShared<FPoliigonBrowserItem>();
				Item->AssetId = Id;
				Item->bOwned = Self->PurchasedIds.Contains(Id);
				Self->AllItems.Add(Item);

				Self->Api->GetAssetDetails(Id, [WeakSelf, Item, RequestPage](bool bDetailsOk, FPoliigonAssetInfo Info, FString)
				{
					const TSharedPtr<SPoliigonBrowser> InnerSelf = WeakSelf.Pin();
					if (!InnerSelf.IsValid() || InnerSelf->CurrentPage != RequestPage)
					{
						return;
					}
					InnerSelf->PendingDetailRequests--;
					if (bDetailsOk)
					{
						Item->Info = MoveTemp(Info);
						InnerSelf->OnDetailsArrived(Item);
					}
					if (InnerSelf->PendingDetailRequests <= 0)
					{
						InnerSelf->SetStatus(FString::Printf(TEXT("%d results."), InnerSelf->TotalHits));
					}
				});
			}
		});
}

bool SPoliigonBrowser::PassesTypeFilter(const FPoliigonAssetInfo& Info) const
{
	if (Info.Type != EPoliigonAssetType::Texture && Info.Type != EPoliigonAssetType::Model)
	{
		return false; // HDRIs/brushes unsupported in v1
	}
	if (bFreeOnly && Info.Credits > 0)
	{
		return false; // client-side guard in case the server-side free filter was unavailable
	}
	if (!SelectedType.IsValid() || *SelectedType == TEXT("All"))
	{
		return true;
	}
	if (*SelectedType == TEXT("Materials"))
	{
		return Info.Type == EPoliigonAssetType::Texture;
	}
	return Info.Type == EPoliigonAssetType::Model;
}

void SPoliigonBrowser::OnDetailsArrived(const FPoliigonItemPtr& Item)
{
	if (PassesTypeFilter(Item->Info))
	{
		RequestThumb(Item);
	}
	RebuildVisibleItems();
}

void SPoliigonBrowser::RebuildVisibleItems()
{
	VisibleItems.Empty();
	for (const FPoliigonItemPtr& Item : AllItems)
	{
		if (Item->Info.bDetailsLoaded && PassesTypeFilter(Item->Info))
		{
			VisibleItems.Add(Item);
		}
	}
	if (TileView.IsValid())
	{
		TileView->RequestListRefresh();
	}
}

// --- Thumbnails --------------------------------------------------------------------

FString SPoliigonBrowser::ThumbCacheFile(int32 AssetId) const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PoliigonBridge"), TEXT("Thumbs"),
		FString::Printf(TEXT("%d.img"), AssetId));
}

void SPoliigonBrowser::RequestThumb(const FPoliigonItemPtr& Item)
{
	if (Item->ThumbBrush.IsValid())
	{
		return;
	}
	const FString CacheFile = ThumbCacheFile(Item->AssetId);
	if (IFileManager::Get().FileExists(*CacheFile))
	{
		ApplyThumbFromDisk(Item, CacheFile);
		return;
	}
	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	Api->DownloadThumb(Item->Info.ThumbBaseUrl, THUMB_PIXELS, CacheFile,
		[WeakSelf, Item, CacheFile](bool bOk)
		{
			const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
			if (Self.IsValid() && bOk)
			{
				Self->ApplyThumbFromDisk(Item, CacheFile);
			}
		});
}

void SPoliigonBrowser::ApplyThumbFromDisk(const FPoliigonItemPtr& Item, const FString& FilePath)
{
	TStrongObjectPtr<UTexture2D> Texture;
	if (!DecodeImageToTexture(FilePath, Texture))
	{
		return;
	}
	Item->ThumbTexture = Texture;
	TSharedPtr<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Texture.Get());
	Brush->ImageSize = FVector2D(160.0f, 160.0f);
	Brush->DrawAs = ESlateBrushDrawType::Image;
	Item->ThumbBrush = Brush;
}

// --- Download & ingest ---------------------------------------------------------------

FString SPoliigonBrowser::StagingDirFor(const FString& AssetName) const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PoliigonBridge"), TEXT("Staging"), AssetName);
}

void SPoliigonBrowser::StartDownload(FPoliigonItemPtr Item)
{
	if (Item->State == ETileState::Downloading || Item->State == ETileState::Ingesting)
	{
		return;
	}
	Item->State = ETileState::Downloading;
	Item->Progress = 0.0f;
	Item->Message.Empty();

	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	auto ProceedToDownload = [WeakSelf, Item]()
	{
		const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		const FString Size = Self->SelectedSize.IsValid() ? *Self->SelectedSize : TEXT("2K");
		const bool bDisp = GetDefault<UPoliigonBridgeSettings>()->bDownloadDisplacement;
		Self->Api->GetDownloadFiles(Item->Info, Size, bDisp,
			[WeakSelf, Item](bool bOk, TArray<FPoliigonFileEntry> Files, FString Error)
			{
				const TSharedPtr<SPoliigonBrowser> InnerSelf = WeakSelf.Pin();
				if (!InnerSelf.IsValid())
				{
					return;
				}
				if (!bOk)
				{
					Item->State = ETileState::Failed;
					Item->Message = Error;
					InnerSelf->SetStatus(Error);
					return;
				}
				int64 TotalBytes = 0;
				for (const FPoliigonFileEntry& File : Files)
				{
					TotalBytes += FMath::Max<int64>(File.Bytes, 0);
				}
				const TSharedRef<FString> StagingDir =
					MakeShared<FString>(InnerSelf->StagingDirFor(Item->Info.AssetName));
				IFileManager::Get().MakeDirectory(**StagingDir, true);
				InnerSelf->DownloadNextFile(Item, MakeShared<TArray<FPoliigonFileEntry>>(MoveTemp(Files)),
					0, FMath::Max<int64>(TotalBytes, 1), 0, StagingDir);
			});
	};

	if (Item->bOwned)
	{
		ProceedToDownload();
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Acquiring %s..."), *Item->Info.DisplayName));
		Api->PurchaseAsset(Item->AssetId, [WeakSelf, Item, ProceedToDownload](bool bOk, FString Error)
		{
			const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!bOk)
			{
				// The owned-set can be stale; try the download anyway and let it fail with a clear message.
				Self->SetStatus(FString::Printf(TEXT("Purchase warning: %s"), *Error));
			}
			else
			{
				Item->bOwned = true;
				Self->PurchasedIds.Add(Item->AssetId);
				Self->RefreshUserInfo(); // credit balance changed
			}
			ProceedToDownload();
		});
	}
}

void SPoliigonBrowser::DownloadNextFile(FPoliigonItemPtr Item, TSharedRef<TArray<FPoliigonFileEntry>> Files,
	int32 Index, int64 TotalBytes, int64 DoneBytes, TSharedRef<FString> StagingDir)
{
	if (Index >= Files->Num())
	{
		FinishDownload(Item, *StagingDir);
		return;
	}
	const FPoliigonFileEntry& File = (*Files)[Index];
	const FString DestPath = FPaths::Combine(*StagingDir, File.Name);
	SetStatus(FString::Printf(TEXT("Downloading %s (%d/%d)..."), *File.Name, Index + 1, Files->Num()));

	TWeakPtr<SPoliigonBrowser> WeakSelf = SharedThis(this);
	const int64 FileBytes = File.Bytes;
	Api->DownloadFileToDisk(File.Url, DestPath,
		[Item, TotalBytes, DoneBytes](uint64 Received)
		{
			Item->Progress = FMath::Clamp(
				static_cast<float>(DoneBytes + static_cast<int64>(Received)) / static_cast<float>(TotalBytes),
				0.0f, 1.0f);
		},
		[WeakSelf, Item, Files, Index, TotalBytes, DoneBytes, FileBytes, StagingDir](bool bOk, FString Error)
		{
			const TSharedPtr<SPoliigonBrowser> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!bOk)
			{
				Item->State = ETileState::Failed;
				Item->Message = Error;
				Self->SetStatus(Error);
				return;
			}
			Self->DownloadNextFile(Item, Files, Index + 1, TotalBytes, DoneBytes + FileBytes, StagingDir);
		});
}

void SPoliigonBrowser::FinishDownload(FPoliigonItemPtr Item, const FString& StagingDir)
{
	Item->State = ETileState::Ingesting;
	Item->Progress = 1.0f;
	SetStatus(FString::Printf(TEXT("Importing %s..."), *Item->Info.DisplayName));

	const FPoliigonIngest::FResult Result = FPoliigonIngest::IngestAsset(Item->Info, StagingDir);
	if (!Result.bOk)
	{
		Item->State = ETileState::Failed;
		Item->Message = Result.Error;
		SetStatus(Result.Error);
		return;
	}

	Item->State = ETileState::Done;
	SetStatus(FString::Printf(TEXT("%s imported (%d assets)."), *Item->Info.DisplayName, Result.CreatedAssetPaths.Num()));

	if (!GetDefault<UPoliigonBridgeSettings>()->bKeepStagingFiles)
	{
		IFileManager::Get().DeleteDirectory(*StagingDir, false, true);
	}

	// Highlight the created assets in the content browser.
	if (Result.CreatedAssetPaths.Num() > 0)
	{
		const FAssetRegistryModule& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Assets;
		for (const FString& Path : Result.CreatedAssetPaths)
		{
			const FAssetData Data = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(Path));
			if (Data.IsValid())
			{
				Assets.Add(Data);
			}
		}
		if (Assets.Num() > 0)
		{
			FContentBrowserModule& ContentBrowser =
				FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			ContentBrowser.Get().SyncBrowserToAssets(Assets);
		}
	}
}

void SPoliigonBrowser::SetStatus(const FString& InStatus)
{
	Status = InStatus;
}

#undef LOCTEXT_NAMESPACE
