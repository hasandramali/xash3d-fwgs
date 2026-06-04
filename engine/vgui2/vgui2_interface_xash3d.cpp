#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include "interface.h"
#include "library.h"

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

CreateInterfaceFn Sys_GetFactoryThis(void)
{
	return CreateInterface;
}

CSysModule* Sys_LoadModule(const char* pModuleName)
{
	return (CSysModule*)COM_LoadLibrary(pModuleName, false, false);
}

void Sys_UnloadModule(CSysModule* pModule)
{
	if (pModule)
		COM_FreeLibrary(pModule);
}

CreateInterfaceFn Sys_GetFactory(CSysModule* pModule)
{
	if (!pModule)
		return NULL;
	return (CreateInterfaceFn)COM_GetProcAddress(pModule, "CreateInterface");
}

CreateInterfaceFn Sys_GetFactory(const char* pModuleName)
{
	if (!pModuleName)
		return NULL;
	CSysModule* mod = Sys_LoadModule(pModuleName);
	if (!mod)
		return NULL;
	return Sys_GetFactory(mod);
}
