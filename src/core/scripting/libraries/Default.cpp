#include "core/scripting/LuaLibrary.hpp"
#include "core/scripting/LuaScript.hpp"
#include "core/filemgr/FileMgr.hpp"

namespace YimMenu::Lua
{
	namespace fs = std::filesystem;

	class Default : LuaLibrary
	{
		using LuaLibrary::LuaLibrary;

		static bool IsPathInside(const fs::path& base, const fs::path& target)
		{
			auto abs_base = fs::weakly_canonical(base);
			auto abs_target = fs::weakly_canonical(target);
			return std::mismatch(abs_base.begin(), abs_base.end(), abs_target.begin()).first == abs_base.end();
		}

		void SetLuaRequirePath(lua_State* state)
		{
			const fs::path scriptsFolder = FileMgr::GetProjectFolder(std::string("./scripts"));
			std::string pathString = (scriptsFolder / "?.lua").string() + ";";

			for (const auto& entry : fs::recursive_directory_iterator(scriptsFolder, fs::directory_options::skip_permission_denied))
			{
				if (!entry.is_directory())
					continue;

				pathString += (entry.path() / "?.lua").string() + ";";
			}

			if (!pathString.empty() && pathString.back() == ';')
				pathString.pop_back();

			lua_getglobal(state, "package");

			if (lua_istable(state, -1))
			{
				lua_pushstring(state, pathString.c_str());
				lua_setfield(state, -2, "path");
			}

			lua_pop(state, 1);
		}

		static void SandboxLuaOSLib(lua_State* state)
		{
			lua_getglobal(state, "os");

			if (!lua_istable(state, -1))
			{
				lua_pop(state, 1);
				return;
			}

			lua_newtable(state);
			const char* allowed[] = {
				"clock",
				"date",
				"difftime",
				"time"
			};

			for (const char* name : allowed)
			{
				lua_getfield(state, -2, name);
				lua_setfield(state, -3, name);
			}

			lua_remove(state, -2);
			lua_setglobal(state, "os");
		}
		
		static int LuaIO_Exists(lua_State* state)
		{
			const char* filename = luaL_checkstring(state, 1);
			const fs::path configPath = FileMgr::GetProjectFolder(std::string("./scripts/config"));
			fs::path fullPath = configPath / filename;

			if (!IsPathInside(configPath, fullPath))
			{
				lua_pushboolean(state, false);
				return 1;
			}

			lua_pushboolean(state, fs::exists(fullPath));
			return 1;
		}

		static int SandboxLuaIO_Open(lua_State* state)
		{
			lua_getfield(state, LUA_REGISTRYINDEX, "sandbox.io.open");

			if (!lua_isfunction(state, -1))
			{
				lua_pop(state, 1);
				lua_pushnil(state);
				lua_pushstring(state, "Original io.open not available");
				return 2;
			}

			const char* filename = luaL_checkstring(state, 1);
			const char* mode = luaL_optstring(state, 2, "r");
			const fs::path configPath = FileMgr::GetProjectFolder(std::string("./scripts/config"));
			const fs::path fullPath = configPath / filename;

			if (!IsPathInside(configPath, fullPath))
			{
				lua_pop(state, 1);
				lua_pushnil(state);
				lua_pushstring(state, "Access denied: I/O pperations are exclusive to the config folder.");
				return 2;
			}

			std::string fullPathStr = fullPath.string();
			lua_pushstring(state, fullPathStr.c_str());
			lua_replace(state, 1);
			lua_call(state, 2, 1); // call default io.open
			return 1; // file handle or nil
		}

		// luaL_requiref
		static void requiref(lua_State* state, const char* name, lua_CFunction openf, int global)
		{
			lua_getfield(state, LUA_REGISTRYINDEX, "_LOADED");
			lua_getfield(state, -1, name);
			if (!lua_toboolean(state, -1))
			{
				lua_pop(state, 1);
				lua_pushcfunction(state, openf);
				lua_pushstring(state, name);
				lua_call(state, 1, 1);
				lua_pushvalue(state, -1);
				lua_setfield(state, -3, name);

				if (global)
					lua_setglobal(state, name);
				else
					lua_pop(state, 1);
			}
			else
			{
				lua_pop(state, 1);
			}
			lua_pop(state, 1);
		}

		void GetOrigLuaIO(lua_State* state)
		{
			// all this bs just to get io.open
			requiref(state, LUA_IOLIBNAME, luaopen_io, 1);
			lua_getfield(state, -1, "open");
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.open");
			lua_getfield(state, -1, "flush");
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.flush");
			lua_getfield(state, -1, "close");
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.close");
			lua_pushnil(state);
			lua_setglobal(state, "io");
			lua_getfield(state, LUA_REGISTRYINDEX, "_LOADED");
			lua_pushnil(state);
			lua_setfield(state, -2, "io");
			lua_pop(state, 1);
		}

		void SandboxLuaIOLib(lua_State* state)
		{
			GetOrigLuaIO(state);
			lua_newtable(state);
			lua_pushcfunction(state, SandboxLuaIO_Open);
			lua_setfield(state, -2, "open");
			lua_getfield(state, LUA_REGISTRYINDEX, "sandbox.io.flush");
			lua_setfield(state, -2, "flush");
			lua_getfield(state, LUA_REGISTRYINDEX, "sandbox.io.close");
			lua_setfield(state, -2, "close");
			lua_pushcfunction(state, LuaIO_Exists);
			lua_setfield(state, -2, "exists");
			lua_setglobal(state, "io");
			lua_pushnil(state);
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.open");
			lua_pushnil(state);
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.flush");
			lua_pushnil(state);
			lua_setfield(state, LUA_REGISTRYINDEX, "sandbox.io.close");
		}

		virtual void Register(lua_State* state) override
		{
			// ensure that only safe libraries are loaded
			static const luaL_Reg lj_lib_load[] = {
			    {"", luaopen_base},
		  //    {LUA_LOADLIBNAME, luaopen_package},
			    {LUA_TABLIBNAME, luaopen_table},
		  //    {LUA_IOLIBNAME, luaopen_io},
	      //    {LUA_OSLIBNAME, luaopen_os},
			    {LUA_STRLIBNAME, luaopen_string},
			    {LUA_MATHLIBNAME, luaopen_math},
			    {LUA_DBLIBNAME, luaopen_debug}, // we have to disable or wrap debuglib
			    {LUA_BITLIBNAME, luaopen_bit},
			    {LUA_JITLIBNAME, luaopen_jit}, // same here? idk
			    {NULL, NULL}
			};

			static const luaL_Reg lj_lib_preload[] = {
		//	    {LUA_FFILIBNAME, luaopen_ffi},
			    {NULL, NULL}
			};

			const luaL_Reg* lib;
			for (lib = lj_lib_load; lib->func; lib++)
			{
				lua_pushcfunction(state, lib->func);
				lua_pushstring(state, lib->name);
				lua_call(state, 1, 0);
			}

			luaL_findtable(state, LUA_REGISTRYINDEX, "_PRELOAD", sizeof(lj_lib_preload) / sizeof(lj_lib_preload[0]) - 1);
			for (lib = lj_lib_preload; lib->func; lib++)
			{
				lua_pushcfunction(state, lib->func);
				lua_setfield(state, -2, lib->name);
			}
			lua_pop(state, 1);

			SandboxLuaOSLib(state);
			SandboxLuaIOLib(state);
		}
	};

	Default _Default;
}