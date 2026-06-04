#ifndef ENGINE_GAMEUI_STUB_GAMEUI_H
#define ENGINE_GAMEUI_STUB_GAMEUI_H

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

// ------------------------------------------------------------------
// Self-contained definitions (no external dependencies)
// ------------------------------------------------------------------

#define EXPORT_FUNCTION __attribute__((visibility("default")))

class IBaseInterface
{
public:
	virtual ~IBaseInterface() {}
};

typedef IBaseInterface* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
typedef IBaseInterface* (*InstantiateInterfaceFn)();

class InterfaceReg
{
public:
	InterfaceReg(InstantiateInterfaceFn fn, const char* pName)
		: m_pName(pName)
	{
		m_CreateFn = fn;
		m_pNext = s_pInterfaceRegs;
		s_pInterfaceRegs = this;
	}
	InstantiateInterfaceFn m_CreateFn;
	const char* m_pName;
	InterfaceReg* m_pNext;
	static InterfaceReg* s_pInterfaceRegs;
};

#define EXPOSE_SINGLE_INTERFACE_GLOBALVAR(className, interfaceName, versionName, globalVarName) \
	static IBaseInterface* __Create##className##interfaceName##_interface() { return (IBaseInterface*)&globalVarName; } \
	static InterfaceReg __g_Create##className##interfaceName##_reg(__Create##className##interfaceName##_interface, versionName);

enum
{
	IFACE_OK = 0,
	IFACE_FAILED
};

// ------------------------------------------------------------------
// Minimal vgui2 types
// ------------------------------------------------------------------
namespace vgui2
{
	typedef unsigned int VPANEL;
}

// ------------------------------------------------------------------
// GameUI interface definitions
// ------------------------------------------------------------------

class IBaseSystem;
struct cl_enginefuncs_s;
typedef struct cl_enginefuncs_s cl_enginefunc_t;

enum ESteamLoginFailure
{
	STEAMLOGINFAILURE_NONE,
	STEAMLOGINFAILURE_BADTICKET,
	STEAMLOGINFAILURE_NOSTEAMLOGIN,
	STEAMLOGINFAILURE_VACBANNED,
	STEAMLOGINFAILURE_LOGGED_IN_ELSEWHERE,
	STEAMLOGINFAILURE_CONNECTIONLOST,
	STEAMLOGINFAILURE_NOCONNECTION
};

class IGameUI : public IBaseInterface
{
public:
	virtual void Initialize(CreateInterfaceFn* factories, int count) = 0;
	virtual void Start(cl_enginefunc_t* engineFuncs, int interfaceVersion, IBaseSystem* system) = 0;
	virtual void Shutdown() = 0;
	virtual int ActivateGameUI() = 0;
	virtual int ActivateDemoUI() = 0;
	virtual int HasExclusiveInput() = 0;
	virtual void RunFrame() = 0;
	virtual void ConnectToServer(const char* game, int IP, int port) = 0;
	virtual void DisconnectFromServer() = 0;
	virtual void HideGameUI() = 0;
	virtual int IsGameUIActive() = 0;
	virtual void LoadingStarted(const char* resourceType, const char* resourceName) = 0;
	virtual void LoadingFinished(const char* resourceType, const char* resourceName) = 0;
	virtual void StartProgressBar(const char* progressType, int progressSteps) = 0;
	virtual int ContinueProgressBar(int progressPoint, float progressFraction) = 0;
	virtual void StopProgressBar(bool bError, const char* failureReason, const char* extendedReason) = 0;
	virtual int SetProgressBarStatusText(const char* statusText) = 0;
	virtual void SetSecondaryProgressBar(float progress) = 0;
	virtual void SetSecondaryProgressBarText(const char* statusText) = 0;
	virtual void ValidateCDKey(bool force, bool inConnect) = 0;
	virtual void OnDisconnectFromServer(int eSteamLoginFailure, const char* username) = 0;
};

#define GAMEUI_INTERFACE_VERSION "GameUI007"

class IGameConsole : public IBaseInterface
{
public:
	virtual void Activate() = 0;
	virtual void Initialize() = 0;
	virtual void Hide() = 0;
	virtual void Clear() = 0;
	virtual bool IsConsoleVisible() = 0;
	virtual void Printf(const char* format, ...) = 0;
	virtual void DPrintf(const char* format, ...) = 0;
	virtual void SetParent(vgui2::VPANEL parent) = 0;
};

#define GAMECONSOLE_INTERFACE_VERSION "GameConsole003"

enum CareerStateType
{
	CAREER_NONE = 0,
	CAREER_LOADING,
	CAREER_PLAYING
};

enum CareerDifficultyType
{
	CAREER_DIFFICULTY_EASY,
	CAREER_DIFFICULTY_NORMAL,
	CAREER_DIFFICULTY_HARD,
	CAREER_DIFFICULTY_EXPERT,
	MAX_CAREER_DIFFICULTY
};

struct MapInfo
{
	const char* name;
	bool defeated;
};

class ICareerTask
{
public:
	virtual bool IsComplete() = 0;
	virtual bool IsCompletedThisRound() = 0;
	virtual bool IsCompletedThisMatch() = 0;
	virtual const char* GetMap() = 0;
	virtual const char* GetTaskName() = 0;
	virtual const wchar_t* GetLocalizedTaskName() = 0;
	virtual const wchar_t* GetLocalizedTaskNameWithCompletion() = 0;
	virtual void Reset() = 0;
	virtual void Set(bool thisRound) = 0;
	virtual void StartRound() = 0;
	virtual const char* GetWeaponName() const = 0;
	virtual int GetRepeat() const = 0;
	virtual bool MustSurvive() const = 0;
	virtual bool InARow() const = 0;
	virtual int GetPartial() const = 0;
	virtual void SetPartial(int num) = 0;
	virtual bool IsMaxRoundTime() const = 0;
};

typedef std::vector<ICareerTask*> ITaskVec;

class ICareerUI : public IBaseInterface
{
public:
	virtual bool IsPlayingMatch() = 0;
	virtual ITaskVec* GetCurrentTaskVec() = 0;
	virtual bool PlayAsCT() = 0;
	virtual int GetReputationGained() = 0;
	virtual int GetNumMapsUnlocked() = 0;
	virtual bool DoesWinUnlockAll() = 0;
	virtual int GetRoundTimeLength() = 0;
	virtual int GetWinfastLength() = 0;
	virtual CareerDifficultyType GetDifficulty() const = 0;
	virtual int GetCurrentMapTriplet(MapInfo* maps) = 0;
	virtual void OnRoundEndMenuOpen(bool didWin) = 0;
	virtual void OnMatchEndMenuOpen(bool didWin) = 0;
	virtual void OnRoundEndMenuClose(bool stillPlaying) = 0;
	virtual void OnMatchEndMenuClose(bool stillPlaying) = 0;
};

#define CAREERUI_INTERFACE_VERSION "CareerUI001"

// ------------------------------------------------------------------
// Stub classes
// ------------------------------------------------------------------

extern "C" {
	EXPORT_FUNCTION IBaseInterface* CreateInterface(const char* pName, int* pReturnCode);
}

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
