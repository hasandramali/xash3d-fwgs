/*
 launchdialog.h - iOS lauch dialog
 Copyright (C) 2019 MoeMod Hymei
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 */

#ifndef launcherdialog_h
#define launcherdialog_h

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XashGameStatus_e
{
	XGS_WAITING = 0,
	XGS_START,
	XGS_SKIP
} XashGameStatus_t;

extern XashGameStatus_t g_iStartGameStatus;

extern int g_iArgc;
extern const char **g_pszArgv;

const char *IOS_GetDocsDir();
const char *IOS_GetBundleDir();
void IOS_SetDefaultArgs();
BOOL IOS_IsResourcesReady();
void IOS_LaunchDialog(void);
char *IOS_GetUDID(void);
void IOS_Log(const char *text);

// Immersive mode / safe area helpers
void IOS_ConstrainGameViewToSafeArea(void);

#ifdef __cplusplus
}
#endif

#endif /* launcherdialog_h */
