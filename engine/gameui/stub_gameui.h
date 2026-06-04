#ifndef ENGINE_GAMEUI_STUB_GAMEUI_H
#define ENGINE_GAMEUI_STUB_GAMEUI_H

#include "interface.h"
#include "GameUI/IGameUI.h"
#include "GameUI/IGameConsole.h"
#include "GameUI/ICareerUI.h"

class CStubGameUI : public IGameUI
{
public:
	void Initialize( CreateInterfaceFn* factories, int count ) override;
	void Start( cl_enginefunc_t* engineFuncs, int interfaceVersion, IBaseSystem* system ) override;
	void Shutdown() override;
	int ActivateGameUI() override;
	int ActivateDemoUI() override;
	int HasExclusiveInput() override;
	void RunFrame() override;
	void ConnectToServer( const char* game, int IP, int port ) override;
	void DisconnectFromServer() override;
	void HideGameUI() override;
	int IsGameUIActive() override;
	void LoadingStarted( const char* resourceType, const char* resourceName ) override;
	void LoadingFinished( const char* resourceType, const char* resourceName ) override;
	void StartProgressBar( const char* progressType, int progressSteps ) override;
	int ContinueProgressBar( int progressPoint, float progressFraction ) override;
	void StopProgressBar( bool bError, const char* failureReason, const char* extendedReason ) override;
	int SetProgressBarStatusText( const char* statusText ) override;
	void SetSecondaryProgressBar( float progress ) override;
	void SetSecondaryProgressBarText( const char* statusText ) override;
	void ValidateCDKey( bool force, bool inConnect ) override;
	void OnDisconnectFromServer( int eSteamLoginFailure, const char* username ) override;
};

class CStubGameConsole : public IGameConsole
{
public:
	void Activate() override;
	void Initialize() override;
	void Hide() override;
	void Clear() override;
	bool IsConsoleVisible() override;
	void Printf( const char* format, ... ) override;
	void DPrintf( const char* format, ... ) override;
	void SetParent( vgui2::VPANEL parent ) override;
};

class CStubCareerUI : public ICareerUI
{
public:
	bool IsPlayingMatch() override;
	ITaskVec* GetCurrentTaskVec() override;
	bool PlayAsCT() override;
	int GetReputationGained() override;
	int GetNumMapsUnlocked() override;
	bool DoesWinUnlockAll() override;
	int GetRoundTimeLength() override;
	int GetWinfastLength() override;
	CareerDifficultyType GetDifficulty() const override;
	int GetCurrentMapTriplet( MapInfo* maps ) override;
	void OnRoundEndMenuOpen( bool didWin ) override;
	void OnMatchEndMenuOpen( bool didWin ) override;
	void OnRoundEndMenuClose( bool stillPlaying ) override;
	void OnMatchEndMenuClose( bool stillPlaying ) override;
};

#endif
