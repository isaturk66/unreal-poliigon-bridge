// GPL v2+ — derived in part from the Poliigon Blender addon (GPL).
#include "Modules/ModuleManager.h"
#include "PoliigonApi.h"
#include "SPoliigonBrowser.h"

#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "PoliigonBridge"

static const FName PoliigonBridgeTabName("PoliigonBridge");

class FPoliigonBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			PoliigonBridgeTabName,
			FOnSpawnTab::CreateRaw(this, &FPoliigonBridgeModule::SpawnBrowserTab))
			.SetDisplayName(LOCTEXT("TabTitle", "Poliigon Bridge"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Browse, download and ingest Poliigon materials and models."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"));
	}

	virtual void ShutdownModule() override
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PoliigonBridgeTabName);
		Api.Reset();
	}

private:
	TSharedRef<SDockTab> SpawnBrowserTab(const FSpawnTabArgs& Args)
	{
		if (!Api.IsValid())
		{
			Api = MakeShared<FPoliigonApi>();
		}
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SPoliigonBrowser).Api(Api)
			];
	}

	TSharedPtr<FPoliigonApi> Api;
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPoliigonBridgeModule, PoliigonBridge)
