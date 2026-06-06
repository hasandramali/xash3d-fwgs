#include "vgui_surface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include <vgui/IPanel.h>
#include <vgui/IClientPanel.h>
#include <vgui/IInputInternal.h>
#include <vgui/IScheme.h>
#include <vgui/IVGui.h>

#include <vgui2_surfacelib/FontManager.h>
#include <vgui2_surfacelib/Win32Font.h>

#include "tier0/platform.h"
#include "VPanel.h"
#include "SDK_Color.h"

#define CURSOR_NONE dc_none
#define CURSOR_ARROW dc_arrow
#define CURSOR_IBEAM dc_ibeam
#define CURSOR_HOURGLASS dc_hourglass
#define CURSOR_CROSSHAIR dc_crosshair
#define CURSOR_UP dc_up
#define CURSOR_SIZENWSE dc_sizenwse
#define CURSOR_SIZENESW dc_sizenesw
#define CURSOR_SIZEWE dc_sizewe
#define CURSOR_SIZENS dc_sizens
#define CURSOR_SIZEALL dc_sizeall
#define CURSOR_NO dc_no
#define CURSOR_HAND dc_hand

CAndroidSurface::CAndroidSurface()
	: m_pAPI(NULL)
	, m_EmbeddedPanel(0)
	, m_nScreenWide(640)
	, m_nScreenTall(480)
	, m_nDrawTextX(0)
	, m_nDrawTextY(0)
	, m_ModalPanel(0)
	, m_nPopupCount(0)
	, m_CurrentFont(0)
{
	m_DrawColor[0] = m_DrawColor[1] = m_DrawColor[2] = m_DrawColor[3] = 255;
	m_DrawTextColor[0] = m_DrawTextColor[1] = m_DrawTextColor[2] = m_DrawTextColor[3] = 255;
	for (int i = 0; i < 64; i++)
		m_Popups[i] = 0;
}

CAndroidSurface::~CAndroidSurface()
{
}

void CAndroidSurface::Init()
{
}

void CAndroidSurface::Shutdown()
{
}

void CAndroidSurface::RunFrame()
{
}

VPANEL CAndroidSurface::GetEmbeddedPanel()
{
	return m_EmbeddedPanel;
}

void CAndroidSurface::SetEmbeddedPanel(VPANEL pPanel)
{
	m_EmbeddedPanel = pPanel;
}

void CAndroidSurface::PushMakeCurrent(VPANEL panel, bool useInsets)
{
}

void CAndroidSurface::PopMakeCurrent(VPANEL panel)
{
}

void CAndroidSurface::DrawSetColor(int r, int g, int b, int a)
{
	m_DrawColor[0] = r;
	m_DrawColor[1] = g;
	m_DrawColor[2] = b;
	m_DrawColor[3] = a;
	if (m_pAPI) m_pAPI->SetupDrawingRect(m_DrawColor);
}

void CAndroidSurface::DrawSetColor(SDK_Color col)
{
	DrawSetColor(col.r(), col.g(), col.b(), col.a());
}

void CAndroidSurface::DrawFilledRect(int x0, int y0, int x1, int y1)
{
	if (!m_pAPI) return;
	m_pAPI->EnableTexture(false);
	vpoint_t ul, lr;
	ul.point[0] = (float)x0;
	ul.point[1] = (float)y0;
	ul.coord[0] = 0;
	ul.coord[1] = 0;
	lr.point[0] = (float)x1;
	lr.point[1] = (float)y1;
	lr.coord[0] = 1;
	lr.coord[1] = 1;
	m_pAPI->DrawQuad(&ul, &lr);
}

void CAndroidSurface::DrawOutlinedRect(int x0, int y0, int x1, int y1)
{
	DrawFilledRect(x0, y0, x1, y0 + 1);
	DrawFilledRect(x0, y1 - 1, x1, y1);
	DrawFilledRect(x0, y0 + 1, x0 + 1, y1 - 1);
	DrawFilledRect(x1 - 1, y0 + 1, x1, y1 - 1);
}

void CAndroidSurface::DrawLine(int x0, int y0, int x1, int y1)
{
	DrawFilledRect(x0, y0, x1 + 1, y1 + 1);
}

void CAndroidSurface::DrawPolyLine(int *px, int *py, int numPoints)
{
	for (int i = 0; i < numPoints - 1; i++)
		DrawLine(px[i], py[i], px[i + 1], py[i + 1]);
}

void CAndroidSurface::DrawSetTextFont(vgui2::HFont font)
{
	m_CurrentFont = font;
}

void CAndroidSurface::DrawSetTextColor(int r, int g, int b, int a)
{
	m_DrawTextColor[0] = r;
	m_DrawTextColor[1] = g;
	m_DrawTextColor[2] = b;
	m_DrawTextColor[3] = a;
	if (m_pAPI) m_pAPI->SetupDrawingText(m_DrawTextColor);
}

void CAndroidSurface::DrawSetTextColor(SDK_Color col)
{
	DrawSetTextColor(col.r(), col.g(), col.b(), col.a());
}

void CAndroidSurface::DrawSetTextPos(int x, int y)
{
	m_nDrawTextX = x;
	m_nDrawTextY = y;
}

void CAndroidSurface::DrawGetTextPos(int &x, int &y)
{
	x = m_nDrawTextX;
	y = m_nDrawTextY;
}

struct GlyphTexture
{
	int textureID;
	int wide;
	int tall;
	float s0, t0, s1, t1;
};

static GlyphTexture s_GlyphCache[256];
static int s_nCachedGlyphs = 0;

void CAndroidSurface::DrawPrintText(const wchar_t *text, int textLen)
{
	if (!m_pAPI || !text || textLen <= 0) return;

	vgui2::HFont font = m_CurrentFont;

	int x = m_nDrawTextX;
	int y = m_nDrawTextY;

	for (int i = 0; i < textLen; i++)
	{
		wchar_t ch = text[i];
		if (ch == '\n')
		{
			x = m_nDrawTextX;
			y += 20;
			continue;
		}

		int a, b, c;
		FontManager().GetCharABCwide(font, ch, a, b, c);

		x += a;

		int fontWide, fontTall;
		FontManager().GetTextSize(font, &ch, fontWide, fontTall);
		fontTall = FontManager().GetFontTall(font);

		unsigned char rgba[128 * 128 * 4];
		FontManager().GetFontForChar(font, ch)->GetCharRGBA(ch, 0, 0, fontWide, fontTall, rgba);

		int texID = m_pAPI->GenerateTexture();
		m_pAPI->UploadTexture(texID, (const char *)rgba, fontWide, fontTall);
		m_pAPI->SetupDrawingImage(m_DrawTextColor);
		m_pAPI->BindTexture(texID);
		m_pAPI->EnableTexture(true);

		vpoint_t ul, lr;
		ul.point[0] = (float)x;
		ul.point[1] = (float)y;
		ul.coord[0] = 0;
		ul.coord[1] = 0;
		lr.point[0] = (float)(x + fontWide);
		lr.point[1] = (float)(y + fontTall);
		lr.coord[0] = 1;
		lr.coord[1] = 1;
		m_pAPI->DrawQuad(&ul, &lr);

		x += b + c;
	}

	m_nDrawTextX = x;
}

void CAndroidSurface::DrawUnicodeChar(wchar_t wch)
{
	DrawPrintText(&wch, 1);
}

void CAndroidSurface::DrawUnicodeCharAdd(wchar_t wch)
{
}

void CAndroidSurface::DrawFlushText()
{
}

void CAndroidSurface::DrawSetTextureFile(int id, const char *filename, int hardwareFilter, bool forceReload)
{
}

void CAndroidSurface::DrawSetTextureRGBA(int id, const unsigned char *rgba, int wide, int tall, int hardwareFilter, bool forceReload)
{
	if (!m_pAPI) return;
	m_pAPI->UploadTexture(id, (const char *)rgba, wide, tall);
}

void CAndroidSurface::DrawSetTexture(int id)
{
	if (!m_pAPI) return;
	m_pAPI->BindTexture(id);
}

void CAndroidSurface::DrawGetTextureSize(int id, int &wide, int &tall)
{
	if (m_pAPI)
		m_pAPI->GetTextureSizes(&wide, &tall);
	else
		wide = tall = 0;
}

void CAndroidSurface::DrawTexturedRect(int x0, int y0, int x1, int y1)
{
	if (!m_pAPI) return;
	m_pAPI->SetupDrawingImage(m_DrawColor);
	m_pAPI->EnableTexture(true);
	vpoint_t ul, lr;
	ul.point[0] = (float)x0;
	ul.point[1] = (float)y0;
	ul.coord[0] = 0;
	ul.coord[1] = 0;
	lr.point[0] = (float)x1;
	lr.point[1] = (float)y1;
	lr.coord[0] = 1;
	lr.coord[1] = 1;
	m_pAPI->DrawQuad(&ul, &lr);
}

bool CAndroidSurface::IsTextureIDValid(int id)
{
	return id > 0;
}

int CAndroidSurface::CreateNewTextureID(bool procedural)
{
	if (!m_pAPI) return 0;
	return m_pAPI->GenerateTexture();
}

void CAndroidSurface::GetScreenSize(int &wide, int &tall)
{
	wide = m_nScreenWide;
	tall = m_nScreenTall;
}

void CAndroidSurface::SetAsTopMost(VPANEL panel, bool state) {}
void CAndroidSurface::BringToFront(VPANEL panel) {}
void CAndroidSurface::SetForegroundWindow(VPANEL panel) {}

void CAndroidSurface::SetPanelVisible(VPANEL panel, bool state)
{
	vgui2::	vgui2::VHandleToPanel(panel)->SetVisible(state);
}

void CAndroidSurface::SetMinimized(VPANEL panel, bool state) {}
bool CAndroidSurface::IsMinimized(VPANEL panel) { return false; }
void CAndroidSurface::FlashWindow(VPANEL panel, bool state) {}

void CAndroidSurface::SetTitle(VPANEL panel, const wchar_t *title) {}
void CAndroidSurface::SetAsToolBar(VPANEL panel, bool state) {}

void CAndroidSurface::CreatePopup(VPANEL panel, bool minimised, bool showTaskbarIcon, bool disabled, bool mouseInput, bool kbInput)
{
	if (m_nPopupCount < 64)
		m_Popups[m_nPopupCount++] = panel;
}

void CAndroidSurface::SwapBuffers(VPANEL panel) {}
void CAndroidSurface::Invalidate(VPANEL panel) {}

void CAndroidSurface::SetCursor(vgui2::HCursor cursor)
{
	if (!m_pAPI) return;
	switch (cursor)
	{
	case vgui2::dc_user: m_pAPI->CursorSelect(CURSOR_ARROW); break;
	case vgui2::dc_none: m_pAPI->CursorSelect(CURSOR_NONE); break;
	case vgui2::dc_arrow: m_pAPI->CursorSelect(CURSOR_ARROW); break;
	case vgui2::dc_ibeam: m_pAPI->CursorSelect(CURSOR_IBEAM); break;
	case vgui2::dc_hourglass: m_pAPI->CursorSelect(CURSOR_HOURGLASS); break;
	case vgui2::dc_crosshair: m_pAPI->CursorSelect(CURSOR_CROSSHAIR); break;
	case vgui2::dc_up: m_pAPI->CursorSelect(CURSOR_UP); break;
	case vgui2::dc_sizenwse: m_pAPI->CursorSelect(CURSOR_SIZENWSE); break;
	case vgui2::dc_sizenesw: m_pAPI->CursorSelect(CURSOR_SIZENESW); break;
	case vgui2::dc_sizewe: m_pAPI->CursorSelect(CURSOR_SIZEWE); break;
	case vgui2::dc_sizens: m_pAPI->CursorSelect(CURSOR_SIZENS); break;
	case vgui2::dc_sizeall: m_pAPI->CursorSelect(CURSOR_SIZEALL); break;
	case vgui2::dc_no: m_pAPI->CursorSelect(CURSOR_NO); break;
	case vgui2::dc_hand: m_pAPI->CursorSelect(CURSOR_HAND); break;
	default: m_pAPI->CursorSelect(CURSOR_ARROW); break;
	}
}

bool CAndroidSurface::IsCursorVisible() { return true; }
void CAndroidSurface::ApplyChanges() {}
bool CAndroidSurface::IsWithin(int x, int y) { return true; }
bool CAndroidSurface::HasFocus() { return true; }

bool CAndroidSurface::SupportsFeature(SurfaceFeature_e feature)
{
	switch (feature)
	{
	case ANTIALIASED_FONTS:
	case DROPSHADOW_FONTS:
	case ESCAPE_KEY:
		return false;
	default:
		return false;
	}
}

void CAndroidSurface::RestrictPaintToSinglePanel(VPANEL panel) {}
void CAndroidSurface::SetModalPanel(VPANEL modal) { m_ModalPanel = modal; }
VPANEL CAndroidSurface::GetModalPanel() { return m_ModalPanel; }
void CAndroidSurface::UnlockCursor() {}
void CAndroidSurface::LockCursor() {}
void CAndroidSurface::SetTranslateExtendedKeys(bool state) {}
VPANEL CAndroidSurface::GetTopmostPopup()
{
	if (m_nPopupCount > 0) return m_Popups[m_nPopupCount - 1];
	return 0;
}

void CAndroidSurface::SetTopLevelFocus(VPANEL panel) {}

vgui2::HFont CAndroidSurface::CreateFont()
{
	return FontManager().CreateFont();
}

bool CAndroidSurface::AddGlyphSetToFont(vgui2::HFont font, const char *windowsFontName, int tall, int weight, int blur, int scanlines, int flags, int lowRange, int highRange)
{
	return FontManager().AddGlyphSetToFont(font, windowsFontName, tall, weight, blur, scanlines, flags);
}

bool CAndroidSurface::AddCustomFontFile(const char *fontFileName) { return false; }

int CAndroidSurface::GetFontTall(vgui2::HFont font)
{
	return FontManager().GetFontTall(font);
}

void CAndroidSurface::GetCharABCwide(vgui2::HFont font, int ch, int &a, int &b, int &c)
{
	FontManager().GetCharABCwide(font, ch, a, b, c);
}

int CAndroidSurface::GetCharacterWidth(vgui2::HFont font, int ch)
{
	return FontManager().GetCharacterWidth(font, ch);
}

void CAndroidSurface::GetTextSize(vgui2::HFont font, const wchar_t *text, int &wide, int &tall)
{
	FontManager().GetTextSize(font, text, wide, tall);
}

VPANEL CAndroidSurface::GetNotifyPanel() { return 0; }

void CAndroidSurface::SetNotifyIcon(VPANEL context, vgui2::HTexture icon, VPANEL panelToReceiveMessages, const char *text) {}

void CAndroidSurface::PlaySound(const char *fileName) {}

int CAndroidSurface::GetPopupCount() { return m_nPopupCount; }

VPANEL CAndroidSurface::GetPopup(int index)
{
	if (index >= 0 && index < m_nPopupCount)
		return m_Popups[index];
	return 0;
}

bool CAndroidSurface::ShouldPaintChildPanel(VPANEL childPanel) { return true; }
bool CAndroidSurface::RecreateContext(VPANEL panel) { return false; }

void CAndroidSurface::AddPanel(VPANEL panel) {}
void CAndroidSurface::ReleasePanel(VPANEL panel) {}

void CAndroidSurface::MovePopupToFront(VPANEL panel)
{
	int found = -1;
	for (int i = 0; i < m_nPopupCount; i++)
	{
		if (m_Popups[i] == panel) { found = i; break; }
	}
	if (found >= 0 && found < m_nPopupCount - 1)
	{
		VPANEL tmp = m_Popups[found];
		for (int i = found; i < m_nPopupCount - 1; i++)
			m_Popups[i] = m_Popups[i + 1];
		m_Popups[m_nPopupCount - 1] = tmp;
	}
}

void CAndroidSurface::MovePopupToBack(VPANEL panel)
{
	int found = -1;
	for (int i = 0; i < m_nPopupCount; i++)
	{
		if (m_Popups[i] == panel) { found = i; break; }
	}
	if (found > 0)
	{
		VPANEL tmp = m_Popups[found];
		for (int i = found; i > 0; i--)
			m_Popups[i] = m_Popups[i - 1];
		m_Popups[0] = tmp;
	}
}

void CAndroidSurface::SolveTraverse(VPANEL panel, bool forceApplySchemeSettings)
{
	if (!panel) return;
	vgui2::VHandleToPanel(panel)->SolveTraverse(forceApplySchemeSettings);
}

void CAndroidSurface::PaintTraverse(VPANEL panel)
{
	if (!panel) return;
	vgui2::VHandleToPanel(panel)->PaintTraverse();
}

void CAndroidSurface::EnableMouseCapture(VPANEL panel, bool state) {}

void CAndroidSurface::GetWorkspaceBounds(int &x, int &y, int &wide, int &tall)
{
	x = 0; y = 0;
	wide = m_nScreenWide;
	tall = m_nScreenTall;
}

void CAndroidSurface::GetAbsoluteWindowBounds(int &x, int &y, int &wide, int &tall)
{
	x = 0; y = 0;
	wide = m_nScreenWide;
	tall = m_nScreenTall;
}

void CAndroidSurface::GetProportionalBase(int &width, int &height)
{
	width = 640;
	height = 480;
}

void CAndroidSurface::CalculateMouseVisible() {}
bool CAndroidSurface::NeedKBInput() { return false; }
bool CAndroidSurface::HasCursorPosFunctions() { return true; }

void CAndroidSurface::SurfaceGetCursorPos(int &x, int &y)
{
	if (m_pAPI) m_pAPI->GetCursorPos(&x, &y);
}

void CAndroidSurface::SurfaceSetCursorPos(int x, int y) {}

void CAndroidSurface::DrawTexturedPolygon(VGuiVertex *pVertices, int n) {}

int CAndroidSurface::GetFontAscent(vgui2::HFont font, wchar_t wch)
{
	return FontManager().GetFontAscent(font, wch);
}

void CAndroidSurface::SetAllowHTMLJavaScript(bool state) {}
void CAndroidSurface::SetLanguage(const char *pchLang) {}
const char *CAndroidSurface::GetLanguage() { return "english"; }
bool CAndroidSurface::DeleteTextureByID(int id) { return false; }

void CAndroidSurface::DrawUpdateRegionTextureBGRA(int nTextureID, int x, int y, const unsigned char *pchData, int wide, int tall) {}
void CAndroidSurface::DrawSetTextureBGRA(int id, const unsigned char *pchData, int wide, int tall) {}

void CAndroidSurface::CreateBrowser(vgui2::VPANEL panel, IHTMLResponses *pBrowser, bool bPopupWindow, const char *pchUserAgentIdentifier) {}
void CAndroidSurface::RemoveBrowser(vgui2::VPANEL panel, IHTMLResponses *pBrowser) {}
IHTMLChromeController *CAndroidSurface::AccessChromeHTMLController() { return NULL; }

EXPOSE_SINGLE_INTERFACE( CAndroidSurface, ISurface, VGUI_SURFACE_INTERFACE_VERSION_GS );
