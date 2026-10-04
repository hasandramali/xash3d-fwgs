/*
net_encode.c - encode network messages
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "netchan.h"
#include "xash3d_mathlib.h"
#include "net_encode.h"
#include "event_api.h"
#include "entity_state.h"
#include "weaponinfo.h"
#include "event_args.h"
#include "protocol.h"
#include "client.h"

#define DELTA_PATH		"delta.lst"

#define DT_BYTE		BIT( 0 )	// A byte
#define DT_SHORT		BIT( 1 ) 	// 2 byte field
#define DT_FLOAT		BIT( 2 )	// A floating point field
#define DT_INTEGER		BIT( 3 )	// 4 byte integer
#define DT_ANGLE		BIT( 4 )	// A floating point angle ( will get masked correctly )
#define DT_TIMEWINDOW_8	BIT( 5 )	// A floating point timestamp, relative to sv.time
#define DT_TIMEWINDOW_BIG	BIT( 6 )	// and re-encoded on the client relative to the client's clock
#define DT_STRING		BIT( 7 )	// A null terminated string, sent as 8 byte chars
#define DT_SIGNED		BIT( 8 )	// sign modificator
#define DT_SIGNED_GS	BIT( 31 ) // GoldSrc-specific sign modificator

// The GoldSrc/Sven wire does not carry per-field type flags, so they are
// spelled out explicitly per field below (derived from the member C types;
// keep them in sync when touching the structs).
// When adding a field, pick the matching DT_* combo for its C type:
// float/double -> DT_FLOAT|DT_SIGNED, int -> DT_INTEGER|DT_SIGNED,
// unsigned int -> DT_INTEGER, short -> DT_SHORT|DT_SIGNED,
// unsigned short -> DT_SHORT, char -> DT_BYTE,
// signed char -> DT_BYTE|DT_SIGNED, unsigned char -> DT_BYTE,
// arrays/vec3_t/strings -> DT_STRING.

static qboolean		delta_init = false;

// list of all the struct names
static const delta_field_t cmd_fields[] =
{
{ "lerp_msec", offsetof( usercmd_t, lerp_msec ), sizeof( ((usercmd_t *)0)->lerp_msec ), DT_SHORT|DT_SIGNED },
{ "msec", offsetof( usercmd_t, msec ), sizeof( ((usercmd_t *)0)->msec ), DT_BYTE },
{ "viewangles[0]", offsetof( usercmd_t, viewangles[0] ), sizeof( ((usercmd_t *)0)->viewangles[0] ), DT_FLOAT|DT_SIGNED },
{ "viewangles[1]", offsetof( usercmd_t, viewangles[1] ), sizeof( ((usercmd_t *)0)->viewangles[1] ), DT_FLOAT|DT_SIGNED },
{ "viewangles[2]", offsetof( usercmd_t, viewangles[2] ), sizeof( ((usercmd_t *)0)->viewangles[2] ), DT_FLOAT|DT_SIGNED },
{ "forwardmove", offsetof( usercmd_t, forwardmove ), sizeof( ((usercmd_t *)0)->forwardmove ), DT_FLOAT|DT_SIGNED },
{ "sidemove", offsetof( usercmd_t, sidemove ), sizeof( ((usercmd_t *)0)->sidemove ), DT_FLOAT|DT_SIGNED },
{ "upmove", offsetof( usercmd_t, upmove ), sizeof( ((usercmd_t *)0)->upmove ), DT_FLOAT|DT_SIGNED },
{ "lightlevel", offsetof( usercmd_t, lightlevel ), sizeof( ((usercmd_t *)0)->lightlevel ), DT_BYTE },
{ "buttons", offsetof( usercmd_t, buttons ), sizeof( ((usercmd_t *)0)->buttons ), DT_SHORT },
{ "impulse", offsetof( usercmd_t, impulse ), sizeof( ((usercmd_t *)0)->impulse ), DT_BYTE },
{ "weaponselect", offsetof( usercmd_t, weaponselect ), sizeof( ((usercmd_t *)0)->weaponselect ), DT_BYTE },
{ "impact_index", offsetof( usercmd_t, reserved[0] ), sizeof( ((usercmd_t *)0)->reserved[0] ), DT_INTEGER|DT_SIGNED },
{ "impact_position[0]", offsetof( usercmd_t, reserved[1] ), sizeof( ((usercmd_t *)0)->reserved[1] ), DT_INTEGER|DT_SIGNED },
{ "impact_position[1]", offsetof( usercmd_t, reserved[2] ), sizeof( ((usercmd_t *)0)->reserved[2] ), DT_INTEGER|DT_SIGNED },
{ "impact_position[2]", offsetof( usercmd_t, reserved[3] ), sizeof( ((usercmd_t *)0)->reserved[3] ), DT_INTEGER|DT_SIGNED },
};

static const delta_field_t pm_fields[] =
{
{ "gravity", offsetof( movevars_t, gravity ), sizeof( ((movevars_t *)0)->gravity ), DT_FLOAT|DT_SIGNED },
{ "stopspeed", offsetof( movevars_t, stopspeed ), sizeof( ((movevars_t *)0)->stopspeed ), DT_FLOAT|DT_SIGNED },
{ "maxspeed", offsetof( movevars_t, maxspeed ), sizeof( ((movevars_t *)0)->maxspeed ), DT_FLOAT|DT_SIGNED },
{ "spectatormaxspeed", offsetof( movevars_t, spectatormaxspeed ), sizeof( ((movevars_t *)0)->spectatormaxspeed ), DT_FLOAT|DT_SIGNED },
{ "accelerate", offsetof( movevars_t, accelerate ), sizeof( ((movevars_t *)0)->accelerate ), DT_FLOAT|DT_SIGNED },
{ "airaccelerate", offsetof( movevars_t, airaccelerate ), sizeof( ((movevars_t *)0)->airaccelerate ), DT_FLOAT|DT_SIGNED },
{ "wateraccelerate", offsetof( movevars_t, wateraccelerate ), sizeof( ((movevars_t *)0)->wateraccelerate ), DT_FLOAT|DT_SIGNED },
{ "friction", offsetof( movevars_t, friction ), sizeof( ((movevars_t *)0)->friction ), DT_FLOAT|DT_SIGNED },
{ "edgefriction", offsetof( movevars_t, edgefriction ), sizeof( ((movevars_t *)0)->edgefriction ), DT_FLOAT|DT_SIGNED },
{ "waterfriction", offsetof( movevars_t, waterfriction ), sizeof( ((movevars_t *)0)->waterfriction ), DT_FLOAT|DT_SIGNED },
{ "bounce", offsetof( movevars_t, bounce ), sizeof( ((movevars_t *)0)->bounce ), DT_FLOAT|DT_SIGNED },
{ "stepsize", offsetof( movevars_t, stepsize ), sizeof( ((movevars_t *)0)->stepsize ), DT_FLOAT|DT_SIGNED },
{ "maxvelocity", offsetof( movevars_t, maxvelocity ), sizeof( ((movevars_t *)0)->maxvelocity ), DT_FLOAT|DT_SIGNED },
{ "zmax", offsetof( movevars_t, zmax ), sizeof( ((movevars_t *)0)->zmax ), DT_FLOAT|DT_SIGNED },
{ "waveHeight", offsetof( movevars_t, waveHeight ), sizeof( ((movevars_t *)0)->waveHeight ), DT_FLOAT|DT_SIGNED },
{ "footsteps", offsetof( movevars_t, footsteps ), sizeof( ((movevars_t *)0)->footsteps ), DT_INTEGER|DT_SIGNED },
{ "skyName", offsetof( movevars_t, skyName ), sizeof( ((movevars_t *)0)->skyName ), DT_STRING },
{ "rollangle", offsetof( movevars_t, rollangle ), sizeof( ((movevars_t *)0)->rollangle ), DT_FLOAT|DT_SIGNED },
{ "rollspeed", offsetof( movevars_t, rollspeed ), sizeof( ((movevars_t *)0)->rollspeed ), DT_FLOAT|DT_SIGNED },
{ "skycolor_r", offsetof( movevars_t, skycolor[0] ), sizeof( ((movevars_t *)0)->skycolor[0] ), DT_FLOAT|DT_SIGNED },
{ "skycolor_g", offsetof( movevars_t, skycolor[1] ), sizeof( ((movevars_t *)0)->skycolor[1] ), DT_FLOAT|DT_SIGNED },
{ "skycolor_b", offsetof( movevars_t, skycolor[2] ), sizeof( ((movevars_t *)0)->skycolor[2] ), DT_FLOAT|DT_SIGNED },
{ "skyvec_x", offsetof( movevars_t, skyvec[0] ), sizeof( ((movevars_t *)0)->skyvec[0] ), DT_FLOAT|DT_SIGNED },
{ "skyvec_y", offsetof( movevars_t, skyvec[1] ), sizeof( ((movevars_t *)0)->skyvec[1] ), DT_FLOAT|DT_SIGNED },
{ "skyvec_z", offsetof( movevars_t, skyvec[2] ), sizeof( ((movevars_t *)0)->skyvec[2] ), DT_FLOAT|DT_SIGNED },
{ "fog_settings", offsetof( movevars_t, fog_settings ), sizeof( ((movevars_t *)0)->fog_settings ), DT_INTEGER|DT_SIGNED },
{ "wateralpha", offsetof( movevars_t, wateralpha ), sizeof( ((movevars_t *)0)->wateralpha ), DT_FLOAT|DT_SIGNED },
{ "skydir_x", offsetof( movevars_t, skydir[0] ), sizeof( ((movevars_t *)0)->skydir[0] ), DT_FLOAT|DT_SIGNED },
{ "skydir_y", offsetof( movevars_t, skydir[1] ), sizeof( ((movevars_t *)0)->skydir[1] ), DT_FLOAT|DT_SIGNED },
{ "skydir_z", offsetof( movevars_t, skydir[2] ), sizeof( ((movevars_t *)0)->skydir[2] ), DT_FLOAT|DT_SIGNED },
{ "skyangle", offsetof( movevars_t, skyangle ), sizeof( ((movevars_t *)0)->skyangle ), DT_FLOAT|DT_SIGNED },
};

static const delta_field_t ev_fields[] =
{
{ "flags", offsetof( event_args_t, flags ), sizeof( ((event_args_t *)0)->flags ), DT_INTEGER|DT_SIGNED },
{ "entindex", offsetof( event_args_t, entindex ), sizeof( ((event_args_t *)0)->entindex ), DT_INTEGER|DT_SIGNED },
{ "origin[0]", offsetof( event_args_t, origin[0] ), sizeof( ((event_args_t *)0)->origin[0] ), DT_FLOAT|DT_SIGNED },
{ "origin[1]", offsetof( event_args_t, origin[1] ), sizeof( ((event_args_t *)0)->origin[1] ), DT_FLOAT|DT_SIGNED },
{ "origin[2]", offsetof( event_args_t, origin[2] ), sizeof( ((event_args_t *)0)->origin[2] ), DT_FLOAT|DT_SIGNED },
{ "angles[0]", offsetof( event_args_t, angles[0] ), sizeof( ((event_args_t *)0)->angles[0] ), DT_FLOAT|DT_SIGNED },
{ "angles[1]", offsetof( event_args_t, angles[1] ), sizeof( ((event_args_t *)0)->angles[1] ), DT_FLOAT|DT_SIGNED },
{ "angles[2]", offsetof( event_args_t, angles[2] ), sizeof( ((event_args_t *)0)->angles[2] ), DT_FLOAT|DT_SIGNED },
{ "velocity[0]", offsetof( event_args_t, velocity[0] ), sizeof( ((event_args_t *)0)->velocity[0] ), DT_FLOAT|DT_SIGNED },
{ "velocity[1]", offsetof( event_args_t, velocity[1] ), sizeof( ((event_args_t *)0)->velocity[1] ), DT_FLOAT|DT_SIGNED },
{ "velocity[2]", offsetof( event_args_t, velocity[2] ), sizeof( ((event_args_t *)0)->velocity[2] ), DT_FLOAT|DT_SIGNED },
{ "ducking", offsetof( event_args_t, ducking ), sizeof( ((event_args_t *)0)->ducking ), DT_INTEGER|DT_SIGNED },
{ "fparam1", offsetof( event_args_t, fparam1 ), sizeof( ((event_args_t *)0)->fparam1 ), DT_FLOAT|DT_SIGNED },
{ "fparam2", offsetof( event_args_t, fparam2 ), sizeof( ((event_args_t *)0)->fparam2 ), DT_FLOAT|DT_SIGNED },
{ "iparam1", offsetof( event_args_t, iparam1 ), sizeof( ((event_args_t *)0)->iparam1 ), DT_INTEGER|DT_SIGNED },
{ "iparam2", offsetof( event_args_t, iparam2 ), sizeof( ((event_args_t *)0)->iparam2 ), DT_INTEGER|DT_SIGNED },
{ "bparam1", offsetof( event_args_t, bparam1 ), sizeof( ((event_args_t *)0)->bparam1 ), DT_INTEGER|DT_SIGNED },
{ "bparam2", offsetof( event_args_t, bparam2 ), sizeof( ((event_args_t *)0)->bparam2 ), DT_INTEGER|DT_SIGNED },
};

static const delta_field_t wd_fields[] =
{
{ "m_iId", offsetof( weapon_data_t, m_iId ), sizeof( ((weapon_data_t *)0)->m_iId ), DT_INTEGER|DT_SIGNED },
{ "m_iClip", offsetof( weapon_data_t, m_iClip ), sizeof( ((weapon_data_t *)0)->m_iClip ), DT_INTEGER|DT_SIGNED },
{ "m_flNextPrimaryAttack", offsetof( weapon_data_t, m_flNextPrimaryAttack ), sizeof( ((weapon_data_t *)0)->m_flNextPrimaryAttack ), DT_FLOAT|DT_SIGNED },
{ "m_flNextSecondaryAttack", offsetof( weapon_data_t, m_flNextSecondaryAttack ), sizeof( ((weapon_data_t *)0)->m_flNextSecondaryAttack ), DT_FLOAT|DT_SIGNED },
{ "m_flTimeWeaponIdle", offsetof( weapon_data_t, m_flTimeWeaponIdle ), sizeof( ((weapon_data_t *)0)->m_flTimeWeaponIdle ), DT_FLOAT|DT_SIGNED },
{ "m_fInReload", offsetof( weapon_data_t, m_fInReload ), sizeof( ((weapon_data_t *)0)->m_fInReload ), DT_INTEGER|DT_SIGNED },
{ "m_fInSpecialReload", offsetof( weapon_data_t, m_fInSpecialReload ), sizeof( ((weapon_data_t *)0)->m_fInSpecialReload ), DT_INTEGER|DT_SIGNED },
{ "m_flNextReload", offsetof( weapon_data_t, m_flNextReload ), sizeof( ((weapon_data_t *)0)->m_flNextReload ), DT_FLOAT|DT_SIGNED },
{ "m_flPumpTime", offsetof( weapon_data_t, m_flPumpTime ), sizeof( ((weapon_data_t *)0)->m_flPumpTime ), DT_FLOAT|DT_SIGNED },
{ "m_fReloadTime", offsetof( weapon_data_t, m_fReloadTime ), sizeof( ((weapon_data_t *)0)->m_fReloadTime ), DT_FLOAT|DT_SIGNED },
{ "m_fAimedDamage", offsetof( weapon_data_t, m_fAimedDamage ), sizeof( ((weapon_data_t *)0)->m_fAimedDamage ), DT_FLOAT|DT_SIGNED },
{ "m_fNextAimBonus", offsetof( weapon_data_t, m_fNextAimBonus ), sizeof( ((weapon_data_t *)0)->m_fNextAimBonus ), DT_FLOAT|DT_SIGNED },
{ "m_fInZoom", offsetof( weapon_data_t, m_fInZoom ), sizeof( ((weapon_data_t *)0)->m_fInZoom ), DT_INTEGER|DT_SIGNED },
{ "m_iWeaponState", offsetof( weapon_data_t, m_iWeaponState ), sizeof( ((weapon_data_t *)0)->m_iWeaponState ), DT_INTEGER|DT_SIGNED },
{ "iuser1", offsetof( weapon_data_t, iuser1 ), sizeof( ((weapon_data_t *)0)->iuser1 ), DT_INTEGER|DT_SIGNED },
{ "iuser2", offsetof( weapon_data_t, iuser2 ), sizeof( ((weapon_data_t *)0)->iuser2 ), DT_INTEGER|DT_SIGNED },
{ "iuser3", offsetof( weapon_data_t, iuser3 ), sizeof( ((weapon_data_t *)0)->iuser3 ), DT_INTEGER|DT_SIGNED },
{ "iuser4", offsetof( weapon_data_t, iuser4 ), sizeof( ((weapon_data_t *)0)->iuser4 ), DT_INTEGER|DT_SIGNED },
{ "fuser1", offsetof( weapon_data_t, fuser1 ), sizeof( ((weapon_data_t *)0)->fuser1 ), DT_FLOAT|DT_SIGNED },
{ "fuser2", offsetof( weapon_data_t, fuser2 ), sizeof( ((weapon_data_t *)0)->fuser2 ), DT_FLOAT|DT_SIGNED },
{ "fuser3", offsetof( weapon_data_t, fuser3 ), sizeof( ((weapon_data_t *)0)->fuser3 ), DT_FLOAT|DT_SIGNED },
{ "fuser4", offsetof( weapon_data_t, fuser4 ), sizeof( ((weapon_data_t *)0)->fuser4 ), DT_FLOAT|DT_SIGNED },
};

static const delta_field_t cd_fields[] =
{
{ "origin[0]", offsetof( clientdata_t, origin[0] ), sizeof( ((clientdata_t *)0)->origin[0] ), DT_FLOAT|DT_SIGNED },
{ "origin[1]", offsetof( clientdata_t, origin[1] ), sizeof( ((clientdata_t *)0)->origin[1] ), DT_FLOAT|DT_SIGNED },
{ "origin[2]", offsetof( clientdata_t, origin[2] ), sizeof( ((clientdata_t *)0)->origin[2] ), DT_FLOAT|DT_SIGNED },
{ "velocity[0]", offsetof( clientdata_t, velocity[0] ), sizeof( ((clientdata_t *)0)->velocity[0] ), DT_FLOAT|DT_SIGNED },
{ "velocity[1]", offsetof( clientdata_t, velocity[1] ), sizeof( ((clientdata_t *)0)->velocity[1] ), DT_FLOAT|DT_SIGNED },
{ "velocity[2]", offsetof( clientdata_t, velocity[2] ), sizeof( ((clientdata_t *)0)->velocity[2] ), DT_FLOAT|DT_SIGNED },
{ "viewmodel", offsetof( clientdata_t, viewmodel ), sizeof( ((clientdata_t *)0)->viewmodel ), DT_INTEGER|DT_SIGNED },
{ "punchangle[0]", offsetof( clientdata_t, punchangle[0] ), sizeof( ((clientdata_t *)0)->punchangle[0] ), DT_FLOAT|DT_SIGNED },
{ "punchangle[1]", offsetof( clientdata_t, punchangle[1] ), sizeof( ((clientdata_t *)0)->punchangle[1] ), DT_FLOAT|DT_SIGNED },
{ "punchangle[2]", offsetof( clientdata_t, punchangle[2] ), sizeof( ((clientdata_t *)0)->punchangle[2] ), DT_FLOAT|DT_SIGNED },
{ "flags", offsetof( clientdata_t, flags ), sizeof( ((clientdata_t *)0)->flags ), DT_INTEGER|DT_SIGNED },
{ "waterlevel", offsetof( clientdata_t, waterlevel ), sizeof( ((clientdata_t *)0)->waterlevel ), DT_INTEGER|DT_SIGNED },
{ "watertype", offsetof( clientdata_t, watertype ), sizeof( ((clientdata_t *)0)->watertype ), DT_INTEGER|DT_SIGNED },
{ "view_ofs[0]", offsetof( clientdata_t, view_ofs[0] ), sizeof( ((clientdata_t *)0)->view_ofs[0] ), DT_FLOAT|DT_SIGNED },
{ "view_ofs[1]", offsetof( clientdata_t, view_ofs[1] ), sizeof( ((clientdata_t *)0)->view_ofs[1] ), DT_FLOAT|DT_SIGNED },
{ "view_ofs[2]", offsetof( clientdata_t, view_ofs[2] ), sizeof( ((clientdata_t *)0)->view_ofs[2] ), DT_FLOAT|DT_SIGNED },
{ "health", offsetof( clientdata_t, health ), sizeof( ((clientdata_t *)0)->health ), DT_FLOAT|DT_SIGNED },
{ "bInDuck", offsetof( clientdata_t, bInDuck ), sizeof( ((clientdata_t *)0)->bInDuck ), DT_INTEGER|DT_SIGNED },
{ "weapons", offsetof( clientdata_t, weapons ), sizeof( ((clientdata_t *)0)->weapons ), DT_INTEGER|DT_SIGNED },
{ "flTimeStepSound", offsetof( clientdata_t, flTimeStepSound ), sizeof( ((clientdata_t *)0)->flTimeStepSound ), DT_INTEGER|DT_SIGNED },
{ "flDuckTime", offsetof( clientdata_t, flDuckTime ), sizeof( ((clientdata_t *)0)->flDuckTime ), DT_INTEGER|DT_SIGNED },
{ "flSwimTime", offsetof( clientdata_t, flSwimTime ), sizeof( ((clientdata_t *)0)->flSwimTime ), DT_INTEGER|DT_SIGNED },
{ "waterjumptime", offsetof( clientdata_t, waterjumptime ), sizeof( ((clientdata_t *)0)->waterjumptime ), DT_INTEGER|DT_SIGNED },
{ "maxspeed", offsetof( clientdata_t, maxspeed ), sizeof( ((clientdata_t *)0)->maxspeed ), DT_FLOAT|DT_SIGNED },
{ "fov", offsetof( clientdata_t, fov ), sizeof( ((clientdata_t *)0)->fov ), DT_FLOAT|DT_SIGNED },
{ "weaponanim", offsetof( clientdata_t, weaponanim ), sizeof( ((clientdata_t *)0)->weaponanim ), DT_INTEGER|DT_SIGNED },
{ "m_iId", offsetof( clientdata_t, m_iId ), sizeof( ((clientdata_t *)0)->m_iId ), DT_INTEGER|DT_SIGNED },
{ "ammo_shells", offsetof( clientdata_t, ammo_shells ), sizeof( ((clientdata_t *)0)->ammo_shells ), DT_INTEGER|DT_SIGNED },
{ "ammo_nails", offsetof( clientdata_t, ammo_nails ), sizeof( ((clientdata_t *)0)->ammo_nails ), DT_INTEGER|DT_SIGNED },
{ "ammo_cells", offsetof( clientdata_t, ammo_cells ), sizeof( ((clientdata_t *)0)->ammo_cells ), DT_INTEGER|DT_SIGNED },
{ "ammo_rockets", offsetof( clientdata_t, ammo_rockets ), sizeof( ((clientdata_t *)0)->ammo_rockets ), DT_INTEGER|DT_SIGNED },
{ "m_flNextAttack", offsetof( clientdata_t, m_flNextAttack ), sizeof( ((clientdata_t *)0)->m_flNextAttack ), DT_FLOAT|DT_SIGNED },
{ "tfstate", offsetof( clientdata_t, tfstate ), sizeof( ((clientdata_t *)0)->tfstate ), DT_INTEGER|DT_SIGNED },
{ "pushmsec", offsetof( clientdata_t, pushmsec ), sizeof( ((clientdata_t *)0)->pushmsec ), DT_INTEGER|DT_SIGNED },
{ "deadflag", offsetof( clientdata_t, deadflag ), sizeof( ((clientdata_t *)0)->deadflag ), DT_INTEGER|DT_SIGNED },
{ "physinfo", offsetof( clientdata_t, physinfo ), sizeof( ((clientdata_t *)0)->physinfo ), DT_STRING },
{ "iuser1", offsetof( clientdata_t, iuser1 ), sizeof( ((clientdata_t *)0)->iuser1 ), DT_INTEGER|DT_SIGNED },
{ "iuser2", offsetof( clientdata_t, iuser2 ), sizeof( ((clientdata_t *)0)->iuser2 ), DT_INTEGER|DT_SIGNED },
{ "iuser3", offsetof( clientdata_t, iuser3 ), sizeof( ((clientdata_t *)0)->iuser3 ), DT_INTEGER|DT_SIGNED },
{ "iuser4", offsetof( clientdata_t, iuser4 ), sizeof( ((clientdata_t *)0)->iuser4 ), DT_INTEGER|DT_SIGNED },
{ "fuser1", offsetof( clientdata_t, fuser1 ), sizeof( ((clientdata_t *)0)->fuser1 ), DT_FLOAT|DT_SIGNED },
{ "fuser2", offsetof( clientdata_t, fuser2 ), sizeof( ((clientdata_t *)0)->fuser2 ), DT_FLOAT|DT_SIGNED },
{ "fuser3", offsetof( clientdata_t, fuser3 ), sizeof( ((clientdata_t *)0)->fuser3 ), DT_FLOAT|DT_SIGNED },
{ "fuser4", offsetof( clientdata_t, fuser4 ), sizeof( ((clientdata_t *)0)->fuser4 ), DT_FLOAT|DT_SIGNED },
{ "vuser1[0]", offsetof( clientdata_t, vuser1[0] ), sizeof( ((clientdata_t *)0)->vuser1[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser1[1]", offsetof( clientdata_t, vuser1[1] ), sizeof( ((clientdata_t *)0)->vuser1[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser1[2]", offsetof( clientdata_t, vuser1[2] ), sizeof( ((clientdata_t *)0)->vuser1[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[0]", offsetof( clientdata_t, vuser2[0] ), sizeof( ((clientdata_t *)0)->vuser2[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[1]", offsetof( clientdata_t, vuser2[1] ), sizeof( ((clientdata_t *)0)->vuser2[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[2]", offsetof( clientdata_t, vuser2[2] ), sizeof( ((clientdata_t *)0)->vuser2[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[0]", offsetof( clientdata_t, vuser3[0] ), sizeof( ((clientdata_t *)0)->vuser3[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[1]", offsetof( clientdata_t, vuser3[1] ), sizeof( ((clientdata_t *)0)->vuser3[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[2]", offsetof( clientdata_t, vuser3[2] ), sizeof( ((clientdata_t *)0)->vuser3[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[0]", offsetof( clientdata_t, vuser4[0] ), sizeof( ((clientdata_t *)0)->vuser4[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[1]", offsetof( clientdata_t, vuser4[1] ), sizeof( ((clientdata_t *)0)->vuser4[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[2]", offsetof( clientdata_t, vuser4[2] ), sizeof( ((clientdata_t *)0)->vuser4[2] ), DT_FLOAT|DT_SIGNED },
};

static const delta_field_t ent_fields[] =
{
{ "entityType", offsetof( entity_state_t, entityType ), sizeof( ((entity_state_t *)0)->entityType ), DT_INTEGER|DT_SIGNED },
{ "origin[0]", offsetof( entity_state_t, origin[0] ), sizeof( ((entity_state_t *)0)->origin[0] ), DT_FLOAT|DT_SIGNED },
{ "origin[1]", offsetof( entity_state_t, origin[1] ), sizeof( ((entity_state_t *)0)->origin[1] ), DT_FLOAT|DT_SIGNED },
{ "origin[2]", offsetof( entity_state_t, origin[2] ), sizeof( ((entity_state_t *)0)->origin[2] ), DT_FLOAT|DT_SIGNED },
{ "angles[0]", offsetof( entity_state_t, angles[0] ), sizeof( ((entity_state_t *)0)->angles[0] ), DT_FLOAT|DT_SIGNED },
{ "angles[1]", offsetof( entity_state_t, angles[1] ), sizeof( ((entity_state_t *)0)->angles[1] ), DT_FLOAT|DT_SIGNED },
{ "angles[2]", offsetof( entity_state_t, angles[2] ), sizeof( ((entity_state_t *)0)->angles[2] ), DT_FLOAT|DT_SIGNED },
{ "modelindex", offsetof( entity_state_t, modelindex ), sizeof( ((entity_state_t *)0)->modelindex ), DT_INTEGER|DT_SIGNED },
{ "sequence", offsetof( entity_state_t, sequence ), sizeof( ((entity_state_t *)0)->sequence ), DT_INTEGER|DT_SIGNED },
{ "frame", offsetof( entity_state_t, frame ), sizeof( ((entity_state_t *)0)->frame ), DT_FLOAT|DT_SIGNED },
{ "colormap", offsetof( entity_state_t, colormap ), sizeof( ((entity_state_t *)0)->colormap ), DT_INTEGER|DT_SIGNED },
{ "skin", offsetof( entity_state_t, skin ), sizeof( ((entity_state_t *)0)->skin ), DT_SHORT|DT_SIGNED },
{ "solid", offsetof( entity_state_t, solid ), sizeof( ((entity_state_t *)0)->solid ), DT_SHORT|DT_SIGNED },
{ "effects", offsetof( entity_state_t, effects ), sizeof( ((entity_state_t *)0)->effects ), DT_INTEGER|DT_SIGNED },
{ "scale", offsetof( entity_state_t, scale ), sizeof( ((entity_state_t *)0)->scale ), DT_FLOAT|DT_SIGNED },
{ "eflags", offsetof( entity_state_t, eflags ), sizeof( ((entity_state_t *)0)->eflags ), DT_BYTE },
{ "rendermode", offsetof( entity_state_t, rendermode ), sizeof( ((entity_state_t *)0)->rendermode ), DT_INTEGER|DT_SIGNED },
{ "renderamt", offsetof( entity_state_t, renderamt ), sizeof( ((entity_state_t *)0)->renderamt ), DT_INTEGER|DT_SIGNED },
{ "rendercolor.r", offsetof( entity_state_t, rendercolor.r ), sizeof( ((entity_state_t *)0)->rendercolor.r ), DT_BYTE },
{ "rendercolor.g", offsetof( entity_state_t, rendercolor.g ), sizeof( ((entity_state_t *)0)->rendercolor.g ), DT_BYTE },
{ "rendercolor.b", offsetof( entity_state_t, rendercolor.b ), sizeof( ((entity_state_t *)0)->rendercolor.b ), DT_BYTE },
{ "renderfx", offsetof( entity_state_t, renderfx ), sizeof( ((entity_state_t *)0)->renderfx ), DT_INTEGER|DT_SIGNED },
{ "movetype", offsetof( entity_state_t, movetype ), sizeof( ((entity_state_t *)0)->movetype ), DT_INTEGER|DT_SIGNED },
{ "animtime", offsetof( entity_state_t, animtime ), sizeof( ((entity_state_t *)0)->animtime ), DT_FLOAT|DT_SIGNED },
{ "framerate", offsetof( entity_state_t, framerate ), sizeof( ((entity_state_t *)0)->framerate ), DT_FLOAT|DT_SIGNED },
{ "body", offsetof( entity_state_t, body ), sizeof( ((entity_state_t *)0)->body ), DT_INTEGER|DT_SIGNED },
{ "controller[0]", offsetof( entity_state_t, controller[0] ), sizeof( ((entity_state_t *)0)->controller[0] ), DT_BYTE },
{ "controller[1]", offsetof( entity_state_t, controller[1] ), sizeof( ((entity_state_t *)0)->controller[1] ), DT_BYTE },
{ "controller[2]", offsetof( entity_state_t, controller[2] ), sizeof( ((entity_state_t *)0)->controller[2] ), DT_BYTE },
{ "controller[3]", offsetof( entity_state_t, controller[3] ), sizeof( ((entity_state_t *)0)->controller[3] ), DT_BYTE },
{ "blending[0]", offsetof( entity_state_t, blending[0] ), sizeof( ((entity_state_t *)0)->blending[0] ), DT_BYTE },
{ "blending[1]", offsetof( entity_state_t, blending[1] ), sizeof( ((entity_state_t *)0)->blending[1] ), DT_BYTE },
{ "blending[2]", offsetof( entity_state_t, blending[2] ), sizeof( ((entity_state_t *)0)->blending[2] ), DT_BYTE },
{ "blending[3]", offsetof( entity_state_t, blending[3] ), sizeof( ((entity_state_t *)0)->blending[3] ), DT_BYTE },
{ "velocity[0]", offsetof( entity_state_t, velocity[0] ), sizeof( ((entity_state_t *)0)->velocity[0] ), DT_FLOAT|DT_SIGNED },
{ "velocity[1]", offsetof( entity_state_t, velocity[1] ), sizeof( ((entity_state_t *)0)->velocity[1] ), DT_FLOAT|DT_SIGNED },
{ "velocity[2]", offsetof( entity_state_t, velocity[2] ), sizeof( ((entity_state_t *)0)->velocity[2] ), DT_FLOAT|DT_SIGNED },
{ "mins[0]", offsetof( entity_state_t, mins[0] ), sizeof( ((entity_state_t *)0)->mins[0] ), DT_FLOAT|DT_SIGNED },
{ "mins[1]", offsetof( entity_state_t, mins[1] ), sizeof( ((entity_state_t *)0)->mins[1] ), DT_FLOAT|DT_SIGNED },
{ "mins[2]", offsetof( entity_state_t, mins[2] ), sizeof( ((entity_state_t *)0)->mins[2] ), DT_FLOAT|DT_SIGNED },
{ "maxs[0]", offsetof( entity_state_t, maxs[0] ), sizeof( ((entity_state_t *)0)->maxs[0] ), DT_FLOAT|DT_SIGNED },
{ "maxs[1]", offsetof( entity_state_t, maxs[1] ), sizeof( ((entity_state_t *)0)->maxs[1] ), DT_FLOAT|DT_SIGNED },
{ "maxs[2]", offsetof( entity_state_t, maxs[2] ), sizeof( ((entity_state_t *)0)->maxs[2] ), DT_FLOAT|DT_SIGNED },
{ "aiment", offsetof( entity_state_t, aiment ), sizeof( ((entity_state_t *)0)->aiment ), DT_INTEGER|DT_SIGNED },
{ "owner", offsetof( entity_state_t, owner ), sizeof( ((entity_state_t *)0)->owner ), DT_INTEGER|DT_SIGNED },
{ "friction", offsetof( entity_state_t, friction ), sizeof( ((entity_state_t *)0)->friction ), DT_FLOAT|DT_SIGNED },
{ "gravity", offsetof( entity_state_t, gravity ), sizeof( ((entity_state_t *)0)->gravity ), DT_FLOAT|DT_SIGNED },
{ "team", offsetof( entity_state_t, team ), sizeof( ((entity_state_t *)0)->team ), DT_INTEGER|DT_SIGNED },
{ "playerclass", offsetof( entity_state_t, playerclass ), sizeof( ((entity_state_t *)0)->playerclass ), DT_INTEGER|DT_SIGNED },
{ "health", offsetof( entity_state_t, health ), sizeof( ((entity_state_t *)0)->health ), DT_INTEGER|DT_SIGNED },
{ "spectator", offsetof( entity_state_t, spectator ), sizeof( ((entity_state_t *)0)->spectator ), DT_INTEGER|DT_SIGNED },
{ "weaponmodel", offsetof( entity_state_t, weaponmodel ), sizeof( ((entity_state_t *)0)->weaponmodel ), DT_INTEGER|DT_SIGNED },
{ "gaitsequence", offsetof( entity_state_t, gaitsequence ), sizeof( ((entity_state_t *)0)->gaitsequence ), DT_INTEGER|DT_SIGNED },
{ "basevelocity[0]", offsetof( entity_state_t, basevelocity[0] ), sizeof( ((entity_state_t *)0)->basevelocity[0] ), DT_FLOAT|DT_SIGNED },
{ "basevelocity[1]", offsetof( entity_state_t, basevelocity[1] ), sizeof( ((entity_state_t *)0)->basevelocity[1] ), DT_FLOAT|DT_SIGNED },
{ "basevelocity[2]", offsetof( entity_state_t, basevelocity[2] ), sizeof( ((entity_state_t *)0)->basevelocity[2] ), DT_FLOAT|DT_SIGNED },
{ "usehull", offsetof( entity_state_t, usehull ), sizeof( ((entity_state_t *)0)->usehull ), DT_INTEGER|DT_SIGNED },
{ "oldbuttons", offsetof( entity_state_t, oldbuttons ), sizeof( ((entity_state_t *)0)->oldbuttons ), DT_INTEGER|DT_SIGNED },	// probably never transmitted
{ "onground", offsetof( entity_state_t, onground ), sizeof( ((entity_state_t *)0)->onground ), DT_INTEGER|DT_SIGNED },
{ "iStepLeft", offsetof( entity_state_t, iStepLeft ), sizeof( ((entity_state_t *)0)->iStepLeft ), DT_INTEGER|DT_SIGNED },
{ "flFallVelocity", offsetof( entity_state_t, flFallVelocity ), sizeof( ((entity_state_t *)0)->flFallVelocity ), DT_FLOAT|DT_SIGNED },
{ "fov", offsetof( entity_state_t, fov ), sizeof( ((entity_state_t *)0)->fov ), DT_FLOAT|DT_SIGNED },
{ "weaponanim", offsetof( entity_state_t, weaponanim ), sizeof( ((entity_state_t *)0)->weaponanim ), DT_INTEGER|DT_SIGNED },
{ "startpos[0]", offsetof( entity_state_t, startpos[0] ), sizeof( ((entity_state_t *)0)->startpos[0] ), DT_FLOAT|DT_SIGNED },
{ "startpos[1]", offsetof( entity_state_t, startpos[1] ), sizeof( ((entity_state_t *)0)->startpos[1] ), DT_FLOAT|DT_SIGNED },
{ "startpos[2]", offsetof( entity_state_t, startpos[2] ), sizeof( ((entity_state_t *)0)->startpos[2] ), DT_FLOAT|DT_SIGNED },
{ "endpos[0]", offsetof( entity_state_t, endpos[0] ), sizeof( ((entity_state_t *)0)->endpos[0] ), DT_FLOAT|DT_SIGNED },
{ "endpos[1]", offsetof( entity_state_t, endpos[1] ), sizeof( ((entity_state_t *)0)->endpos[1] ), DT_FLOAT|DT_SIGNED },
{ "endpos[2]", offsetof( entity_state_t, endpos[2] ), sizeof( ((entity_state_t *)0)->endpos[2] ), DT_FLOAT|DT_SIGNED },
{ "impacttime", offsetof( entity_state_t, impacttime ), sizeof( ((entity_state_t *)0)->impacttime ), DT_FLOAT|DT_SIGNED },
{ "starttime", offsetof( entity_state_t, starttime ), sizeof( ((entity_state_t *)0)->starttime ), DT_FLOAT|DT_SIGNED },
{ "iuser1", offsetof( entity_state_t, iuser1 ), sizeof( ((entity_state_t *)0)->iuser1 ), DT_INTEGER|DT_SIGNED },
{ "iuser2", offsetof( entity_state_t, iuser2 ), sizeof( ((entity_state_t *)0)->iuser2 ), DT_INTEGER|DT_SIGNED },
{ "iuser3", offsetof( entity_state_t, iuser3 ), sizeof( ((entity_state_t *)0)->iuser3 ), DT_INTEGER|DT_SIGNED },
{ "iuser4", offsetof( entity_state_t, iuser4 ), sizeof( ((entity_state_t *)0)->iuser4 ), DT_INTEGER|DT_SIGNED },
{ "fuser1", offsetof( entity_state_t, fuser1 ), sizeof( ((entity_state_t *)0)->fuser1 ), DT_FLOAT|DT_SIGNED },
{ "fuser2", offsetof( entity_state_t, fuser2 ), sizeof( ((entity_state_t *)0)->fuser2 ), DT_FLOAT|DT_SIGNED },
{ "fuser3", offsetof( entity_state_t, fuser3 ), sizeof( ((entity_state_t *)0)->fuser3 ), DT_FLOAT|DT_SIGNED },
{ "fuser4", offsetof( entity_state_t, fuser4 ), sizeof( ((entity_state_t *)0)->fuser4 ), DT_FLOAT|DT_SIGNED },
{ "vuser1[0]", offsetof( entity_state_t, vuser1[0] ), sizeof( ((entity_state_t *)0)->vuser1[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser1[1]", offsetof( entity_state_t, vuser1[1] ), sizeof( ((entity_state_t *)0)->vuser1[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser1[2]", offsetof( entity_state_t, vuser1[2] ), sizeof( ((entity_state_t *)0)->vuser1[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[0]", offsetof( entity_state_t, vuser2[0] ), sizeof( ((entity_state_t *)0)->vuser2[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[1]", offsetof( entity_state_t, vuser2[1] ), sizeof( ((entity_state_t *)0)->vuser2[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser2[2]", offsetof( entity_state_t, vuser2[2] ), sizeof( ((entity_state_t *)0)->vuser2[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[0]", offsetof( entity_state_t, vuser3[0] ), sizeof( ((entity_state_t *)0)->vuser3[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[1]", offsetof( entity_state_t, vuser3[1] ), sizeof( ((entity_state_t *)0)->vuser3[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser3[2]", offsetof( entity_state_t, vuser3[2] ), sizeof( ((entity_state_t *)0)->vuser3[2] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[0]", offsetof( entity_state_t, vuser4[0] ), sizeof( ((entity_state_t *)0)->vuser4[0] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[1]", offsetof( entity_state_t, vuser4[1] ), sizeof( ((entity_state_t *)0)->vuser4[1] ), DT_FLOAT|DT_SIGNED },
{ "vuser4[2]", offsetof( entity_state_t, vuser4[2] ), sizeof( ((entity_state_t *)0)->vuser4[2] ), DT_FLOAT|DT_SIGNED },
};

static const delta_field_t meta_fields[] =
{
{ "fieldType", offsetof( goldsrc_delta_t, fieldType ), sizeof( ((goldsrc_delta_t *)0)->fieldType ), DT_INTEGER|DT_SIGNED },
{ "fieldName", offsetof( goldsrc_delta_t, fieldName ), sizeof( ((goldsrc_delta_t *)0)->fieldName ), DT_STRING },
{ "fieldOffset", offsetof( goldsrc_delta_t, fieldOffset ), sizeof( ((goldsrc_delta_t *)0)->fieldOffset ), DT_INTEGER|DT_SIGNED },
{ "fieldSize", offsetof( goldsrc_delta_t, fieldSize ), sizeof( ((goldsrc_delta_t *)0)->fieldSize ), DT_SHORT|DT_SIGNED },
{ "significant_bits", offsetof( goldsrc_delta_t, significant_bits ), sizeof( ((goldsrc_delta_t *)0)->significant_bits ), DT_INTEGER|DT_SIGNED },
{ "premultiply", offsetof( goldsrc_delta_t, premultiply ), sizeof( ((goldsrc_delta_t *)0)->premultiply ), DT_FLOAT|DT_SIGNED },
{ "postmultiply", offsetof( goldsrc_delta_t, postmultiply ), sizeof( ((goldsrc_delta_t *)0)->postmultiply ), DT_FLOAT|DT_SIGNED },
};

#if XASH_ENGINE_TESTS
typedef struct delta_test_struct_t
{
	char     dt_string[128];    // always signed
	float    dt_timewindow_big; // always signed
	float    dt_timewindow_8;   // always signed
	float	 dt_angle;          // always_signed
	float    dt_float_signed;
	float    dt_float_unsigned;
	int32_t  dt_integer_signed;
	uint32_t dt_integer_unsigned;
	int16_t  dt_short_signed;
	uint16_t dt_short_unsigned;
	int8_t   dt_byte_signed;
	uint8_t  dt_byte_unsigned;
	int32_t  dt_integer_signed_mul;
	int16_t  dt_short_signed_mul;
	int8_t   dt_byte_signed_mul;
} delta_test_struct_t;

#define TEST_DEF( x )	#x, offsetof( delta_test_struct_t, x ), sizeof( ((delta_test_struct_t *)0)->x )

static const delta_field_t test_fields[] =
{
{ TEST_DEF( dt_string ) },
{ TEST_DEF( dt_timewindow_big )},
{ TEST_DEF( dt_timewindow_8 )},
{ TEST_DEF( dt_angle ) },
{ TEST_DEF( dt_float_signed ) },
{ TEST_DEF( dt_float_unsigned ) },
{ TEST_DEF( dt_integer_signed ) },
{ TEST_DEF( dt_integer_unsigned ) },
{ TEST_DEF( dt_short_signed ) },
{ TEST_DEF( dt_short_unsigned ) },
{ TEST_DEF( dt_byte_signed ) },
{ TEST_DEF( dt_byte_unsigned ) },
{ TEST_DEF( dt_integer_signed_mul ) },
{ TEST_DEF( dt_short_signed_mul ) },
{ TEST_DEF( dt_byte_signed_mul ) },
};
#endif

static delta_info_t dt_info[] =
{
[DT_EVENT_T]               = { "event_t", ev_fields, ARRAYSIZE( ev_fields ) },
[DT_MOVEVARS_T]            = { "movevars_t", pm_fields, ARRAYSIZE( pm_fields ) },
[DT_USERCMD_T]             = { "usercmd_t", cmd_fields, ARRAYSIZE( cmd_fields ) },
[DT_CLIENTDATA_T]          = { "clientdata_t", cd_fields, ARRAYSIZE( cd_fields ) },
[DT_WEAPONDATA_T]          = { "weapon_data_t", wd_fields, ARRAYSIZE( wd_fields ) },
[DT_ENTITY_STATE_T]        = { "entity_state_t", ent_fields, ARRAYSIZE( ent_fields ) },
[DT_ENTITY_STATE_PLAYER_T] = { "entity_state_player_t", ent_fields, ARRAYSIZE( ent_fields ) },
[DT_CUSTOM_ENTITY_STATE_T] = { "custom_entity_state_t", ent_fields, ARRAYSIZE( ent_fields ) },
#if XASH_ENGINE_TESTS
[DT_DELTA_TEST_STRUCT_T]   = { "delta_test_struct_t", test_fields, ARRAYSIZE( test_fields ) },
#endif
};

// meta description is special, it cannot be overriden
static const delta_info_t dt_goldsrc_meta =
{
	.pName = "goldsrc_delta_t",
	.pInfo = meta_fields,
	.maxFields = ARRAYSIZE( meta_fields ),
	.numFields = ARRAYSIZE( meta_fields ),
	.pFields = (delta_t[ARRAYSIZE( meta_fields )])
	{
		{
			"fieldType", offsetof( goldsrc_delta_t, fieldType ), sizeof( ((goldsrc_delta_t *)0)->fieldType ), DT_INTEGER|DT_SIGNED,
			.flags = DT_INTEGER,
			.multiplier = 1.0f,
			.post_multiplier = 1.0f,
			.bits = 32,
		},
		{
			"fieldName", offsetof( goldsrc_delta_t, fieldName ), sizeof( ((goldsrc_delta_t *)0)->fieldName ), DT_STRING,
			.flags = DT_STRING,
			.multiplier = 1.0f,
			.post_multiplier = 1.0f,
			.bits = 1,
		},
		{
			"fieldOffset", offsetof( goldsrc_delta_t, fieldOffset ), sizeof( ((goldsrc_delta_t *)0)->fieldOffset ), DT_INTEGER|DT_SIGNED,
			.flags = DT_INTEGER,
			.multiplier = 1.0f,
			.post_multiplier = 1.0f,
			.bits = 16,
		},
		{
			"fieldSize", offsetof( goldsrc_delta_t, fieldSize ), sizeof( ((goldsrc_delta_t *)0)->fieldSize ), DT_SHORT|DT_SIGNED,
			.flags = DT_INTEGER,
			.multiplier = 1.0f,
			.post_multiplier = 1.0f,
			.bits = 8,
		},
		{
			"significant_bits", offsetof( goldsrc_delta_t, significant_bits ), sizeof( ((goldsrc_delta_t *)0)->significant_bits ), DT_INTEGER|DT_SIGNED,
			.flags = DT_INTEGER,
			.multiplier = 1.0f,
			.post_multiplier = 1.0f,
			.bits = 8,
		},
		{
			"premultiply", offsetof( goldsrc_delta_t, premultiply ), sizeof( ((goldsrc_delta_t *)0)->premultiply ), DT_FLOAT|DT_SIGNED,
			.flags = DT_FLOAT,
			.multiplier = 4000.0f,
			.post_multiplier = 1.0f,
			.bits = 32,
		},
		{
			"postmultiply", offsetof( goldsrc_delta_t, postmultiply ), sizeof( ((goldsrc_delta_t *)0)->postmultiply ), DT_FLOAT|DT_SIGNED,
			.flags = DT_FLOAT,
			.multiplier = 4000.0f,
			.post_multiplier = 1.0f,
			.bits = 32,
		},
	},
	.bInitialized = true
};

static delta_info_t *Delta_FindStruct( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	for( int i = 0; i < ARRAYSIZE( dt_info ); i++ )
	{
		if( !Q_stricmp( dt_info[i].pName, name ))
			return &dt_info[i];
	}

	// NOTE: no warning here on purpose. This lookup doubles as a
	// struct-or-field probe while parsing server delta tables (each field
	// name misses first, then resolves as a field), so warning per miss
	// spammed ~100 lines per connect. Genuine failures are still loud:
	// Delta_InitFields Sys_Errors on unknown structs, and unknown wire
	// fields log in the GS table parser.
	// found nothing
	return NULL;
}

static int Delta_NumTables( void )
{
	return ARRAYSIZE( dt_info );
}

static delta_info_t *Delta_FindStructByIndex( int index )
{
	if( index < 0 || index >= ARRAYSIZE( dt_info ))
		return NULL;
	return &dt_info[index];
}

static delta_info_t *Delta_FindStructByEncoder( const char *encoderName )
{
	if( COM_StringEmptyOrNULL( encoderName ) )
		return NULL;

	for( int i = 0; i < ARRAYSIZE( dt_info ); i++ )
	{
		if( !Q_stricmp( dt_info[i].funcName, encoderName ))
			return &dt_info[i];
	}
	// found nothing
	return NULL;
}

static delta_info_t *Delta_FindStructByDelta( const delta_t *pFields )
{
	if( !pFields ) return NULL;

	for( int i = 0; i < ARRAYSIZE( dt_info ); i++ )
	{
		if( dt_info[i].pFields == pFields )
			return &dt_info[i];
	}
	// found nothing
	return NULL;
}

static void Delta_CustomEncode( delta_info_t *dt, const void *from, const void *to )
{
	Assert( dt != NULL );

	// set all fields is active by default
	for( int i = 0; i < dt->numFields; i++ )
		dt->pFields[i].bInactive = false;

	if( dt->userCallback )
		dt->userCallback( dt->pFields, from, to );
}

static const delta_field_t *Delta_FindFieldInfo( const delta_field_t *pInfo, const char *fieldName, int maxFields )
{
	if( !fieldName || !*fieldName )
		return NULL;

	for( int i = 0; i < maxFields; i++ )
	{
		if( !Q_strcmp( pInfo[i].name, fieldName ))
			return &pInfo[i];
	}

	return NULL;
}

static int Delta_IndexForFieldInfo( const delta_field_t *pInfo, const char *fieldName, int maxFields )
{
	if( !fieldName || !*fieldName )
		return -1;

	for( int i = 0; i < maxFields; i++ )
	{
		if( !Q_strcmp( pInfo[i].name, fieldName ))
			return i;
	}
	return -1;
}

static qboolean Delta_AddField( delta_info_t *dt, const char *pName, int flags, int bits, float mul, float post_mul )
{
	delta_t *pField;
	int i;

	// check for coexisting field
	for( i = 0, pField = dt->pFields; i < dt->numFields && pField; i++, pField++ )
	{
		if( !Q_strcmp( pField->name, pName ))
		{
			// update existed field
			pField->flags = flags;
			pField->bits = bits;
			pField->multiplier = mul;
			pField->post_multiplier = post_mul;
			return true;
		}
	}

	// find field description
	const delta_field_t *pFieldInfo = Delta_FindFieldInfo( dt->pInfo, pName, dt->maxFields );
	if( !pFieldInfo )
	{
		Con_Printf( S_ERROR "%s: couldn't find description for %s->%s (numFields=%d maxFields=%d)\n",
			__func__, dt->pName, pName ? pName : "(null)", dt->numFields, dt->maxFields );
		return false;
	}

	if( dt->numFields + 1 > dt->maxFields )
	{
		Con_DPrintf( S_WARN "%s: can't add %s->%s encoder list is full\n", __func__, dt->pName, pName );
		return false; // too many fields specified (duplicated ?)
	}

	// allocate a new one
	dt->pFields = Z_Realloc( dt->pFields, (dt->numFields + 1) * sizeof( delta_t ));
	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ );

	// copy info to new field
	pField->name = pFieldInfo->name;
	pField->offset = pFieldInfo->offset;
	pField->size = pFieldInfo->size;
	pField->flags = flags;
	pField->bits = bits;
	pField->multiplier = mul;
	pField->post_multiplier = post_mul;
	dt->numFields++;

	return true;
}

static void Delta_WriteTableField( sizebuf_t *msg, int tableIndex, const delta_t *pField )
{
	Assert( pField != NULL );

	if( COM_StringEmptyOrNULL( pField->name ))
		return;// not initialized ?

	delta_info_t *dt = Delta_FindStructByIndex( tableIndex );
	Assert( dt && dt->bInitialized );

	int nameIndex = Delta_IndexForFieldInfo( dt->pInfo, pField->name, dt->maxFields );
	Assert( nameIndex >= 0 && nameIndex < dt->maxFields );

	MSG_BeginServerCmd( msg, svc_deltatable );
	MSG_WriteUBitLong( msg, tableIndex, 4 ); // assume we support 16 network tables
	MSG_WriteUBitLong( msg, nameIndex, 8 ); // 255 fields by struct should be enough
	MSG_WriteUBitLong( msg, pField->flags, 10 ); // flags are indicated various input types
	MSG_WriteUBitLong( msg, pField->bits - 1, 5 ); // max received value is 32 (32 bit)

	// multipliers is null-compressed
	if( !Q_equal( pField->multiplier, 1.0f ))
	{
		MSG_WriteOneBit( msg, 1 );
		MSG_WriteFloat( msg, pField->multiplier );
	}
	else MSG_WriteOneBit( msg, 0 );

	if( !Q_equal( pField->post_multiplier, 1.0f ))
	{
		MSG_WriteOneBit( msg, 1 );
		MSG_WriteFloat( msg, pField->post_multiplier );
	}
	else MSG_WriteOneBit( msg, 0 );
}

void Delta_ParseTableField( sizebuf_t *msg )
{
	float mul = 1.0f, post_mul = 1.0f;
	const char *pName;
	qboolean ignore = false;

	int tableIndex = MSG_ReadUBitLong( msg, 4 );
	delta_info_t *dt = Delta_FindStructByIndex( tableIndex );
	if( !dt )
		Host_Error( "%s: not initialized", __func__ );

	int nameIndex = MSG_ReadUBitLong( msg, 8 );	// read field name index
	if( ( nameIndex >= 0 && nameIndex < dt->maxFields ) )
	{
		pName = dt->pInfo[nameIndex].name;
	}
	else
	{
		ignore = true;
		Con_Reportf( "%s: wrong nameIndex %d for table %s, ignoring\n", __func__, nameIndex,  dt->pName );
	}

	int flags = MSG_ReadUBitLong( msg, 10 );
	int bits = MSG_ReadUBitLong( msg, 5 ) + 1;

	// read the multipliers
	if( MSG_ReadOneBit( msg ))
		mul = MSG_ReadFloat( msg );

	if( MSG_ReadOneBit( msg ))
		post_mul = MSG_ReadFloat( msg );

	if( ignore )
		return;

	// delta encoders it's already initialized on this machine (local game)
	if( delta_init )
		Delta_Shutdown();

	// add field to table
	Delta_AddField( dt, pName, flags, bits, mul, post_mul );
}

static qboolean Delta_ParseField( char **delta_script, const delta_info_t *dt, delta_t *pField, qboolean bPost )
{
	string token;

	*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
	if( Q_strcmp( token, "(" ))
	{
		Con_DPrintf( S_ERROR "%s: expected '(', found '%s' instead\n", __func__, token );
		return false;
	}

	// read the variable name
	if(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) == NULL )
	{
		Con_DPrintf( S_ERROR "%s: missing field name\n", __func__ );
		return false;
	}

	const delta_field_t *pFieldInfo = Delta_FindFieldInfo( dt->pInfo, token, dt->maxFields );
	if( !pFieldInfo )
	{
		Con_DPrintf( S_ERROR "%s: unable to find field %s\n", __func__, token );
		return false;
	}

	*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
	if( Q_strcmp( token, "," ))
	{
		Con_DPrintf( S_ERROR "%s: expected ',', found '%s' instead\n", __func__, token );
		return false;
	}

	// copy base info to new field
	pField->name = pFieldInfo->name;
	pField->offset = pFieldInfo->offset;
	pField->size = pFieldInfo->size;
	pField->flags = 0;

	// read delta-flags
	while(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) != NULL )
	{
		if( !Q_strcmp( token, "," ))
			break;	// end of flags argument

		if( !Q_strcmp( token, "|" ))
			continue;

		if( !Q_strcmp( token, "DT_BYTE" ))
			pField->flags |= DT_BYTE;
		else if( !Q_strcmp( token, "DT_SHORT" ))
			pField->flags |= DT_SHORT;
		else if( !Q_strcmp( token, "DT_FLOAT" ))
			pField->flags |= DT_FLOAT;
		else if( !Q_strcmp( token, "DT_INTEGER" ))
			pField->flags |= DT_INTEGER;
		else if( !Q_strcmp( token, "DT_ANGLE" ))
			pField->flags |= DT_ANGLE;
		else if( !Q_strcmp( token, "DT_TIMEWINDOW_8" ))
			pField->flags |= DT_TIMEWINDOW_8;
		else if( !Q_strcmp( token, "DT_TIMEWINDOW_BIG" ))
			pField->flags |= DT_TIMEWINDOW_BIG;
		else if( !Q_strcmp( token, "DT_STRING" ))
			pField->flags |= DT_STRING;
		else if( !Q_strcmp( token, "DT_SIGNED" ))
			pField->flags |= DT_SIGNED;
	}

	if( Q_strcmp( token, "," ))
	{
		Con_DPrintf( S_ERROR "%s: expected ',', found '%s' instead\n", __func__, token );
		return false;
	}

	// read delta-bits
	if(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) == NULL )
	{
		Con_DPrintf( S_ERROR "%s: %s field bits argument is missing\n", __func__, pField->name );
		return false;
	}

	pField->bits = Q_atoi( token );

	*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
	if( Q_strcmp( token, "," ))
	{
		Con_DPrintf( S_ERROR "%s: expected ',', found '%s' instead\n", __func__, token );
		return false;
	}

	// read delta-multiplier
	if(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) == NULL )
	{
		Con_DPrintf( S_ERROR "%s: %s missing 'multiplier' argument\n", __func__, pField->name );
		return false;
	}

	pField->multiplier = Q_atof( token );

	if( bPost )
	{
		*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
		if( Q_strcmp( token, "," ))
		{
			Con_DPrintf( S_ERROR "%s: expected ',', found '%s' instead\n", __func__, token );
			return false;
		}

		// read delta-postmultiplier
		if(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) == NULL )
		{
			Con_DPrintf( S_ERROR "%s: %s missing 'post_multiply' argument\n", __func__, pField->name );
			return false;
		}

		pField->post_multiplier = Q_atof( token );
	}
	else
	{
		// to avoid division by zero
		pField->post_multiplier = 1.0f;
	}

	// closing brace...
	*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
	if( Q_strcmp( token, ")" ))
	{
		Con_DPrintf( S_ERROR "%s: expected ')', found '%s' instead\n", __func__, token );
		return false;
	}

	// ... and trying to parse optional ',' post-symbol
	char *oldpos = *delta_script;
	*delta_script = COM_ParseFile( *delta_script, token, sizeof( token ));
	if( token[0] != ',' ) *delta_script = oldpos; // not a ','

	return true;
}

static void Delta_ParseTable( char **delta_script, delta_info_t *dt, const char *encodeDll, const char *encodeFunc )
{
	string token;

	// allocate the delta-structures
	if( !dt->pFields ) dt->pFields = (delta_t *)Z_Calloc( dt->maxFields * sizeof( delta_t ));

	delta_t *pField = dt->pFields;
	dt->numFields = 0;

	// assume we have handled '{'
	while(( *delta_script = COM_ParseFile( *delta_script, token, sizeof( token ))) != NULL )
	{
		Assert( dt->numFields <= dt->maxFields );

		if( !Q_strcmp( token, "DEFINE_DELTA" ))
		{
			if( Delta_ParseField( delta_script, dt, &pField[dt->numFields], false ))
				dt->numFields++;
		}
		else if( !Q_strcmp( token, "DEFINE_DELTA_POST" ))
		{
			if( Delta_ParseField( delta_script, dt, &pField[dt->numFields], true ))
				dt->numFields++;
		}
		else if( token[0] == '}' )
		{
			// end of the section
			break;
		}
	}

	// copy function name
	Q_strncpy( dt->funcName, encodeFunc, sizeof( dt->funcName ));

	if( !Q_stricmp( encodeDll, "none" ))
		dt->customEncode = CUSTOM_NONE;
	else if( !Q_stricmp( encodeDll, "gamedll" ))
		dt->customEncode = CUSTOM_SERVER_ENCODE;
	else if( !Q_stricmp( encodeDll, "clientdll" ))
		dt->customEncode = CUSTOM_CLIENT_ENCODE;

	// adjust to fit memory size
	if( dt->numFields < dt->maxFields )
	{
		dt->pFields = Z_Realloc( dt->pFields, dt->numFields * sizeof( delta_t ));
	}

	dt->bInitialized = true; // table is ok
}

static void Delta_InitFields( void )
{
	string encodeDll, encodeFunc, token;

	byte *afile = FS_LoadFile( DELTA_PATH, NULL, false );
	if( !afile ) Sys_Error( "%s: couldn't load file %s\n", __func__, DELTA_PATH );

	char *pfile = (char *)afile;

	while(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) != NULL )
	{
		delta_info_t *dt = Delta_FindStruct( token );

		if( dt == NULL )
		{
			Sys_Error( "%s: unknown struct %s\n", DELTA_PATH, token );
		}

		pfile = COM_ParseFile( pfile, encodeDll, sizeof( encodeDll ));

		if( !Q_stricmp( encodeDll, "none" ))
			Q_strncpy( encodeFunc, "null", sizeof( encodeFunc ));
		else pfile = COM_ParseFile( pfile, encodeFunc, sizeof( encodeFunc ));

		// jump to '{'
		pfile = COM_ParseFile( pfile, token, sizeof( token ));

		if( token[0] != '{' )
		{
			Sys_Error( "%s: missing '{' in section %s\n", DELTA_PATH, dt->pName );
		}

		Delta_ParseTable( &pfile, dt, encodeDll, encodeFunc );
	}

	Mem_Free( afile );
}

void Delta_Init( void )
{
	// shutdown it first
	if( delta_init ) Delta_Shutdown ();

	Delta_InitFields ();	// initialize fields
	delta_init = true;

	delta_info_t *dt = Delta_FindStructByIndex( DT_MOVEVARS_T );

	Assert( dt != NULL );

	if( dt->bInitialized )
		return;	// "movevars_t" already specified by user

	// create movevars_t delta internal
	Delta_AddField( dt, "gravity", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "stopspeed", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "maxspeed", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "spectatormaxspeed", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "accelerate", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "airaccelerate", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "wateraccelerate", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "friction", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "edgefriction", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "waterfriction", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "bounce", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "stepsize", DT_FLOAT|DT_SIGNED, 16, 16.0f, 1.0f );
	Delta_AddField( dt, "maxvelocity", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );

	// a1ba: set zmax large enough to fit 3d skybox
	// this fixes an issue when mapper sets sv_zmax value high enough
	// to not overflow the variable but not enough to be encoded in delta,
	// thus being clamped at 16-bit signed integer max.
	// by removing signed flag (zmax is always positive) and increasing it to
	// 24 bits, we ensure that even these maps will not have problems with 3d
	// skyboxes (that virtually have no coordinates limit)
	// see comment in SV_UpdateMovevars for more details
	Delta_AddField( dt, "zmax", DT_FLOAT, 24, 1.0f, 1.0f );

	Delta_AddField( dt, "waveHeight", DT_FLOAT|DT_SIGNED, 16, 16.0f, 1.0f );
	Delta_AddField( dt, "skyName", DT_STRING, 1, 1.0f, 1.0f );
	Delta_AddField( dt, "footsteps", DT_INTEGER, 1, 1.0f, 1.0f );
	Delta_AddField( dt, "rollangle", DT_FLOAT|DT_SIGNED, 16, 32.0f, 1.0f );
	Delta_AddField( dt, "rollspeed", DT_FLOAT|DT_SIGNED, 16, 8.0f, 1.0f );
	Delta_AddField( dt, "skycolor_r", DT_FLOAT|DT_SIGNED, 16, 1.0f, 1.0f ); // 0 - 264
	Delta_AddField( dt, "skycolor_g", DT_FLOAT|DT_SIGNED, 16, 1.0f, 1.0f );
	Delta_AddField( dt, "skycolor_b", DT_FLOAT|DT_SIGNED, 16, 1.0f, 1.0f );
	Delta_AddField( dt, "skyvec_x", DT_FLOAT|DT_SIGNED, 16, 32.0f, 1.0f ); // 0 - 1
	Delta_AddField( dt, "skyvec_y", DT_FLOAT|DT_SIGNED, 16, 32.0f, 1.0f );
	Delta_AddField( dt, "skyvec_z", DT_FLOAT|DT_SIGNED, 16, 32.0f, 1.0f );
	Delta_AddField( dt, "wateralpha", DT_FLOAT|DT_SIGNED, 16, 32.0f, 1.0f );
	Delta_AddField( dt, "fog_settings", DT_INTEGER, 32, 1.0f, 1.0f );
	dt->numFields = ARRAYSIZE( pm_fields ) - 4;

	// now done
	dt->bInitialized = true;
}

void Delta_InitClient( void )
{
	int numActive = 0;

	// already initalized
	if( delta_init ) return;

	for( int i = 0; i < ARRAYSIZE( dt_info ); i++ )
	{
		if( dt_info[i].numFields > 0 )
		{
			dt_info[i].bInitialized = true;
			numActive++;
		}
	}

	if( numActive ) delta_init = true;
}

void Delta_Shutdown( void )
{
	if( !delta_init ) return;

	for( int i = 0; i < ARRAYSIZE( dt_info ); i++ )
	{
		dt_info[i].numFields = 0;
		dt_info[i].customEncode = CUSTOM_NONE;
		dt_info[i].userCallback = NULL;
		dt_info[i].funcName[0] = '\0';

		if( dt_info[i].pFields )
		{
			Z_Free( dt_info[i].pFields );
			dt_info[i].pFields = NULL;
		}

		dt_info[i].bInitialized = false;
	}

	delta_init = false;
}

/*
=====================
Delta_ClampIntegerField

prevent data to out of range
=====================
*/
static int Delta_ClampIntegerField( delta_t *pField, int iValue, int signbit, int numbits )
{
#ifdef _DEBUG
	if( numbits < 32 && abs( iValue ) >= (uint)BIT( numbits ))
		Con_Reportf( S_WARN "Delta_ClampIntegerField: field %s = %d overflowed %d\n", pField->name, abs( iValue ), (uint)BIT( numbits ));
#endif
	if( numbits < 32 )
	{
		int signbits = numbits - signbit;
		int maxnum = BIT( signbits ) - 1;

		if( iValue > maxnum )
			iValue = maxnum;
		else if( signbit && iValue < -maxnum - 1 )
			iValue = -maxnum - 1;
	}

	return iValue; // clamped;
}

/*
=====================
Delta_IntegerToDouble

using double here as double can represent whole 32-bit integer range
unlike float
=====================
*/
static double Delta_IntegerToDouble( uint iValue, qboolean bSigned )
{
	return bSigned ? (double)(int)iValue : (double)iValue;
}

/*
=====================
Delta_IntegerFromDouble

=====================
*/
static uint Delta_IntegerFromDouble( double value, qboolean bSigned )
{
	if( bSigned )
		return (int)bound( (double)INT_MIN, value, (double)INT_MAX );
	return (uint)bound( 0.0, value, (double)UINT_MAX );
}

/*
=====================
Delta_PreMultiplyInteger

=====================
*/
static uint Delta_PreMultiplyInteger( const delta_t *pField, uint iValue, qboolean bSigned )
{
	if( Q_equal( pField->multiplier, 1.0 ))
		return iValue;

	return Delta_IntegerFromDouble( Delta_IntegerToDouble( iValue, bSigned ) * pField->multiplier, bSigned );
}

/*
=====================
Delta_PostMultiplyInteger

=====================
*/
static uint Delta_PostMultiplyInteger( const delta_t *pField, uint iValue, qboolean bSigned )
{
	if( Q_equal( pField->multiplier, 1.0 ) && Q_equal( pField->post_multiplier, 1.0 ))
		return iValue;

	double value = Delta_IntegerToDouble( iValue, bSigned );

	if( !Q_equal( pField->multiplier, 1.0 ))
		value /= pField->multiplier;

	if( !Q_equal( pField->post_multiplier, 1.0 ))
		value *= pField->post_multiplier;

	return Delta_IntegerFromDouble( value, bSigned );
}

/*
=====================
Delta_CompareField

compare fields by offsets
assume from and to is valid
=====================
*/
static qboolean Delta_CompareField( delta_t *pField, const void *from, const void *to )
{
	int		signbit = ( pField->flags & DT_SIGNED ) ? 1 : 0;
	float	val_a, val_b;
	int	fromF, toF;

	Assert( pField != NULL );
	Assert( from != NULL );
	Assert( to != NULL );

	if( pField->bInactive )
		return true;

	fromF = toF = 0;

	if( pField->flags & DT_BYTE )
	{
		if( signbit )
		{
			fromF = *(int8_t *)((int8_t *)from + pField->offset );
			toF = *(int8_t *)((int8_t *)to + pField->offset );
		}
		else
		{
			fromF = *(uint8_t *)((int8_t *)from + pField->offset );
			toF = *(uint8_t *)((int8_t *)to + pField->offset );
		}

		if( !Q_equal(pField->multiplier, 1.0f ))
		{
			fromF *= pField->multiplier;
			toF *= pField->multiplier;
		}

		fromF = Delta_ClampIntegerField( pField, fromF, signbit, pField->bits );
		toF = Delta_ClampIntegerField( pField, toF, signbit, pField->bits );
	}
	else if( pField->flags & DT_SHORT )
	{
		if( signbit )
		{
			fromF = *(int16_t *)((int8_t *)from + pField->offset );
			toF = *(int16_t *)((int8_t *)to + pField->offset );
		}
		else
		{
			fromF = *(uint16_t *)((int8_t *)from + pField->offset );
			toF = *(uint16_t *)((int8_t *)to + pField->offset );
		}

		if( !Q_equal(pField->multiplier, 1.0f ))
		{
			fromF *= pField->multiplier;
			toF *= pField->multiplier;
		}

		fromF = Delta_ClampIntegerField( pField, fromF, signbit, pField->bits );
		toF = Delta_ClampIntegerField( pField, toF, signbit, pField->bits );
	}
	else if( pField->flags & DT_INTEGER )
	{
		if( signbit )
		{
			fromF = *(int32_t *)((int8_t *)from + pField->offset );
			toF = *(int32_t *)((int8_t *)to + pField->offset );
		}
		else
		{
			fromF = *(uint32_t *)((int8_t *)from + pField->offset );
			toF = *(uint32_t *)((int8_t *)to + pField->offset );
		}

		if( !Q_equal(pField->multiplier, 1.0f ))
		{
			fromF *= pField->multiplier;
			toF *= pField->multiplier;
		}

		fromF = Delta_ClampIntegerField( pField, fromF, signbit, pField->bits );
		toF = Delta_ClampIntegerField( pField, toF, signbit, pField->bits );
	}
	else if( pField->flags & ( DT_ANGLE|DT_FLOAT ))
	{
		// don't convert floats to integers
		fromF = *((int *)((byte *)from + pField->offset ));
		toF = *((int *)((byte *)to + pField->offset ));
	}
	else if( pField->flags & DT_TIMEWINDOW_8 )
	{
		val_a = *(float *)((byte *)from + pField->offset );
		val_b = *(float *)((byte *)to + pField->offset );
		fromF = Q_rint( val_a * 100.0 );
		toF = Q_rint( val_b * 100.0 );
	}
	else if( pField->flags & DT_TIMEWINDOW_BIG )
	{
		val_a = *(float *)((byte *)from + pField->offset );
		val_b = *(float *)((byte *)to + pField->offset );
		fromF = Q_rint( val_a * pField->multiplier );
		toF = Q_rint( val_b * pField->multiplier );
	}
	else if( pField->flags & DT_STRING )
	{
		// compare strings
		char	*s1 = (char *)((byte *)from + pField->offset );
		char	*s2 = (char *)((byte *)to + pField->offset );

		// 0 is equal, otherwise not equal
		toF = Q_strcmp( s1, s2 );
	}

	return fromF == toF;
}

/*
=====================
Delta_TestBaseline

compare baselines to find optimal
=====================
*/
int Delta_TestBaseline( const entity_state_t *from, const entity_state_t *to, qboolean player, double timebase )
{
	delta_info_t *dt = NULL;
	int countBits = MAX_ENTITY_BITS + 2;

	if( to == NULL )
	{
		if( from == NULL ) return 0;
		return countBits;
	}

	if( FBitSet( to->entityType, ENTITY_BEAM ))
		dt = Delta_FindStructByIndex( DT_CUSTOM_ENTITY_STATE_T );
	else if( player )
		dt = Delta_FindStructByIndex( DT_ENTITY_STATE_PLAYER_T );
	else dt = Delta_FindStructByIndex( DT_ENTITY_STATE_T );

	Assert( dt && dt->bInitialized );

	countBits++; // entityType flag

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		// flag about field change (sets always)
		countBits++;

		if( !Delta_CompareField( pField, from, to ))
		{
			// strings are handled differently
			if( FBitSet( pField->flags, DT_STRING ))
				countBits += Q_strlen((char *)((byte *)to + pField->offset )) * 8;
			else countBits += pField->bits;
		}
	}

	// g-cont. compare bitcount directly no reason to call BitByte here
	return countBits;
}

/*
=====================
Delta_WriteField

write fields by offsets
assume from and to is valid
=====================
*/
static void Delta_WriteField_( sizebuf_t *msg, delta_t *pField, const void *from, const void *to, double timebase )
{
	int		signbit = FBitSet( pField->flags, DT_SIGNED ) ? 1 : 0;
	float		flValue, flAngle;
	uint		iValue;
	int dt;
	const char	*pStr;

	if( pField->flags & DT_BYTE )
	{
		if( signbit )
			iValue = *(int8_t *)((int8_t *)to + pField->offset );
		else
			iValue = *(uint8_t *)((int8_t *)to + pField->offset );

		iValue = Delta_PreMultiplyInteger( pField, iValue, signbit );

		iValue = Delta_ClampIntegerField( pField, iValue, signbit, pField->bits );
		MSG_WriteBitLong( msg, iValue, pField->bits, signbit );
	}
	else if( pField->flags & DT_SHORT )
	{
		if( signbit )
			iValue = *(int16_t *)((int8_t *)to + pField->offset );
		else
			iValue = *(uint16_t *)((int8_t *)to + pField->offset );

		iValue = Delta_PreMultiplyInteger( pField, iValue, signbit );

		iValue = Delta_ClampIntegerField( pField, iValue, signbit, pField->bits );
		MSG_WriteBitLong( msg, iValue, pField->bits, signbit );
	}
	else if( pField->flags & DT_INTEGER )
	{
		if( signbit )
			iValue = *(int32_t *)((int8_t *)to + pField->offset );
		else
			iValue = *(uint32_t *)((int8_t *)to + pField->offset );

		iValue = Delta_PreMultiplyInteger( pField, iValue, signbit );

		iValue = Delta_ClampIntegerField( pField, iValue, signbit, pField->bits );
		MSG_WriteBitLong( msg, iValue, pField->bits, signbit );
	}
	else if( pField->flags & DT_FLOAT )
	{
		flValue = *(float *)((byte *)to + pField->offset );
		iValue = (int)((double)flValue * pField->multiplier);
		iValue = Delta_ClampIntegerField( pField, iValue, signbit, pField->bits );
		MSG_WriteBitLong( msg, iValue, pField->bits, signbit );
	}
	else if( pField->flags & DT_ANGLE )
	{
		flAngle = *(float *)((byte *)to + pField->offset );

		// NOTE: never applies multipliers to angle because
		// result may be wrong on client-side
		MSG_WriteBitAngle( msg, flAngle, pField->bits );
	}
	else if( pField->flags & DT_TIMEWINDOW_8 )
	{
		flValue = *(float *)((byte *)to + pField->offset );
		dt = Q_rint(( timebase - flValue ) * 100.0 );
		dt = Delta_ClampIntegerField( pField, dt, 1, pField->bits );
		MSG_WriteSBitLong( msg, dt, pField->bits );
	}
	else if( pField->flags & DT_TIMEWINDOW_BIG )
	{
		flValue = *(float *)((byte *)to + pField->offset );
		dt = Q_rint(( timebase - flValue ) * pField->multiplier );
		dt = Delta_ClampIntegerField( pField, dt, 1, pField->bits );
		MSG_WriteSBitLong( msg, dt, pField->bits );
	}
	else if( pField->flags & DT_STRING )
	{
		pStr = (char *)((byte *)to + pField->offset );
		MSG_WriteString( msg, pStr );
	}
}

static qboolean Delta_WriteField( sizebuf_t *msg, delta_t *pField, const void *from, const void *to, double timebase )
{
	if( Delta_CompareField( pField, from, to ))
	{
		MSG_WriteOneBit( msg, 0 );	// unchanged
		return false;
	}

	MSG_WriteOneBit( msg, 1 );	// changed

	Delta_WriteField_( msg, pField, from, to, timebase );

	return true;
}

/*
====================
Delta_CopyField

====================
*/
static void Delta_CopyField( delta_t *pField, const void *from, void *to, double timebase )
{
	qboolean bSigned = FBitSet( pField->flags, DT_SIGNED );
	uint8_t *to_field = (uint8_t *)to + pField->offset;
	uint8_t *from_field = (uint8_t *)from + pField->offset;

	if( FBitSet( pField->flags, DT_BYTE ))
	{
		if( bSigned )
			*(int8_t *)( to_field ) = *(int8_t *)( from_field );
		else
			*(uint8_t *)( to_field ) = *(uint8_t *)( from_field );
	}
	else if( FBitSet( pField->flags, DT_SHORT ))
	{
		if( bSigned )
			*(int16_t *)( to_field ) = *(int16_t *)( from_field );
		else
			*(uint16_t *)( to_field ) = *(uint16_t *)( from_field );
	}
	else if( FBitSet( pField->flags, DT_INTEGER ))
	{
		if( bSigned )
			*(int32_t *)( to_field ) = *(int32_t *)( from_field );
		else
			*(uint32_t *)( to_field ) = *(uint32_t *)( from_field );
	}
	else if( FBitSet( pField->flags, DT_FLOAT|DT_ANGLE|DT_TIMEWINDOW_8|DT_TIMEWINDOW_BIG ))
	{
		*(float *)( to_field ) = *(float *)( from_field );
	}
	else if( FBitSet( pField->flags, DT_STRING ))
	{
		Q_strncpy( to_field, from_field, pField->size );
	}
	else
	{
		Assert( 0 );
	}
}

/*
=====================
Delta_ReadField

read fields by offsets
assume 'from' and 'to' is valid
=====================
*/
static void Delta_ReadField_( sizebuf_t *msg, delta_t *pField, void *to, double timebase )
{
	qboolean		bSigned = ( pField->flags & DT_SIGNED ) ? true : false;
	float		flValue, flAngle, flTime;
	uint		iValue;
	const char	*pStr;
	char		*pOut;

	Assert( pField->multiplier != 0.0f );

	if( pField->flags & DT_BYTE )
	{
		iValue = MSG_ReadBitLong( msg, pField->bits, bSigned );
		iValue = Delta_PostMultiplyInteger( pField, iValue, bSigned );

		if( bSigned )
			*(int8_t *)((uint8_t *)to + pField->offset ) = iValue;
		else
			*(uint8_t *)((uint8_t *)to + pField->offset ) = iValue;
	}
	else if( pField->flags & DT_SHORT )
	{
		iValue = MSG_ReadBitLong( msg, pField->bits, bSigned );
		iValue = Delta_PostMultiplyInteger( pField, iValue, bSigned );

		if( bSigned )
			*(int16_t *)((uint8_t *)to + pField->offset ) = iValue;
		else
			*(uint16_t *)((uint8_t *)to + pField->offset ) = iValue;
	}
	else if( pField->flags & DT_INTEGER )
	{
		iValue = MSG_ReadBitLong( msg, pField->bits, bSigned );
		iValue = Delta_PostMultiplyInteger( pField, iValue, bSigned );

		if( bSigned )
			*(int32_t *)((uint8_t *)to + pField->offset ) = iValue;
		else
			*(uint32_t *)((uint8_t *)to + pField->offset ) = iValue;
	}
	else if( pField->flags & DT_FLOAT )
	{
		iValue = MSG_ReadBitLong( msg, pField->bits, bSigned );
		if( bSigned )
			flValue = (int)iValue;
		else
			flValue = iValue;

		if( !Q_equal( pField->multiplier, 1.0 ))
			flValue /= pField->multiplier;

		if( !Q_equal( pField->post_multiplier, 1.0 ))
			flValue *= pField->post_multiplier;

		*(float *)((byte *)to + pField->offset ) = flValue;
	}
	else if( pField->flags & DT_ANGLE )
	{
		flAngle = MSG_ReadUBitLong( msg, pField->bits ) * ( 360.0f / (float)( 1 << pField->bits ));
		*(float *)((byte *)to + pField->offset ) = flAngle;
	}
	else if( pField->flags & DT_TIMEWINDOW_8 )
	{
		iValue = MSG_ReadSBitLong( msg, pField->bits );
		flTime = ( timebase * 100.0 - (int)iValue ) / 100.0;
		*(float *)((byte *)to + pField->offset ) = flTime;
	}
	else if( pField->flags & DT_TIMEWINDOW_BIG )
	{
		iValue = MSG_ReadSBitLong( msg, pField->bits );
		flTime = ( timebase * pField->multiplier - (int)iValue ) / pField->multiplier;
		*(float *)((byte *)to + pField->offset ) = flTime;
	}
	else if( pField->flags & DT_STRING )
	{
		pStr = MSG_ReadString( msg );
		pOut = (char *)((byte *)to + pField->offset );
		Q_strncpy( pOut, pStr, pField->size );
	}
}

static qboolean Delta_ReadField( sizebuf_t *msg, delta_t *pField, const void *from, void *to, double timebase )
{
	if( !MSG_ReadOneBit( msg ))
	{
		Delta_CopyField( pField, from, to, timebase );
		return false;
	}

	Delta_ReadField_( msg, pField, to, timebase );
	return true;
}

/*
=====================
Delta_DebugFieldValue

Render the already-decoded value of a delta field that Delta_ReadField_
just wrote into `to`. This is what makes the GSDELTA ledger self-contained:
floats/times are printed post-multiplier, angles in degrees, ints raw — so
a single engine.log line carries the FINAL value and no offline re-compute
is needed to read the trace.
=====================
*/
static void Delta_DebugFieldValue( const delta_t *pField, const void *to, char *buf, size_t bufsize )
{
	const void *pv = (const uint8_t *)to + pField->offset;

	if( pField->flags & DT_FLOAT )
		Q_snprintf( buf, bufsize, "%.6g", *(const float *)pv );
	else if( pField->flags & DT_ANGLE )
		Q_snprintf( buf, bufsize, "%.3fdeg", *(const float *)pv );
	else if( pField->flags & DT_TIMEWINDOW_8 || pField->flags & DT_TIMEWINDOW_BIG )
		Q_snprintf( buf, bufsize, "%.5f", *(const float *)pv );
	else if( pField->flags & DT_STRING )
		Q_snprintf( buf, bufsize, "\"%s\"", (const char *)pv );
	else if( pField->flags & DT_BYTE )
		Q_snprintf( buf, bufsize, "%d", ( pField->flags & DT_SIGNED ) ? *(const int8_t *)pv : *(const uint8_t *)pv );
	else if( pField->flags & DT_SHORT )
		Q_snprintf( buf, bufsize, "%d", ( pField->flags & DT_SIGNED ) ? *(const int16_t *)pv : *(const uint16_t *)pv );
	else
		Q_snprintf( buf, bufsize, "%d", ( pField->flags & DT_SIGNED ) ? *(const int32_t *)pv : *(const uint32_t *)pv );
}

static void Delta_ParseGSFields( sizebuf_t *msg, const delta_info_t *dt, const void *from, void *to, double timebase )
{
	// The four-bit wire count can describe up to fifteen mask bytes.
	uint8_t bits[16] = { 0 };
	delta_t *pField;
	int dbg = Cvar_VariableInteger( "cl_goldsrc_debug" );
	int entryBit;
	int i;

	entryBit = MSG_GetNumBitsRead( msg );

	byte c = MSG_ReadUBitLong( msg, 4 );

	for( i = 0; i < c; i++ )
		bits[i] = MSG_ReadByte( msg );

	// field-level bit ledger: reconstruct exactly which fields the client
	// consumed and where each field landed in the bitstream. Together with
	// the table dump on parse error this makes table/layout mismatches
	// visible in a single engine.log.
	if( dbg >= 6 )
	{
		char flags[16 * 3 + 1];
		int nflags = Q_min( c, (int)sizeof( bits ));
		for( i = 0; i < nflags; i++ )
			Q_snprintf( flags + i * 3, sizeof( flags ) - i * 3, "%02x%s", bits[i], i + 1 < nflags ? " " : "" );
		Con_DPrintf( "GSDELTA-READ: table=%s c=%d bitflags=%s startbit=%d\n",
			dt->pName, c, flags, entryBit );
	}

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		int b = Q_min( i >> 3, (int)sizeof( bits ) - 1 );
		int n = 1 << ( i & 7 );
		int bitBefore = MSG_GetNumBitsRead( msg );

		if( FBitSet( bits[b], n ))
		{
			Delta_ReadField_( msg, pField, to, timebase );
			if( dbg >= 6 )
			{
				char valbuf[64];
				Delta_DebugFieldValue( pField, to, valbuf, sizeof( valbuf ));
				Con_DPrintf( "GSDELTA-FIELD: table=%s idx=%d name='%s' bits=%d types=0x%x pre=%.5g post=%.5g read_at_bit=%d consumed_bits=%d value=%s\n",
					dt->pName, i, pField->name ? pField->name : "?", pField->bits, pField->flags,
					pField->multiplier, pField->post_multiplier, bitBefore, MSG_GetNumBitsRead( msg ) - bitBefore, valbuf );
			}
		}
		else
		{
			Delta_CopyField( pField, from, to, timebase );
			if( dbg >= 6 )
			{
				char valbuf[64];
				Delta_DebugFieldValue( pField, to, valbuf, sizeof( valbuf ));
				Con_DPrintf( "GSDELTA-COPY: table=%s idx=%d name='%s' bits=%d value=%s\n",
					dt->pName, i, pField->name ? pField->name : "?", pField->bits, valbuf );
			}
		}
	}

	if( dbg >= 6 )
		Con_DPrintf( "GSDELTA-READ: table=%s done endbit=%d total_bits=%d\n",
			dt->pName, MSG_GetNumBitsRead( msg ), MSG_GetNumBitsRead( msg ) - entryBit );
}

/*
=====================
Delta_DebugDumpTable

dump the *current* state of a delta table (field list, widths, offsets)
as seen by the client right now. Called on parse errors so a table/layout
mismatch (e.g. 17 vs 34 fields) is visible in a single engine.log.
=====================
*/
void Delta_DebugDumpTable( sizebuf_t *msg, int index, const char *context )
{
	const delta_info_t *dt = Delta_FindStructByIndex( index );
	int dbg = Cvar_VariableInteger( "cl_goldsrc_debug" );

	if( !dt )
	{
		Con_Printf( "GSDELTA-TABLE %s: index=%d not found\n", context, index );
		return;
	}

	Con_Printf( "GSDELTA-TABLE %s: name=%s numFields=%d maxFields=%d initialized=%d\n",
		context, dt->pName, dt->numFields, dt->maxFields, dt->bInitialized );

	if( !dt->bInitialized || !dt->pFields || dbg < 5 )
		return;

	for( int i = 0; i < dt->numFields; i++ )
	{
		const delta_t *p = &dt->pFields[i];
		Con_Printf( "  [%3d] name='%s' offset=%d size=%d flags=0x%x bits=%d\n",
			i, p->name ? p->name : "?", p->offset, p->size, p->flags, p->bits );
	}
}

void Delta_DebugDumpAllTables( const char *context )
{
	Con_Printf( "GSDELTA-ALL %s: numTables=%d\n", context, Delta_NumTables() );
	for( int i = 0; i < Delta_NumTables(); i++ )
		Delta_DebugDumpTable( NULL, i, context );
}

void Delta_ReadGSFields( sizebuf_t *msg, int index, const void *from, void *to, double timebase )
{
	const delta_info_t *dt = Delta_FindStructByIndex( index );
	Delta_ParseGSFields( msg, dt, from, to, timebase );
}

void Delta_WriteGSFields( sizebuf_t *msg, int index, const void *from, const void *to, double timebase )
{
	delta_info_t *dt = Delta_FindStructByIndex( index );
	delta_t *pField;
	uint8_t bits[8] = { 0 };
	uint c = 0;
	int i;

	Delta_CustomEncode( dt, from, to );

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		if( !Delta_CompareField( pField, from, to ))
		{
			int b = Q_min( i >> 3, (int)sizeof( bits ) - 1 );
			int n = 1 << ( i & 7 );

			SetBits( bits[b], n );
			c = b + 1;
		}
	}

	MSG_WriteUBitLong( msg, c, 4 );
	for( i = 0; i < c; i++ )
		MSG_WriteByte( msg, bits[i] );

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		int b = Q_min( i >> 3, (int)sizeof( bits ) - 1 );
		int n = 1 << ( i & 7 );

		if( FBitSet( bits[b], n ))
			Delta_WriteField_( msg, pField, from, to, timebase );
	}
}

/*
=============================================================================

usercmd_t communication

=============================================================================
*/
/*
=====================
MSG_WriteDeltaUsercmd
=====================
*/
void MSG_WriteDeltaUsercmd( sizebuf_t *msg, const usercmd_t *from, const usercmd_t *to )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_USERCMD_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_WriteField( msg, pField, from, to, 0.0f );
	}
}

/*
=====================
MSG_ReadDeltaUsercmd
=====================
*/
void MSG_ReadDeltaUsercmd( sizebuf_t *msg, const usercmd_t *from, usercmd_t *to )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_USERCMD_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	*to = *from;

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_ReadField( msg, pField, from, to, 0.0f );
	}

	COM_NormalizeAngles( to->viewangles );
}

/*
============================================================================

event_args_t communication

============================================================================
*/
/*
=====================
MSG_WriteDeltaEvent
=====================
*/
void MSG_WriteDeltaEvent( sizebuf_t *msg, const event_args_t *from, const event_args_t *to )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_EVENT_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_WriteField( msg, pField, from, to, 0.0f );
	}
}

/*
=====================
MSG_ReadDeltaEvent
=====================
*/
void MSG_ReadDeltaEvent( sizebuf_t *msg, const event_args_t *from, event_args_t *to )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_EVENT_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	*to = *from;

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_ReadField( msg, pField, from, to, 0.0f );
	}
}

/*
=============================================================================

movevars_t communication

=============================================================================
*/
qboolean MSG_WriteDeltaMovevars( sizebuf_t *msg, const movevars_t *from, const movevars_t *to )
{
	int numChanges = 0;

	delta_info_t *dt = Delta_FindStructByIndex( DT_MOVEVARS_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	int startBit = msg->iCurBit;

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	MSG_BeginServerCmd( msg, svc_deltamovevars );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		if( Delta_WriteField( msg, pField, from, to, 0.0f ))
			numChanges++;
	}

	// if we have no changes - kill the message
	if( !numChanges )
	{
		MSG_SeekToBit( msg, startBit, SEEK_SET );
		return false;
	}
	return true;
}

void MSG_ReadDeltaMovevars( sizebuf_t *msg, const movevars_t *from, movevars_t *to )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_MOVEVARS_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	*to = *from;

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_ReadField( msg, pField, from, to, 0.0f );
	}
}

/*
=============================================================================

clientdata_t communication

=============================================================================
*/
/*
==================
MSG_WriteClientData

Writes current client data only for local client
Other clients can grab the client state from entity_state_t
==================
*/
void MSG_WriteClientData( sizebuf_t *msg, const clientdata_t *from, const clientdata_t *to, double timebase )
{
	int numChanges = 0;

	delta_info_t *dt = Delta_FindStructByIndex( DT_CLIENTDATA_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	int startBit = msg->iCurBit;

	MSG_WriteOneBit( msg, 1 ); // have clientdata

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		if( Delta_WriteField( msg, pField, from, to, timebase ))
			numChanges++;
	}

	if( numChanges ) return; // we have updates

	MSG_SeekToBit( msg, startBit, SEEK_SET );
	MSG_WriteOneBit( msg, 0 ); // no changes
}

/*
==================
MSG_ReadClientData

Read the clientdata
==================
*/
void MSG_ReadClientData( sizebuf_t *msg, const clientdata_t *from, clientdata_t *to, double timebase )
{
#if !XASH_DEDICATED
	delta_info_t *dt = Delta_FindStructByIndex( DT_CLIENTDATA_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	qboolean noChanges = !MSG_ReadOneBit( msg );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		if( noChanges )
			Delta_CopyField( pField, from, to, timebase );
		else Delta_ReadField( msg, pField, from, to, timebase );
	}
#endif
}

/*
=============================================================================

weapon_data_t communication

=============================================================================
*/
/*
==================
MSG_WriteWeaponData

Writes current client data only for local client
Other clients can grab the client state from entity_state_t
==================
*/
void MSG_WriteWeaponData( sizebuf_t *msg, const weapon_data_t *from, const weapon_data_t *to, double timebase, int index )
{
	int numChanges = 0;

	delta_info_t *dt = Delta_FindStructByIndex( DT_WEAPONDATA_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// activate fields and call custom encode func
	Delta_CustomEncode( dt, from, to );

	int startBit = msg->iCurBit;

	MSG_WriteOneBit( msg, 1 );
	MSG_WriteUBitLong( msg, index, MAX_WEAPON_BITS );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		if( Delta_WriteField( msg, pField, from, to, timebase ))
			numChanges++;
	}

	// if we have no changes - kill the message
	if( !numChanges ) MSG_SeekToBit( msg, startBit, SEEK_SET );
}

/*
==================
MSG_ReadWeaponData

Read the clientdata
==================
*/
void MSG_ReadWeaponData( sizebuf_t *msg, const weapon_data_t *from, weapon_data_t *to, double timebase )
{
	delta_info_t *dt = Delta_FindStructByIndex( DT_WEAPONDATA_T );
	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_ReadField( msg, pField, from, to, timebase );
	}
}

/*
=============================================================================

entity_state_t communication

=============================================================================
*/
/*
==================
MSG_WriteDeltaEntity

Writes part of a packetentities message, including the entity number.
Can delta from either a baseline or a previous packet_entity
If to is NULL, a remove entity update will be sent
If force is not set, then nothing at all will be generated if the entity is
identical, under the assumption that the in-order delta code will catch it.
==================
*/
void MSG_WriteDeltaEntity( const entity_state_t *from, const entity_state_t *to, sizebuf_t *msg, qboolean force, int delta_type, double timebase, int baseline )
{
	delta_info_t *dt = NULL;
	int numChanges = 0;

	if( to == NULL )
	{
		int fRemoveType;

		if( from == NULL ) return;

		// a NULL to is a delta remove message
		MSG_WriteUBitLong( msg, from->number, MAX_ENTITY_BITS );

		// fRemoveType:
		// 0 - keep alive, has delta-update
		// 1 - remove from delta message (but keep states)
		// 2 - completely remove from server
		if( force ) fRemoveType = 2;
		else fRemoveType = 1;

		MSG_WriteUBitLong( msg, fRemoveType, 2 );
		return;
	}

	int startBit = msg->iCurBit;

	if( to->number < 0 || to->number >= GI->max_edicts )
		Host_Error( "%s: Bad entity number: %i\n", __func__, to->number );

	MSG_WriteUBitLong( msg, to->number, MAX_ENTITY_BITS );
	MSG_WriteUBitLong( msg, 0, 2 ); // alive

	if( baseline != 0 )
	{
		MSG_WriteOneBit( msg, 1 );
		MSG_WriteSBitLong( msg, baseline, 7 );
	}
	else MSG_WriteOneBit( msg, 0 );

	if( force || ( to->entityType != from->entityType ))
	{
		MSG_WriteOneBit( msg, 1 );
		MSG_WriteUBitLong( msg, to->entityType, 2 );
		numChanges++;
	}
	else MSG_WriteOneBit( msg, 0 );

	if( FBitSet( to->entityType, ENTITY_BEAM ))
	{
		dt = Delta_FindStructByIndex( DT_CUSTOM_ENTITY_STATE_T );
	}
	else if( delta_type == DELTA_PLAYER )
	{
		dt = Delta_FindStructByIndex( DT_ENTITY_STATE_PLAYER_T );
	}
	else
	{
		dt = Delta_FindStructByIndex( DT_ENTITY_STATE_T );
	}

	Assert( dt && dt->bInitialized );

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	if( delta_type == DELTA_STATIC )
	{
		// static entities won't to be custom encoded
		for( int i = 0; i < dt->numFields; i++ )
			dt->pFields[i].bInactive = false;
	}
	else
	{
		// activate fields and call custom encode func
		Delta_CustomEncode( dt, from, to );
	}

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		if( Delta_WriteField( msg, pField, from, to, timebase ))
			numChanges++;
	}

	// if we have no changes - kill the message
	if( !numChanges && !force ) MSG_SeekToBit( msg, startBit, SEEK_SET );
}

/*
==================
MSG_ReadDeltaEntity

The entity number has already been read from the message, which
is how the from state is identified.

If the delta removes the entity, entity_state_t->number will be set to MAX_EDICTS
Can go from either a baseline or a previous packet_entity
==================
*/
qboolean MSG_ReadDeltaEntity( sizebuf_t *msg, const entity_state_t *from, entity_state_t *to, int number, int delta_type, double timebase )
{
#if !XASH_DEDICATED
	delta_info_t *dt = NULL;
	int baseline_offset = 0;

	if( number < 0 || number >= clgame.maxEntities )
	{
		Con_Printf( S_ERROR "%s: bad delta entity number: %i\n", __func__, number );
		return false;
	}

	int fRemoveType = MSG_ReadUBitLong( msg, 2 );

	if( fRemoveType )
	{
		// check for a remove
		memset( to, 0, sizeof( *to ));

		if( fRemoveType & 1 )
		{
			// removed from delta-message
			return false;
		}

		if( fRemoveType & 2 )
		{
			// entity was removed from server
			to->number = -1;
			return false;
		}

		Con_Printf( S_ERROR "%s: unknown update type %i\n", __func__, fRemoveType );
		return false;
	}

	if( MSG_ReadOneBit( msg ))
		baseline_offset = MSG_ReadSBitLong( msg, 7 );

	if( baseline_offset != 0 )
	{
		if( delta_type == DELTA_STATIC )
		{
			int backup = Q_max( 0, clgame.numStatics - abs( baseline_offset ));
			from = &clgame.static_entities[backup].baseline;
		}
		else if( baseline_offset > 0 )
		{
			int backup = cls.next_client_entities - baseline_offset;
			from = &cls.packet_entities[backup % cls.num_client_entities];
		}
		else
		{
			baseline_offset = abs( baseline_offset + 1 );
			if( baseline_offset < cl.instanced_baseline_count )
				from = &cl.instanced_baseline[baseline_offset];
		}
	}

	// g-cont. probably is redundant
	*to = *from;

	if( MSG_ReadOneBit( msg ))
		to->entityType = MSG_ReadUBitLong( msg, 2 );
	to->number = number;

	if( FBitSet( to->entityType, ENTITY_BEAM ))
	{
		dt = Delta_FindStructByIndex( DT_CUSTOM_ENTITY_STATE_T );
	}
	else if( delta_type == DELTA_PLAYER )
	{
		dt = Delta_FindStructByIndex( DT_ENTITY_STATE_PLAYER_T );
	}
	else
	{
		dt = Delta_FindStructByIndex( DT_ENTITY_STATE_T );
	}

	if( !dt || !dt->bInitialized )
	{
		Con_Printf( S_ERROR "%s: broken delta\n", __func__ );
		return true;
	}

	delta_t *pField = dt->pFields;
	Assert( pField != NULL );

	// process fields
	for( int i = 0; i < dt->numFields; i++, pField++ )
	{
		Delta_ReadField( msg, pField, from, to, timebase );
	}
#endif // XASH_DEDICATED
	// message parsed
	return true;
}

static void Delta_GSDumpPayload( const char *label, const sizebuf_t *msg, int startBit )
{
	static const char hexd[] = "0123456789abcdef";
	int start = startBit >> 3;
	int end = msg->nDataBits >> 3;
	int len = end - start;

	if( len > 256 ) len = 256;
	if( len <= 0 ) return;

	for( int i = 0; i < len; i += 16 )
	{
		char hex[64];
		int n = Q_min( 16, len - i );
		for( int j = 0; j < n; j++ )
		{
			byte b = msg->pData[start + i + j];
			hex[j * 3] = hexd[b >> 4];
			hex[j * 3 + 1] = hexd[b & 15];
			hex[j * 3 + 2] = ' ';
		}
		hex[n * 3] = 0;
		Con_Printf( "%s %04x: %s\n", label, start + i, hex );
	}
}

/*
===================================
GoldSrc/Sven Co-op delta description

The Sven engine sends ALL table descriptions in ONE svc_deltatable message.
Each table is:

	name\0
	u16   count   (number of slots; real field records == count / 2,
	               each field is a (data-blob, name+descriptor) pair)
	then  count/2 records, each:
	       [data blob]      opaque junk, skipped by token scan
	       [name\0]         field name
	       [descriptor]     variable length (see below)
	                         - u16  offset, ONLY present if nonzero
	                         - u8   marker byte (0x01 in captures)
	                         - u8   significant_bits
	                         - u32  premultiply   (fixed point, 4000 == 1.0)
	                         - u32  postmultiply  (fixed point, 4000 == 1.0)

The descriptor is what the server's DELTA_WriteDelta emits for the field
description against a zeroed record (g_MetaDelta): meta fields whose value
is zero (e.g. fieldOffset == 0) are omitted on the wire, which is why the
offset u16 disappears for the first struct member. There is NO flags byte:
type flags are taken from the local struct layout (explicit DT_* per field).

After the last table comes a usermsg id->name registration array plus
other Sven payload, which is simply consumed up to the end of the message.
===================================
*/
static qboolean Delta_ReadGSToken( sizebuf_t *msg, char *buf, size_t maxlen )
{
	// skip non-printable data until a NUL-terminated printable run of
	// at least 3 chars is found; consumes the trailing NUL as well
	while( MSG_GetNumBitsLeft( msg ) >= 8 )
	{
		int c = MSG_ReadByte( msg );

		if( c < 32 || c > 126 )
			continue; // junk byte

		size_t len = 0;
		while( c >= 32 && c <= 126 )
		{
			if( len < maxlen - 1 )
				buf[len] = (char)c;
			len++;

			if( MSG_GetNumBitsLeft( msg ) < 8 )
			{
				buf[0] = 0;
				return false;
			}
			c = MSG_ReadByte( msg );
		}

		if( len >= 3 && c == 0 )
		{
			if( len > maxlen - 1 )
				len = maxlen - 1;
			buf[len] = 0;
			return true;
		}
		// not a usable token, keep scanning
	}

	buf[0] = 0;
	return false;
}

void Delta_ParseTableField_GS( sizebuf_t *msg )
{
	char name[32], pending[32] = "";
	delta_info_t *dt;
	int dbg = Cvar_VariableInteger( "cl_goldsrc_debug" );

	// delta encoders it's already initialized on this machine (local game)
	// re-initialize and load delta.lst before applying server overrides
	if( delta_init )
		Delta_Init();

	MSG_StartBitWriting( msg );

	while( MSG_GetNumBitsLeft( msg ) >= 8 )
	{
		const char *s = name;
		int savedBit = msg->iCurBit;

		if( pending[0] )
		{
			// next table name was already read while parsing the previous one
			Q_strncpy( name, pending, sizeof( name ));
			pending[0] = 0;
		}
		else if( !Delta_ReadGSToken( msg, name, sizeof( name )))
		{
			break;
		}

		dt = Delta_FindStruct( s );
		if( !dt )
		{
			// not a known delta struct: rewind past the consumed token
			// so the main parse loop can read it as the next svc command
			MSG_SeekToBit( msg, savedBit, SEEK_SET );
			if( dbg >= 1 )
				Con_DPrintf( "GS-DELTA: tail '%s' bitpos=%d bitsleft=%d\n", s, msg->iCurBit, MSG_GetNumBitsLeft( msg ));
			break;
		}

		int num_fields = MSG_ReadShort( msg );
		int pairs = Q_min( num_fields / 2, 512 ); // blob+name pairs

		if( dbg >= 2 )
			Con_DPrintf( "GS-DELTA: table='%s' num_fields=%d pairs=%d maxFields=%d bitpos=%d\n",
				s, num_fields, pairs, dt->maxFields, msg->iCurBit );

		for( int i = 0; i < pairs; i++ )
		{
			int bitBefore = msg->iCurBit;

			if( !Delta_ReadGSToken( msg, name, sizeof( name )))
				break;

			if( Delta_FindStruct( name ))
			{
				// field list done, next table name is already consumed
				Q_strncpy( pending, name, sizeof( pending ));
				break;
			}

			const delta_field_t *pInfo = Delta_FindFieldInfo( dt->pInfo, name, dt->maxFields );

			int fOffset = 0;
			int fBits = 1;
			uint fPre = 0, fPost = 0;

			// the server omits the u16 offset when the field offset is zero
			if( !pInfo || pInfo->offset != 0 )
			{
				fOffset = MSG_ReadShort( msg );
			}
			MSG_ReadByte( msg ); // constant marker byte
			fBits = MSG_ReadByte( msg );
			fPre = MSG_ReadUBitLong( msg, 32 );
			fPost = MSG_ReadUBitLong( msg, 32 );

			if( dbg >= 2 )
				Con_DPrintf( "  [%d] bit=%d name='%s' offset=%d sigbits=%d pre=%u post=%u\n",
					i, bitBefore, name, fOffset, fBits, fPre, fPost );

			if( !pInfo )
			{
				// Sven-specific field this engine doesn't know; consume in wire but skip
				if( dbg >= 2 )
					Con_DPrintf( S_WARN "%s: %s->%s: unknown wire field, skipped\n", __func__, dt->pName, name );
				continue;
			}

			if( pInfo->offset != fOffset )
			{
				Con_DPrintf( S_WARN "%s: %s->%s: offset mismatch wire=%d local=%d\n", __func__, dt->pName, name, fOffset, pInfo->offset );
			}

			// premultiply/postmultiply are fixed point with 4000 == 1.0
			float mul = ( fPre != 0 ) ? (float)fPre / 4000.0f : 1.0f;
			float post_mul = ( fPost != 0 ) ? (float)fPost / 4000.0f : 1.0f;

			int existingFlags = pInfo->flags;
			for( int k = 0; k < dt->numFields; k++ )
			{
				if( !Q_strcmp( dt->pFields[k].name, name ))
				{
					existingFlags = dt->pFields[k].flags;
					break;
				}
			}

			Delta_AddField( dt, name, existingFlags, fBits, mul, post_mul );
		}

		dt->bInitialized = true;
	}

	// after applying the server overrides, dump the resulting layout so a
	// wire/local field-count mismatch (Sven's 17 vs delta.lst's 34) can be
	// caught in the same log that shows the rest of the parse.
	if( dbg >= 5 )
		Delta_DebugDumpAllTables( "after-deltatable" );

	if( dbg >= 3 )
		Delta_GSDumpPayload( "GS-DELTA-TAIL", msg, msg->iCurBit );

	// Do NOT consume remaining bytes here: after the delta tables the
	// network message may contain additional svc commands (movevars,
	// cdtrack, setview, etc.) that the main parse loop must handle.

	if( dbg >= 2 )
		Con_DPrintf( "GS-DELTA: done bitpos=%d bitsleft=%d\n", msg->iCurBit, MSG_GetNumBitsLeft( msg ));

	MSG_EndBitWriting( msg );
}

/*
==================
Delta_WriteDescriptionToClient

send delta communication encoding
==================
*/
void Delta_WriteDescriptionToClient( sizebuf_t *msg )
{
	for( int tableIndex = 0; tableIndex < Delta_NumTables(); tableIndex++ )
	{
		delta_info_t *dt = Delta_FindStructByIndex( tableIndex );

		for( int fieldIndex = 0; fieldIndex < dt->numFields; fieldIndex++ )
			Delta_WriteTableField( msg, tableIndex, &dt->pFields[fieldIndex] );
	}
}

/*
=============================================================================

	game.dll interface

=============================================================================
*/
void GAME_EXPORT Delta_AddEncoder( char *name, pfnDeltaEncode encodeFunc )
{
	delta_info_t *dt = Delta_FindStructByEncoder( name );

	if( !dt || !dt->bInitialized )
	{
		Con_DPrintf( S_ERROR "%s: couldn't find delta with specified custom encode %s\n", __func__, name );
		return;
	}

	if( dt->customEncode == CUSTOM_NONE )
	{
		Con_DPrintf( S_ERROR "%s: %s not supposed for custom encoding\n", __func__, dt->pName );
		return;
	}

	// register new encode func
	dt->userCallback = encodeFunc;
}

int GAME_EXPORT Delta_FindField( delta_t *pFields, const char *fieldname )
{
	delta_t *pField;
	int i;

	delta_info_t *dt = Delta_FindStructByDelta( pFields );
	if( dt == NULL || !fieldname || !fieldname[0] )
		return -1;

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		if( !Q_strcmp( pField->name, fieldname ))
			return i;
	}
	return -1;
}

void GAME_EXPORT Delta_SetField( delta_t *pFields, const char *fieldname )
{
	delta_t *pField;
	int i;

	delta_info_t *dt = Delta_FindStructByDelta( pFields );
	if( dt == NULL || !fieldname || !fieldname[0] )
		return;

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		if( !Q_strcmp( pField->name, fieldname ))
		{
			pField->bInactive = false;
			return;
		}
	}
}

void GAME_EXPORT Delta_UnsetField( delta_t *pFields, const char *fieldname )
{
	delta_t *pField;
	int i;

	delta_info_t *dt = Delta_FindStructByDelta( pFields );
	if( dt == NULL || !fieldname || !fieldname[0] )
		return;

	for( i = 0, pField = dt->pFields; i < dt->numFields; i++, pField++ )
	{
		if( !Q_strcmp( pField->name, fieldname ))
		{
			pField->bInactive = true;
			return;
		}
	}
}

void GAME_EXPORT Delta_SetFieldByIndex( delta_t *pFields, int fieldNumber )
{
	delta_info_t *dt = Delta_FindStructByDelta( pFields );
	if( dt == NULL || fieldNumber < 0 || fieldNumber >= dt->numFields )
		return;

	dt->pFields[fieldNumber].bInactive = false;
}

void GAME_EXPORT Delta_UnsetFieldByIndex( delta_t *pFields, int fieldNumber )
{
	delta_info_t *dt = Delta_FindStructByDelta( pFields );
	if( dt == NULL || fieldNumber < 0 || fieldNumber >= dt->numFields )
		return;

	dt->pFields[fieldNumber].bInactive = true;
}

#if XASH_ENGINE_TESTS
#include "tests.h"

void Test_RunDelta( void )
{
	delta_info_t *dt = &dt_info[DT_DELTA_TEST_STRUCT_T];
	delta_test_struct_t from, to = { 0 };
	delta_test_struct_t null = { 0 };
	sizebuf_t msg;
	char buffer[4096] = { 0 };
	const double timebase = 123.123;

	Delta_AddField( dt, "dt_string", DT_STRING, 1, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_timewindow_big", DT_TIMEWINDOW_BIG, 24, 1000.f, 1.0f );
	Delta_AddField( dt, "dt_timewindow_8", DT_TIMEWINDOW_8, 8, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_angle", DT_ANGLE, 16, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_float_signed", DT_FLOAT | DT_SIGNED, 22, 100.0f, 1.0f );
	Delta_AddField( dt, "dt_float_unsigned", DT_FLOAT, 24, 10000.0f, 0.1f );
	Delta_AddField( dt, "dt_integer_signed", DT_INTEGER | DT_SIGNED, 24, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_integer_unsigned", DT_INTEGER, 24, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_short_signed", DT_SHORT | DT_SIGNED, 16, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_short_unsigned", DT_SHORT, 15, 0.125f, 1.0f );
	Delta_AddField( dt, "dt_byte_signed", DT_BYTE | DT_SIGNED, 6, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_byte_unsigned", DT_BYTE, 8, 1.0f, 1.0f );
	Delta_AddField( dt, "dt_integer_signed_mul", DT_INTEGER | DT_SIGNED, 24, 2.0f, 1.0f );
	Delta_AddField( dt, "dt_short_signed_mul", DT_SHORT | DT_SIGNED, 16, 4.0f, 1.0f );
	Delta_AddField( dt, "dt_byte_signed_mul", DT_BYTE | DT_SIGNED, 8, 2.0f, 1.0f );

	Q_strncpy( from.dt_string, "test data check it's the same", sizeof( from.dt_string ));
	from.dt_timewindow_big = timebase + 2.3456;
	from.dt_timewindow_8 = timebase + 0.0234;
	from.dt_angle = 160.245f;
	from.dt_float_signed = -15.123f;
	from.dt_float_unsigned = 1235.321f;
	from.dt_integer_signed = -412784;
	from.dt_integer_unsigned = 123453;
	from.dt_short_signed = -12343;
	from.dt_short_unsigned = 32131;
	from.dt_byte_signed = 16;
	from.dt_byte_unsigned = 218;
	from.dt_integer_signed_mul = -412784;
	from.dt_short_signed_mul = -1234;
	from.dt_byte_signed_mul = -30;

	MSG_Init( &msg, "test message", buffer, sizeof( buffer ));

	for( int i = 0; i < dt->numFields; i++ )
		Delta_WriteField( &msg, &dt->pFields[i], &null, &from, timebase );

	MSG_SeekToBit( &msg, 0, SEEK_SET );

	for( int i = 0; i < dt->numFields; i++ )
		Delta_ReadField( &msg, &dt->pFields[i], &null, &to, timebase );

	Con_Printf( "struct as encoded to delta:\n" );
	TASSERT_STR( from.dt_string, to.dt_string );

	// the epsilon value is derived from multiplier value
	TASSERT( Q_equal_e( from.dt_timewindow_big, to.dt_timewindow_big, 0.001f ));

	// dt_timewindow_8 type has multiplier locked at 100.0f
	TASSERT( Q_equal_e( from.dt_timewindow_8, to.dt_timewindow_8, 0.01f ));
	TASSERT( Q_equal_e( from.dt_angle, to.dt_angle, 0.1f ));
	TASSERT( Q_equal_e( from.dt_float_signed, to.dt_float_signed, 0.01f ));

	// dt_float_unsigned has post-multiplier that doesn't affect network data
	// and therefore should be reverted back when comparing
	TASSERT( Q_equal_e( from.dt_float_unsigned, to.dt_float_unsigned * 10.f , 0.01f ));

	TASSERT_EQi( from.dt_integer_signed, to.dt_integer_signed );
	TASSERT_EQi( from.dt_integer_unsigned, to.dt_integer_unsigned );
	TASSERT_EQi( from.dt_short_signed, to.dt_short_signed );
	TASSERT(( from.dt_short_unsigned & ( 0xffff << 3 )) == to.dt_short_unsigned );
	TASSERT_EQi( from.dt_byte_signed, to.dt_byte_signed );
	TASSERT_EQi( from.dt_byte_unsigned, to.dt_byte_unsigned );
	TASSERT_EQi( from.dt_integer_signed_mul, to.dt_integer_signed_mul );
	TASSERT_EQi( from.dt_short_signed_mul, to.dt_short_signed_mul );
	TASSERT_EQi( from.dt_byte_signed_mul, to.dt_byte_signed_mul );

	Con_Printf( "from.dt_timewindow_big = %f\n", from.dt_timewindow_big );
	Con_Printf( "to.dt_timewindow_big   = %f\n", to.dt_timewindow_big );
	Con_Printf( "from.dt_timewindow_8 = %f\n", from.dt_timewindow_8 );
	Con_Printf( "to.dt_timewindow_8   = %f\n", to.dt_timewindow_8 );
	Con_Printf( "from.dt_angle = %f\n", from.dt_angle );
	Con_Printf( "to.dt_angle   = %f\n", to.dt_angle );
	Con_Printf( "from.dt_float_signed = %f\n", from.dt_float_signed );
	Con_Printf( "to.dt_float_signed   = %f\n", to.dt_float_signed );
	Con_Printf( "from.dt_float_unsigned = %f\n", from.dt_float_unsigned );
	Con_Printf( "to.dt_float_unsigned   = %f\n", to.dt_float_unsigned );
	Con_Printf( "from.dt_integer_signed = %i\n", from.dt_integer_signed );
	Con_Printf( "to.dt_integer_signed   = %i\n", to.dt_integer_signed );
	Con_Printf( "from.dt_integer_unsigned = %i\n", from.dt_integer_unsigned );
	Con_Printf( "to.dt_integer_unsigned   = %i\n", to.dt_integer_unsigned );
	Con_Printf( "from.dt_short_signed = %i\n", from.dt_short_signed );
	Con_Printf( "to.dt_short_signed   = %i\n", to.dt_short_signed );
	Con_Printf( "from.dt_short_unsigned = %i\n", from.dt_short_unsigned );
	Con_Printf( "to.dt_short_unsigned   = %i\n", to.dt_short_unsigned );
	Con_Printf( "from.dt_byte_signed = %i\n", from.dt_byte_signed );
	Con_Printf( "to.dt_byte_signed   = %i\n", to.dt_byte_signed );
	Con_Printf( "from.dt_byte_unsigned = %i\n", from.dt_byte_unsigned );
	Con_Printf( "to.dt_byte_unsigned   = %i\n", to.dt_byte_unsigned );
}
#endif // XASH_ENGINE_TESTS
