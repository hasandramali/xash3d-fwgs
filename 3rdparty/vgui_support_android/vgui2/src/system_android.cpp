#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <sys/statvfs.h>
#include <unistd.h>

#include "FileSystem.h"
#include "FileSystem_Helpers.h"

#include <vgui/IInputInternal.h>
#include <vgui_controls/Controls.h>

#include "KeyValues.h"

#include "system_android.h"
#include "vgui.h"
#include "vgui_key_translation.h"

using vgui2::ISystem;

EXPOSE_SINGLE_INTERFACE( CSystem, ISystem, VGUI_SYSTEM_INTERFACE_VERSION_GS );

CSystem::CSystem()
{
	m_flCurTime = GetCurrentTime();
}

CSystem::~CSystem()
{
}

void CSystem::Shutdown()
{
	if( m_pUserConfigData )
		m_pUserConfigData->deleteThis();
}

void CSystem::RunFrame()
{
	const auto time = GetCurrentTime();
	m_flFrameTime = time;

	if( m_bStaticWatchForComputerUse )
	{
		int x, y;
		vgui2::input()->GetCursorPos( x, y );

		const auto deltaX = m_iStaticMouseOldX - x;
		const auto deltaY = m_iStaticMouseOldY - y;

		const auto iDistance = sqrt( deltaX * deltaX + deltaY * deltaY );

		if( iDistance > 50 )
		{
			m_iStaticMouseOldX = x;
			m_iStaticMouseOldY = y;
			m_StaticLastComputerUseTime = GetTimeMillis() * 0.001;
		}
	}
}

void CSystem::ShellExecute( const char *command, const char *file )
{
}

double CSystem::GetFrameTime()
{
	return m_flFrameTime;
}

double CSystem::GetCurrentTime()
{
	static int sametimecount;
	static uint64_t oldtime;
	static bool first = true;

	timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );

	uint64_t temp = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

	if( first )
	{
		oldtime = temp;
		first = false;
	}

	if( temp <= oldtime && ( oldtime - temp ) < 0x10000000 )
	{
		oldtime = temp;
	}
	else
	{
		uint64_t t2 = temp - oldtime;
		double time = (double)t2 / 1000000000.0;
		oldtime = temp;

		m_flCurTime += time;

		if( m_flCurTime == m_flLastCurTime )
		{
			sametimecount++;
			if( sametimecount > 100000 )
			{
				m_flCurTime += 1.0;
				sametimecount = 0;
			}
		}
		else
		{
			sametimecount = 0;
		}

		m_flLastCurTime = m_flCurTime;
	}

	return m_flCurTime;
}

long CSystem::GetTimeMillis()
{
	return (long)( GetCurrentTime() * 1000.0 );
}

int CSystem::GetClipboardTextCount()
{
	return 0;
}

void CSystem::SetClipboardText( const char *text, int textLen )
{
}

void CSystem::SetClipboardText( const wchar_t *text, int textLen )
{
}

int CSystem::GetClipboardText( int offset, char *buf, int bufLen )
{
	if( buf && bufLen > 0 )
		buf[ 0 ] = '\0';
	return 0;
}

int CSystem::GetClipboardText( int offset, wchar_t *buf, int bufLen )
{
	if( buf && bufLen > 0 )
		buf[ 0 ] = L'\0';
	return 0;
}

bool CSystem::SetRegistryString( const char *key, const char *value )
{
	return false;
}

bool CSystem::GetRegistryString( const char *key, char *value, int valueLen )
{
	if( value && valueLen > 0 )
		value[ 0 ] = '\0';
	return false;
}

bool CSystem::SetRegistryInteger( const char *key, int value )
{
	return false;
}

bool CSystem::GetRegistryInteger( const char *key, int &value )
{
	return false;
}

KeyValues *CSystem::GetUserConfigFileData( const char *dialogName, int dialogID )
{
	if( !m_pUserConfigData )
		return nullptr;

	char buf[ 256 ];
	const char* pszName = dialogName;

	if( dialogID )
	{
		snprintf( buf, sizeof( buf ), "%s_%d", dialogName, dialogID );
		pszName = buf;
	}

	return m_pUserConfigData->FindKey( pszName, true );
}

void CSystem::SetUserConfigFile( const char *fileName, const char *pathName )
{
	if( !m_pUserConfigData )
		m_pUserConfigData = new KeyValues( "UserConfigData" );

	strncpy( m_szFileName, fileName, sizeof( m_szFileName ) - 1 );
	strncpy( m_szPathID, pathName, sizeof( m_szPathID ) - 1 );

	m_pUserConfigData->LoadFromFile( vgui2::filesystem(), m_szFileName, m_szPathID );
}

void CSystem::SaveUserConfigFile()
{
	if( m_pUserConfigData )
		m_pUserConfigData->SaveToFile( vgui2::filesystem(), m_szFileName, m_szPathID );
}

bool CSystem::SetWatchForComputerUse( bool state )
{
	if( m_bStaticWatchForComputerUse != state )
		m_bStaticWatchForComputerUse = state;
	return true;
}

double CSystem::GetTimeSinceLastUse()
{
	if( !m_bStaticWatchForComputerUse )
		return 0;
	return GetCurrentTime() / 1000.0 - m_StaticLastComputerUseTime;
}

int CSystem::GetAvailableDrives( char *buf, int bufLen )
{
	if( buf && bufLen > 0 )
		buf[ 0 ] = '\0';
	return 0;
}

bool CSystem::CommandLineParamExists( const char *paramName )
{
	return strstr( m_szCommandLine, paramName ) != nullptr;
}

const char *CSystem::GetFullCommandLine()
{
	return m_szCommandLine;
}

bool CSystem::GetCurrentTimeAndDate( int *year, int *month, int *dayOfWeek, int *day, int *hour, int *minute, int *second )
{
	time_t rawtime;
	time( &rawtime );
	struct tm* timeinfo = localtime( &rawtime );

	if( year ) *year = timeinfo->tm_year + 1900;
	if( month ) *month = timeinfo->tm_mon + 1;
	if( dayOfWeek ) *dayOfWeek = timeinfo->tm_wday;
	if( day ) *day = timeinfo->tm_mday;
	if( hour ) *hour = timeinfo->tm_hour;
	if( minute ) *minute = timeinfo->tm_min;
	if( second ) *second = timeinfo->tm_sec;

	return true;
}

double CSystem::GetFreeDiskSpace( const char *path )
{
	struct statvfs buf;
	if( statvfs( path, &buf ) != 0 )
		return 0;
	return (double)buf.f_frsize * (double)buf.f_bavail;
}

bool CSystem::CreateShortcut( const char *linkFileName, const char *targetPath, const char *arguments, const char *workingDirectory, const char *iconFile )
{
	return false;
}

bool CSystem::GetShortcutTarget( const char *linkFileName, char *targetPath, char *arguments, int destBufferSizes )
{
	return false;
}

bool CSystem::ModifyShortcutTarget( const char *linkFileName, const char *targetPath, const char *arguments, const char *workingDirectory )
{
	return false;
}

bool CSystem::GetCommandLineParamValue( const char *paramName, char *value, int valueBufferSize )
{
	return false;
}

bool CSystem::DeleteRegistryKey( const char *keyName )
{
	return false;
}

const char *CSystem::GetDesktopFolderPath()
{
	return m_szDesktopFolderPath;
}

vgui2::KeyCode CSystem::KeyCode_VirtualKeyToVGUI( int keyCode )
{
	return ::KeyCode_VirtualKeyToVGUI( keyCode );
}

int CSystem::KeyCode_VGUIToVirtualKey( vgui2::KeyCode keyCode )
{
	return ::KeyCode_VGUIToVirtualKey( keyCode );
}

const char *CSystem::GetStartMenuFolderPath()
{
	return nullptr;
}

const char *CSystem::GetAllUserDesktopFolderPath()
{
	return nullptr;
}

const char *CSystem::GetAllUserStartMenuFolderPath()
{
	return nullptr;
}
