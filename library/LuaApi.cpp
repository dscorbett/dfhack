﻿/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2011 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "Internal.h"

#include <string>
#include <vector>
#include <map>

#include "MemAccess.h"
#include "Core.h"
#include "VersionInfo.h"
#include "tinythread.h"
// must be last due to MS stupidity
#include "DataDefs.h"
#include "DataIdentity.h"
#include "DataFuncs.h"

#include "modules/World.h"
#include "modules/Gui.h"
#include "modules/Job.h"
#include "modules/Translation.h"
#include "modules/Units.h"

#include "LuaWrapper.h"
#include "LuaTools.h"

#include "MiscUtils.h"

#include "df/job.h"
#include "df/job_item.h"
#include "df/building.h"
#include "df/unit.h"
#include "df/item.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

using namespace DFHack;
using namespace DFHack::LuaWrapper;

/**************************************************
 * Per-world persistent configuration storage API *
 **************************************************/

static PersistentDataItem persistent_by_struct(lua_State *state, int idx)
{
    lua_getfield(state, idx, "entry_id");
    int id = lua_tointeger(state, -1);
    lua_pop(state, 1);

    PersistentDataItem ref = Core::getInstance().getWorld()->GetPersistentData(id);

    if (ref.isValid())
    {
        lua_getfield(state, idx, "key");
        const char *str = lua_tostring(state, -1);
        if (!str || str != ref.key())
            luaL_argerror(state, idx, "inconsistent id and key");
        lua_pop(state, 1);
    }

    return ref;
}

static int read_persistent(lua_State *state, PersistentDataItem ref, bool create)
{
    if (!ref.isValid())
    {
        lua_pushnil(state);
        lua_pushstring(state, "entry not found");
        return 2;
    }

    if (create)
        lua_createtable(state, 0, 4);

    lua_pushvalue(state, lua_upvalueindex(1));
    lua_setmetatable(state, -2);

    lua_pushinteger(state, ref.entry_id());
    lua_setfield(state, -2, "entry_id");
    lua_pushstring(state, ref.key().c_str());
    lua_setfield(state, -2, "key");
    lua_pushstring(state, ref.val().c_str());
    lua_setfield(state, -2, "value");

    lua_createtable(state, PersistentDataItem::NumInts, 0);
    for (int i = 0; i < PersistentDataItem::NumInts; i++)
    {
        lua_pushinteger(state, ref.ival(i));
        lua_rawseti(state, -2, i+1);
    }
    lua_setfield(state, -2, "ints");

    return 1;
}

static PersistentDataItem get_persistent(lua_State *state)
{
    luaL_checkany(state, 1);

    if (lua_istable(state, 1))
    {
        if (!lua_getmetatable(state, 1) ||
            !lua_rawequal(state, -1, lua_upvalueindex(1)))
            luaL_argerror(state, 1, "invalid table type");

        lua_settop(state, 1);

        return persistent_by_struct(state, 1);
    }
    else
    {
        const char *str = luaL_checkstring(state, 1);

        return Core::getInstance().getWorld()->GetPersistentData(str);
    }
}

static int dfhack_persistent_get(lua_State *state)
{
    CoreSuspender suspend;

    auto ref = get_persistent(state);

    return read_persistent(state, ref, !lua_istable(state, 1));
}

static int dfhack_persistent_delete(lua_State *state)
{
    CoreSuspender suspend;

    auto ref = get_persistent(state);

    bool ok = Core::getInstance().getWorld()->DeletePersistentData(ref);

    lua_pushboolean(state, ok);
    return 1;
}

static int dfhack_persistent_get_all(lua_State *state)
{
    CoreSuspender suspend;

    const char *str = luaL_checkstring(state, 1);
    bool prefix = (lua_gettop(state)>=2 ? lua_toboolean(state,2) : false);

    std::vector<PersistentDataItem> data;
    Core::getInstance().getWorld()->GetPersistentData(&data, str, prefix);

    if (data.empty())
    {
        lua_pushnil(state);
    }
    else
    {
        lua_createtable(state, data.size(), 0);
        for (size_t i = 0; i < data.size(); ++i)
        {
            read_persistent(state, data[i], true);
            lua_rawseti(state, -2, i+1);
        }
    }

    return 1;
}

static int dfhack_persistent_save(lua_State *state)
{
    CoreSuspender suspend;

    lua_settop(state, 2);
    luaL_checktype(state, 1, LUA_TTABLE);
    bool add = lua_toboolean(state, 2);

    lua_getfield(state, 1, "key");
    const char *str = lua_tostring(state, -1);
    if (!str)
        luaL_argerror(state, 1, "no key field");

    lua_settop(state, 1);

    // Retrieve existing or create a new entry
    PersistentDataItem ref;
    bool added = false;

    if (add)
    {
        ref = Core::getInstance().getWorld()->AddPersistentData(str);
        added = true;
    }
    else if (lua_getmetatable(state, 1))
    {
        if (!lua_rawequal(state, -1, lua_upvalueindex(1)))
            return luaL_argerror(state, 1, "invalid table type");
        lua_pop(state, 1);

        ref = persistent_by_struct(state, 1);
    }
    else
    {
        ref = Core::getInstance().getWorld()->GetPersistentData(str);
    }

    // Auto-add if not found
    if (!ref.isValid())
    {
        ref = Core::getInstance().getWorld()->AddPersistentData(str);
        if (!ref.isValid())
            luaL_error(state, "cannot create persistent entry");
        added = true;
    }

    // Copy data from lua to C++ memory
    lua_getfield(state, 1, "value");
    if (const char *str = lua_tostring(state, -1))
        ref.val() = str;
    lua_pop(state, 1);

    lua_getfield(state, 1, "ints");
    if (lua_istable(state, -1))
    {
        for (int i = 0; i < PersistentDataItem::NumInts; i++)
        {
            lua_rawgeti(state, -1, i+1);
            if (lua_isnumber(state, -1))
                ref.ival(i) = lua_tointeger(state, -1);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    // Reinitialize lua from C++ and return
    read_persistent(state, ref, false);
    lua_pushboolean(state, added);
    return 2;
}

static const luaL_Reg dfhack_persistent_funcs[] = {
    { "get", dfhack_persistent_get },
    { "delete", dfhack_persistent_delete },
    { "get_all", dfhack_persistent_get_all },
    { "save", dfhack_persistent_save },
    { NULL, NULL }
};

static void OpenPersistent(lua_State *state)
{
    luaL_getsubtable(state, lua_gettop(state), "persistent");

    lua_dup(state);
    luaL_setfuncs(state, dfhack_persistent_funcs, 1);

    lua_dup(state);
    lua_setfield(state, -2, "__index");

    lua_pop(state, 1);
}

/************************
 * Wrappers for C++ API *
 ************************/

static void OpenModule(lua_State *state, const char *mname, const LuaWrapper::FunctionReg *reg)
{
    luaL_getsubtable(state, lua_gettop(state), mname);
    LuaWrapper::SetFunctionWrappers(state, reg);
    lua_pop(state, 1);
}

#define WRAPM(module, function) { #function, df::wrap_function(&module::function) }
#define WRAP(function) { #function, df::wrap_function(&function) }
#define WRAPN(name, function) { #name, df::wrap_function(&function) }

static const LuaWrapper::FunctionReg dfhack_module[] = {
    WRAPM(Translation, TranslateName),
    { NULL, NULL }
};

static const LuaWrapper::FunctionReg dfhack_gui_module[] = {
    WRAPM(Gui, getSelectedWorkshopJob),
    WRAPM(Gui, getSelectedJob),
    WRAPM(Gui, getSelectedUnit),
    WRAPM(Gui, getSelectedItem),
    WRAPM(Gui, showAnnouncement),
    WRAPM(Gui, showPopupAnnouncement),
    { NULL, NULL }
};

static bool jobEqual(df::job *job1, df::job *job2) { return *job1 == *job2; }
static bool jobItemEqual(df::job_item *job1, df::job_item *job2) { return *job1 == *job2; }

static const LuaWrapper::FunctionReg dfhack_job_module[] = {
    WRAP(cloneJobStruct),
    WRAP(printJobDetails),
    WRAP(getJobHolder),
    WRAPN(is_equal, jobEqual),
    WRAPN(is_item_equal, jobItemEqual),
    { NULL, NULL }
};

static const LuaWrapper::FunctionReg dfhack_units_module[] = {
    WRAPM(Units, getVisibleName),
    WRAPM(Units, isDead),
    WRAPM(Units, isAlive),
    WRAPM(Units, isSane),
    { NULL, NULL }
};

/************************
 *  Main Open function  *
 ************************/

void OpenDFHackApi(lua_State *state)
{
    OpenPersistent(state);

    LuaWrapper::SetFunctionWrappers(state, dfhack_module);
    OpenModule(state, "gui", dfhack_gui_module);
    OpenModule(state, "job", dfhack_job_module);
    OpenModule(state, "units", dfhack_units_module);
}
