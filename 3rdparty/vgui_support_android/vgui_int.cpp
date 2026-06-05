#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>

#include "vgui_api.h"

#include "vgui_main.h"

vguiapi_t *g_api = NULL;
static CRootPanel *g_pRootPanel = NULL;

CRootPanel::CRootPanel()
	: m_Width( 640 ), m_Height( 480 ), m_bVisible( true )
{
}

CRootPanel::~CRootPanel()
{
}

void CRootPanel::SetSize( int width, int height )
{
	m_Width = width;
	m_Height = height;
}

void CRootPanel::GetSize( int &width, int &height )
{
	width = m_Width;
	height = m_Height;
}

void CRootPanel::Paint()
{
	if( !g_api || !g_api->DrawInit )
		return;

	g_api->DrawInit();
	g_api->DrawShutdown();
}

void CRootPanel::MouseEvent( VGUI_MouseAction action, int code )
{
}

void CRootPanel::KeyEvent( VGUI_KeyAction action, VGUI_KeyCode code )
{
}

void CRootPanel::MouseMove( int x, int y )
{
}

void CRootPanel::TextInput( const char *text )
{
}

static void VGui_Startup( int width, int height )
{
	if( !g_pRootPanel )
	{
		g_pRootPanel = new CRootPanel;
	}
	g_pRootPanel->SetSize( width, height );

	if( g_api && g_api->DrawInit )
		g_api->DrawInit();
}

static void VGui_Shutdown()
{
	delete g_pRootPanel;
	g_pRootPanel = NULL;

	if( g_api && g_api->DrawShutdown )
		g_api->DrawShutdown();
}

static void *VGui_GetPanel()
{
	if( g_pRootPanel )
		return g_pRootPanel->GetHandle();
	return NULL;
}

static void VGui_Paint()
{
	if( g_pRootPanel )
		g_pRootPanel->Paint();
}

static void VGui_Mouse( VGUI_MouseAction action, int code )
{
	if( g_pRootPanel )
		g_pRootPanel->MouseEvent( action, code );
}

static void VGui_Key( VGUI_KeyAction action, VGUI_KeyCode code )
{
	if( g_pRootPanel )
		g_pRootPanel->KeyEvent( action, code );
}

static void VGui_MouseMove( int x, int y )
{
	if( g_pRootPanel )
		g_pRootPanel->MouseMove( x, y );
}

static void VGui_TextInput( const char *text )
{
	if( g_pRootPanel )
		g_pRootPanel->TextInput( text );
}

extern "C" EXPORT void InitAPI( vguiapi_t *api )
{
	g_api = api;

	g_api->Startup = VGui_Startup;
	g_api->Shutdown = VGui_Shutdown;
	g_api->GetPanel = VGui_GetPanel;
	g_api->Paint = VGui_Paint;
	g_api->Mouse = VGui_Mouse;
	g_api->Key = VGui_Key;
	g_api->MouseMove = VGui_MouseMove;
	g_api->TextInput = VGui_TextInput;
}
