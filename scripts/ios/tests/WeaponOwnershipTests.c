#include "../../../engine/client/dll_int/cl_game.c"
#include <assert.h>

client_t cl;
client_static_t cls;
convar_t cl_weaponlistfix = {.value = 2};

void Con_Printf(const char *format, ...) { (void)format; }
void Con_DPrintf(const char *format, ...) { (void)format; }
int Cvar_VariableInteger(const char *name) { (void)name; return 0; }
void Cvar_DirectSet(convar_t *variable, const char *value) { (void)variable; (void)value; }
void Cvar_Set(const char *name, const char *value) { (void)name; (void)value; }
void DBG_AssertFunction(qboolean expression, const char *text, const char *file, int line, const char *message) {
    (void)text; (void)file; (void)line; (void)message;
    assert(expression);
}

static void Ammo(int index, int count) {
    byte message[] = {index, count & 255, (count >> 8) & 255, (count >> 16) & 255, (count >> 24) & 255};
    CL_WeaponListFix_OnUserMessage("AmmoX", sizeof(message), message);
}

static void Current(int id, int clip) {
    byte message[] = {1, id & 255, id >> 8, clip & 255, (clip >> 8) & 255,
                      (clip >> 16) & 255, (clip >> 24) & 255, 0, 0, 0, 0};
    CL_WeaponListFix_OnUserMessage("CurWeapon", sizeof(message), message);
}

int main(void) {
    weapon_data_t snapshot[CL_WEAPONLISTFIX_MAX_WEAPONS] = {0};
    cl.time = 10;
    CL_WeaponListFix_Reset();
    CL_WeaponListFix_AddNamedWeapon(1, "weapon_test_throwable");
    CL_WeaponListFix_AddNamedWeapon(2, "weapon_test_firearm");
    cl_weaponlistfix_state.weapons[1].flags = BIT(4);
    cl_weaponlistfix_state.weapons[1].ammo1 = 7;
    cl_weaponlistfix_state.weapons[2].ammo1 = 8;
    Current(1, -1);
    Ammo(7, 0);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    Current(2, 0);
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    Ammo(8, 0);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[2]));

    cl_weaponlistfix_state.weapons[1].owned_hint = true;
    cl_weaponlistfix_state.weapons[1].flags |= CL_WEAPONLISTFIX_SELECTONEMPTY;
    Ammo(7, 0);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    cl_weaponlistfix_state.weapons[1].flags &= ~CL_WEAPONLISTFIX_SELECTONEMPTY;
    cl_weaponlistfix_state.weapons[1].clip = 1;
    Ammo(7, 0);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    cl_weaponlistfix_state.weapons[1].clip = -1;
    cl_weaponlistfix_state.weapons[1].ammo2 = 9;
    Ammo(7, 0);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    Ammo(9, 1);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    Ammo(9, 0);
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    cl_weaponlistfix_state.weapons[1].owned_hint = true;
    Ammo(7, 2);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    snapshot[1].m_iId = 1;
    snapshot[2].m_iId = 2;
    snapshot[200].m_iId = 200;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(cl_weaponlistfix_state.weapondata_valid);
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[200]));
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[2]));

    snapshot[1].m_iId = 0;
    cl_weaponlistfix_state.active_weapon = 1;
    cl_weaponlistfix_state.selected_weapon = cl_weaponlistfix_state.select_pending = 1;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[1]));
    assert(cl_weaponlistfix_state.active_weapon == -1);
    assert(cl_weaponlistfix_state.selected_weapon == -1 && !cl_weaponlistfix_state.select_pending);
    snapshot[200].m_iId = 0;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[200]));
    snapshot[200].m_iId = 200;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[200]));

    cl_weaponlistfix_state.drop_pending = 200;
    cl_weaponlistfix_state.drop_pending_time = cl.time;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[200]));
    snapshot[200].m_iId = 0;
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(!cl_weaponlistfix_state.drop_pending);
    memset(snapshot, 0, sizeof(snapshot));
    CL_WeaponListFix_UpdateWeaponData(snapshot, ARRAYSIZE(snapshot));
    assert(!CL_WeaponListFix_HasWeapon(&cl_weaponlistfix_state.weapons[2]));

    CL_WeaponListFix_OnResetHUD();
    assert(!cl_weaponlistfix_state.ammo_valid[7]);
    CL_WeaponListFix_Reset();
    assert(!cl_weaponlistfix_state.weapondata_valid);
    return 0;
}
