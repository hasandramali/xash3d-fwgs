#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include "vgui_api.h"
#include "xash3d_types.h"

#include "vgui_surface.h"

#include <vgui/IVGui.h>
#include <vgui/IPanel.h>
#include <vgui/ISurface.h>
#include <vgui/IInput.h>
#include <vgui/IInputInternal.h>
#include <vgui/IScheme.h>
#include <vgui/ISystem.h>
#include <vgui/ILocalize.h>
#include <vgui/Cursor.h>
#include <vgui/KeyCode.h>
#include <vgui/MouseCode.h>

#include <vgui_controls/Controls.h>
#include <vgui_controls/Panel.h>

#include <KeyValues.h>
#include <vstdlib/IKeyValuesSystem.h>
#include <FileSystem.h>
#include "interface.h"

#include "input.h"

using namespace vgui2;

static vguiapi_t *g_pAPI = NULL;
static Panel *g_pRootPanel = NULL;

static IBaseInterface* VGuiFactory(const char *pName, int *pReturnCode)
{
	return CreateInterface(pName, pReturnCode);
}

static void VGui_Startup(int width, int height)
{
	if (g_pRootPanel)
	{
		g_pRootPanel->SetSize(width, height);
		return;
	}

	if (!g_pAPI)
		return;

	g_pAPI->DrawInit();

	CreateInterfaceFn factoryList[1];
	factoryList[0] = VGuiFactory;
	if (!vgui2::VGui_InitInterfacesList("vgui_support", factoryList, 1))
	{
		fprintf(stderr, "CAndroidSurface:: VGui_InitInterfacesList failed\n");
		return;
	}

	if (vgui2::surface())
		static_cast<CAndroidSurface*>(vgui2::surface())->SetAPI(g_pAPI);

	int w = width, h = height;
	if (g_pAPI)
		g_pAPI->GetTextureSizes(&w, &h);

	g_pRootPanel = new Panel(NULL, "RootPanel");
	g_pRootPanel->SetSize(w, h);
	g_pRootPanel->SetPaintBorderEnabled(false);
	g_pRootPanel->SetPaintBackgroundEnabled(false);
	g_pRootPanel->SetVisible(true);
	g_pRootPanel->SetCursor(vgui2::dc_none);

	vgui2::surface()->SetEmbeddedPanel((VPANEL)g_pRootPanel->GetVPanel());

	if (ivgui())
		ivgui()->Start();
}

static void VGui_Shutdown()
{
	if (ivgui())
		ivgui()->Stop();

	delete g_pRootPanel;
	g_pRootPanel = NULL;

	if (g_pAPI)
		g_pAPI->DrawShutdown();
}

static void VGui_Paint()
{
	if (!g_pRootPanel || !g_pAPI)
		return;

	if (!g_pAPI->IsInGame())
		return;

	if (ivgui())
		ivgui()->RunFrame();

	VPANEL embedded = vgui2::surface()->GetEmbeddedPanel();
	if (embedded)
	{
		vgui2::surface()->SolveTraverse(embedded, false);
		vgui2::surface()->PaintTraverse(embedded);
	}
}

static void *VGui_GetPanel()
{
	return (void *)g_pRootPanel;
}

static void VGui_Mouse(VGUI_MouseAction action, int code)
{
	if (!g_pRootPanel) return;

	vgui2::MouseCode mouseCode = (vgui2::MouseCode)code;

	switch (action)
	{
	case MA_PRESSED:
		input()->InternalMousePressed(mouseCode);
		break;
	case MA_RELEASED:
		input()->InternalMouseReleased(mouseCode);
		break;
	case MA_DOUBLE:
		input()->InternalMouseDoublePressed(mouseCode);
		break;
	case MA_WHEEL:
		input()->InternalMouseWheeled(mouseCode);
		break;
	}
}

static void VGui_Key(VGUI_KeyAction action, VGUI_KeyCode code)
{
	if (!input()) return;

	switch (action)
	{
	case KA_TYPED:
	case KA_PRESSED:
		input()->InternalKeyCodePressed((vgui2::KeyCode)code);
		break;
	case KA_RELEASED:
		input()->InternalKeyCodeReleased((vgui2::KeyCode)code);
		break;
	}
}

static void VGui_MouseMove(int x, int y)
{
	if (input())
		input()->InternalCursorMoved(x, y);
}

static void VGui_TextInput(const char *text)
{
	if (input())
	{
		for (const char *p = text; *p; p++)
		{
			wchar_t wch = (wchar_t)(unsigned char)*p;
			input()->InternalKeyCodeTyped((vgui2::KeyCode)wch);
		}
	}
}

extern "C" EXPORT void InitAPI(vguiapi_t *api)
{
	if (!api) return;

	g_pAPI = api;

	api->Startup = VGui_Startup;
	api->Shutdown = VGui_Shutdown;
	api->GetPanel = VGui_GetPanel;
	api->Paint = VGui_Paint;
	api->Mouse = VGui_Mouse;
	api->Key = VGui_Key;
	api->MouseMove = VGui_MouseMove;
	api->TextInput = VGui_TextInput;

	api->initialized = true;
}
