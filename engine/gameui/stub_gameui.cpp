#include "stub_gameui.h"

InterfaceReg* InterfaceReg::s_pInterfaceRegs = NULL;

InterfaceReg::InterfaceReg(InstantiateInterfaceFn fn, const char* pName) :
	m_pName(pName)
{
	m_CreateFn = fn;
	m_pNext = s_pInterfaceRegs;
	s_pInterfaceRegs = this;
}

EXPORT_FUNCTION IBaseInterface* CreateInterface(const char* pName, int* pReturnCode)
{
	InterfaceReg* pCur;
	for (pCur = InterfaceReg::s_pInterfaceRegs; pCur; pCur = pCur->m_pNext)
	{
		if (strcmp(pCur->m_pName, pName) == 0)
		{
			if (pReturnCode) *pReturnCode = IFACE_OK;
			return pCur->m_CreateFn();
		}
	}
	if (pReturnCode) *pReturnCode = IFACE_FAILED;
	return NULL;
}

static CStubGameUI g_GameUI;
static CStubGameConsole g_GameConsole;
static CStubCareerUI g_CareerUI;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CStubGameUI, IGameUI, GAMEUI_INTERFACE_VERSION, g_GameUI);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CStubGameConsole, IGameConsole, GAMECONSOLE_INTERFACE_VERSION, g_GameConsole);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CStubCareerUI, ICareerUI, CAREERUI_INTERFACE_VERSION, g_CareerUI);

void CStubGameUI::Initialize(CreateInterfaceFn* factories, int count) {}
void CStubGameUI::Start(cl_enginefunc_t* engineFuncs, int interfaceVersion, IBaseSystem* system) {}
void CStubGameUI::Shutdown() {}
int CStubGameUI::ActivateGameUI() { return 0; }
int CStubGameUI::ActivateDemoUI() { return 0; }
int CStubGameUI::HasExclusiveInput() { return 0; }
void CStubGameUI::RunFrame() {}
void CStubGameUI::ConnectToServer(const char* game, int IP, int port) {}
void CStubGameUI::DisconnectFromServer() {}
void CStubGameUI::HideGameUI() {}
int CStubGameUI::IsGameUIActive() { return 1; }
void CStubGameUI::LoadingStarted(const char* resourceType, const char* resourceName) {}
void CStubGameUI::LoadingFinished(const char* resourceType, const char* resourceName) {}
void CStubGameUI::StartProgressBar(const char* progressType, int progressSteps) {}
int CStubGameUI::ContinueProgressBar(int progressPoint, float progressFraction) { return 1; }
void CStubGameUI::StopProgressBar(bool bError, const char* failureReason, const char* extendedReason) {}
int CStubGameUI::SetProgressBarStatusText(const char* statusText) { return 1; }
void CStubGameUI::SetSecondaryProgressBar(float progress) {}
void CStubGameUI::SetSecondaryProgressBarText(const char* statusText) {}
void CStubGameUI::ValidateCDKey(bool force, bool inConnect) {}
void CStubGameUI::OnDisconnectFromServer(int eSteamLoginFailure, const char* username) {}

void CStubGameConsole::Activate() {}
void CStubGameConsole::Initialize() {}
void CStubGameConsole::Hide() {}
void CStubGameConsole::Clear() {}
bool CStubGameConsole::IsConsoleVisible() { return false; }
void CStubGameConsole::Printf(const char* format, ...) { va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); }
void CStubGameConsole::DPrintf(const char* format, ...) { va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); }
void CStubGameConsole::SetParent(vgui2::VPANEL parent) {}

bool CStubCareerUI::IsPlayingMatch() { return false; }
ITaskVec* CStubCareerUI::GetCurrentTaskVec() { return nullptr; }
bool CStubCareerUI::PlayAsCT() { return true; }
int CStubCareerUI::GetReputationGained() { return 0; }
int CStubCareerUI::GetNumMapsUnlocked() { return 0; }
bool CStubCareerUI::DoesWinUnlockAll() { return false; }
int CStubCareerUI::GetRoundTimeLength() { return 0; }
int CStubCareerUI::GetWinfastLength() { return 0; }
CareerDifficultyType CStubCareerUI::GetDifficulty() const { return CAREER_DIFFICULTY_NORMAL; }
int CStubCareerUI::GetCurrentMapTriplet(MapInfo* maps) { return 0; }
void CStubCareerUI::OnRoundEndMenuOpen(bool didWin) {}
void CStubCareerUI::OnMatchEndMenuOpen(bool didWin) {}
void CStubCareerUI::OnRoundEndMenuClose(bool stillPlaying) {}
void CStubCareerUI::OnMatchEndMenuClose(bool stillPlaying) {}
