#include "interface.h"
#include <string.h>

InterfaceReg* InterfaceReg::s_pInterfaceRegs = NULL;

InterfaceReg::InterfaceReg(InstantiateInterfaceFn fn, const char* pName)
	: m_pName(pName)
{
	m_CreateFn = fn;
	m_pNext = s_pInterfaceRegs;
	s_pInterfaceRegs = this;
}

extern "C"
{
	EXPORT_FUNCTION IBaseInterface* CreateInterface(const char *pName, int *pReturnCode)
	{
		for (InterfaceReg* pCur = InterfaceReg::s_pInterfaceRegs; pCur; pCur = pCur->m_pNext)
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
}

CreateInterfaceFn Sys_GetFactoryThis(void)
{
	return CreateInterface;
}
