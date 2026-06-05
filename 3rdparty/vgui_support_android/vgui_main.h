#ifndef VGUI_MAIN_H
#define VGUI_MAIN_H

#include "vgui_api.h"
#include "xash3d_types.h"

extern vguiapi_t *g_api;

class CRootPanel
{
public:
	CRootPanel();
	~CRootPanel();

	void SetSize( int width, int height );
	void GetSize( int &width, int &height );
	void Paint();
	void MouseEvent( VGUI_MouseAction action, int code );
	void KeyEvent( VGUI_KeyAction action, VGUI_KeyCode code );
	void MouseMove( int x, int y );
	void TextInput( const char *text );
	void *GetHandle() { return (void *)this; }
	bool IsVisible() { return m_bVisible; }
	void SetVisible( bool visible ) { m_bVisible = visible; }

private:
	int m_Width, m_Height;
	bool m_bVisible;
};

#endif
