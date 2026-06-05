#ifndef IHTMLCHROME_H
#define IHTMLCHROME_H

#ifdef _WIN32
#pragma once
#endif

#include "tier1/UtlBuffer.h"

enum EHTMLCommands
{
	EHTMLCommands_Invalid = 0
};

struct HTMLCommandBuffer_t
{
	EHTMLCommands m_eCmd;
	int m_iBrowser;
	CUtlBuffer m_Buffer;
};

class IHTMLResponses
{
public:
	virtual ~IHTMLResponses() {}
};

class IHTMLResponses_HL1
{
public:
	virtual void BrowserReady() = 0;
	virtual void BrowserNeedsPaint( int textureid, int wide, int tall, const unsigned char* rgba, int updatex, int updatey, int updatewide, int updatetall, int combobox_wide, int combobox_tall, const unsigned char* combobox_rgba ) = 0;
	virtual void BrowserStartRequest( const char *url, const char *target, const char *postdata, bool isredirect ) = 0;
	virtual void BrowserURLChanged( const char *url, const char *postdata, bool isredirect ) = 0;
	virtual void BrowserFinishedRequest( const char *url, const char *pagetitle ) = 0;
	virtual void BrowserShowPopup() = 0;
	virtual void BrowserHidePopup() = 0;
	virtual void BrowserSizePopup( int x, int y, int wide, int tall ) = 0;
	virtual void BrowserHorizontalScrollBarSizeResponse( int x, int y, int wide, int tall, int scroll, int scroll_max, float zoom ) = 0;
	virtual void BrowserVerticalScrollBarSizeResponse( int x, int y, int wide, int tall, int scroll, int scroll_max, float zoom ) = 0;
	virtual void BrowserGetZoomResponse( float flZoom ) = 0;
	virtual void BrowserCanGoBackandForward( bool bgoback, bool bgoforward ) = 0;
	virtual void BrowserJSAlert( const char *message ) = 0;
	virtual void BrowserJSConfirm( const char *message ) = 0;
	virtual void BrowserPopupHTMLWindow( const char *url, int wide, int tall, int x, int y ) = 0;
	virtual void BrowserSetHTMLTitle( const char *title ) = 0;
	virtual void BrowserLoadingResource() = 0;
	virtual void BrowserStatusText( const char *text ) = 0;
	virtual void BrowserSetCursor( int in_cursor ) = 0;
	virtual void BrowserFileLoadDialog() = 0;
	virtual void BrowserShowToolTip( const char *text ) = 0;
	virtual void BrowserUpdateToolTip( const char *text ) = 0;
	virtual void BrowserHideToolTip() = 0;
	virtual void BrowserClose() = 0;
	virtual void BrowserLinkAtPositionResponse( const char *url, int x, int y ) = 0;
};

#endif
