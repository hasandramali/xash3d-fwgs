#ifndef VGUI_SURFACE_H
#define VGUI_SURFACE_H

#include <vgui/VGUI2.h>
#include <vgui/ISurface.h>
#include <vgui/IPanel.h>
#include <vgui/Cursor.h>
#include <vgui/KeyCode.h>
#include <vgui/MouseCode.h>
#include <vgui/IClientPanel.h>
#include <vgui/IInputInternal.h>

#include "tier0/platform.h"

#include "vgui_api.h"

class CAndroidSurface : public vgui2::ISurface
{
public:
	CAndroidSurface();
	~CAndroidSurface();

	void SetAPI(vguiapi_t *api) { m_pAPI = api; }
	vguiapi_t *GetAPI() { return m_pAPI; }

	void Init();
	void Shutdown();

	virtual void RunFrame();

	virtual VPANEL GetEmbeddedPanel();
	virtual void SetEmbeddedPanel(VPANEL pPanel);

	virtual void PushMakeCurrent(VPANEL panel, bool useInsets);
	virtual void PopMakeCurrent(VPANEL panel);

	virtual void DrawSetColor(int r, int g, int b, int a);
	virtual void DrawSetColor(SDK_Color col);
	virtual void DrawFilledRect(int x0, int y0, int x1, int y1);
	virtual void DrawOutlinedRect(int x0, int y0, int x1, int y1);
	virtual void DrawLine(int x0, int y0, int x1, int y1);
	virtual void DrawPolyLine(int *px, int *py, int numPoints);

	virtual void DrawSetTextFont(vgui2::HFont font);
	virtual void DrawSetTextColor(int r, int g, int b, int a);
	virtual void DrawSetTextColor(SDK_Color col);
	virtual void DrawSetTextPos(int x, int y);
	virtual void DrawGetTextPos(int &x, int &y);
	virtual void DrawPrintText(const wchar_t *text, int textLen);
	virtual void DrawUnicodeChar(wchar_t wch);
	virtual void DrawUnicodeCharAdd(wchar_t wch);
	virtual void DrawFlushText();

	virtual void DrawSetTextureFile(int id, const char *filename, int hardwareFilter, bool forceReload);
	virtual void DrawSetTextureRGBA(int id, const unsigned char *rgba, int wide, int tall, int hardwareFilter, bool forceReload);
	virtual void DrawSetTexture(int id);
	virtual void DrawGetTextureSize(int id, int &wide, int &tall);
	virtual void DrawTexturedRect(int x0, int y0, int x1, int y1);
	virtual bool IsTextureIDValid(int id);
	virtual int CreateNewTextureID(bool procedural = false);
	virtual void GetScreenSize(int &wide, int &tall);
	virtual void SetAsTopMost(VPANEL panel, bool state);
	virtual void BringToFront(VPANEL panel);
	virtual void SetForegroundWindow(VPANEL panel);
	virtual void SetPanelVisible(VPANEL panel, bool state);
	virtual void SetMinimized(VPANEL panel, bool state);
	virtual bool IsMinimized(VPANEL panel);
	virtual void FlashWindow(VPANEL panel, bool state);
	virtual void SetTitle(VPANEL panel, const wchar_t *title);
	virtual void SetAsToolBar(VPANEL panel, bool state);
	virtual void CreatePopup(VPANEL panel, bool minimised, bool showTaskbarIcon, bool disabled, bool mouseInput, bool kbInput);
	virtual void SwapBuffers(VPANEL panel);
	virtual void Invalidate(VPANEL panel);
	virtual void SetCursor(vgui2::HCursor cursor);
	virtual bool IsCursorVisible();
	virtual void ApplyChanges();
	virtual bool IsWithin(int x, int y);
	virtual bool HasFocus();

	virtual bool SupportsFeature(SurfaceFeature_e feature);
	virtual void RestrictPaintToSinglePanel(VPANEL panel);
	virtual void SetModalPanel(VPANEL modal);
	virtual VPANEL GetModalPanel();
	virtual void UnlockCursor();
	virtual void LockCursor();
	virtual void SetTranslateExtendedKeys(bool state);
	virtual VPANEL GetTopmostPopup();
	virtual void SetTopLevelFocus(VPANEL panel);

	virtual vgui2::HFont CreateFont();
	virtual bool AddGlyphSetToFont(vgui2::HFont font, const char *windowsFontName, int tall, int weight, int blur, int scanlines, int flags, int lowRange, int highRange);
	virtual bool AddCustomFontFile(const char *fontFileName);
	virtual int GetFontTall(vgui2::HFont font);
	virtual void GetCharABCwide(vgui2::HFont font, int ch, int &a, int &b, int &c);
	virtual int GetCharacterWidth(vgui2::HFont font, int ch);
	virtual void GetTextSize(vgui2::HFont font, const wchar_t *text, int &wide, int &tall);

	virtual VPANEL GetNotifyPanel();
	virtual void SetNotifyIcon(VPANEL context, vgui2::HTexture icon, VPANEL panelToReceiveMessages, const char *text);

	virtual void PlaySound(const char *fileName);

	virtual int GetPopupCount();
	virtual VPANEL GetPopup(int index);
	virtual bool ShouldPaintChildPanel(VPANEL childPanel);
	virtual bool RecreateContext(VPANEL panel);
	virtual void AddPanel(VPANEL panel);
	virtual void ReleasePanel(VPANEL panel);
	virtual void MovePopupToFront(VPANEL panel);
	virtual void MovePopupToBack(VPANEL panel);

	virtual void SolveTraverse(VPANEL panel, bool forceApplySchemeSettings = false);
	virtual void PaintTraverse(VPANEL panel);

	virtual void EnableMouseCapture(VPANEL panel, bool state);
	virtual void GetWorkspaceBounds(int &x, int &y, int &wide, int &tall);
	virtual void GetAbsoluteWindowBounds(int &x, int &y, int &wide, int &tall);
	virtual void GetProportionalBase(int &width, int &height);
	virtual void CalculateMouseVisible();
	virtual bool NeedKBInput();
	virtual bool HasCursorPosFunctions();
	virtual void SurfaceGetCursorPos(int &x, int &y);
	virtual void SurfaceSetCursorPos(int x, int y);

	virtual void DrawTexturedPolygon(VGuiVertex *pVertices, int n);
	virtual int GetFontAscent(vgui2::HFont font, wchar_t wch);
	virtual void SetAllowHTMLJavaScript(bool state);
	virtual void SetLanguage(const char *pchLang);
	virtual const char *GetLanguage();
	virtual bool DeleteTextureByID(int id);
	virtual void DrawUpdateRegionTextureBGRA(int nTextureID, int x, int y, const unsigned char *pchData, int wide, int tall);
	virtual void DrawSetTextureBGRA(int id, const unsigned char *pchData, int wide, int tall);
	virtual void CreateBrowser(vgui2::VPANEL panel, IHTMLResponses *pBrowser, bool bPopupWindow, const char *pchUserAgentIdentifier);
	virtual void RemoveBrowser(vgui2::VPANEL panel, IHTMLResponses *pBrowser);
	virtual IHTMLChromeController *AccessChromeHTMLController();

private:
	vguiapi_t *m_pAPI;
	VPANEL m_EmbeddedPanel;
	int m_nScreenWide;
	int m_nScreenTall;
	int m_nDrawTextX;
	int m_nDrawTextY;
	int m_DrawColor[4];
	int m_DrawTextColor[4];
	vgui2::HFont m_CurrentFont;
	VPANEL m_ModalPanel;
	int m_nPopupCount;
	VPANEL m_Popups[64];
};

#endif
