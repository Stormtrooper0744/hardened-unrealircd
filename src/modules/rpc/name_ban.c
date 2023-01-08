/* name_ban.* RPC calls
 * (C) Copyright 2022-.. Bram Matthys (Syzop) and the UnrealIRCd team
 * License: GPLv2 or later
 */

#include "unrealircd.h"

ModuleHeader MOD_HEADER
= {
	"rpc/name_ban",
	"1.0.0",
	"name_ban.* RPC calls",
	"UnrealIRCd Team",
	"unrealircd-6",
};

/* Forward declarations */
RPC_CALL_FUNC(rpc_name_ban_list);
RPC_CALL_FUNC(rpc_name_ban_get);
RPC_CALL_FUNC(rpc_name_ban_del);
RPC_CALL_FUNC(rpc_name_ban_add);

MOD_INIT()
{
	RPCHandlerInfo r;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	memset(&r, 0, sizeof(r));
	r.method = "name_ban.list";
	r.call = rpc_name_ban_list;
	if (!RPCHandlerAdd(modinfo->handle, &r))
	{
		config_error("[rpc/name_ban] Could not register RPC handler");
		return MOD_FAILED;
	}
	r.method = "name_ban.get";
	r.call = rpc_name_ban_get;
	if (!RPCHandlerAdd(modinfo->handle, &r))
	{
		config_error("[rpc/name_ban] Could not register RPC handler");
		return MOD_FAILED;
	}
	r.method = "name_ban.del";
	r.call = rpc_name_ban_del;
	if (!RPCHandlerAdd(modinfo->handle, &r))
	{
		config_error("[rpc/name_ban] Could not register RPC handler");
		return MOD_FAILED;
	}
	r.method = "name_ban.add";
	r.call = rpc_name_ban_add;
	if (!RPCHandlerAdd(modinfo->handle, &r))
	{
		config_error("[rpc/name_ban] Could not register RPC handler");
		return MOD_FAILED;
	}

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

RPC_CALL_FUNC(rpc_name_ban_list)
{
	json_t *result, *list, *item;
	int index;
	TKL *tkl;

	result = json_object();
	list = json_array();
	json_object_set_new(result, "list", list);

	for (index = 0; index < TKLISTLEN; index++)
	{
		for (tkl = tklines[index]; tkl; tkl = tkl->next)
		{
			if (TKLIsNameBan(tkl))
			{
				item = json_object();
				json_expand_tkl(item, NULL, tkl, 1);
				json_array_append_new(list, item);
			}
		}
	}

	rpc_response(client, request, result);
	json_decref(result);
}

TKL *my_find_tkl_nameban(const char *name)
{
	TKL *tkl;

	for (tkl = tklines[tkl_hash('Q')]; tkl; tkl = tkl->next)
	{
		if (!TKLIsNameBan(tkl))
			continue;
		if (!strcasecmp(name, tkl->ptr.nameban->name))
			return tkl;
	}
	return NULL;
}

RPC_CALL_FUNC(rpc_name_ban_get)
{
	json_t *result, *list, *item;
	const char *name;
	TKL *tkl;

	REQUIRE_PARAM_STRING("name", name);

	if (!(tkl = my_find_tkl_nameban(name)))
	{
		rpc_error(client, request, JSON_RPC_ERROR_NOT_FOUND, "Ban not found");
		return;
	}

	result = json_object();
	json_expand_tkl(result, "tkl", tkl, 1);
	rpc_response(client, request, result);
	json_decref(result);
}

RPC_CALL_FUNC(rpc_name_ban_del)
{
	json_t *result, *list, *item;
	const char *name;
	const char *error;
	char *usermask, *hostmask;
	int soft;
	TKL *tkl;
	char tkl_type_char;
	int tkl_type_int;
	const char *tkllayer[10];

	REQUIRE_PARAM_STRING("name", name);

	if (!(tkl = my_find_tkl_nameban(name)))
	{
		rpc_error(client, request, JSON_RPC_ERROR_NOT_FOUND, "Ban not found");
		return;
	}

	result = json_object();
	json_expand_tkl(result, "tkl", tkl, 1);

	tkllayer[0] = NULL;
	tkllayer[1] = "-";
	tkllayer[2] = "Q";
	tkllayer[3] = "*";
	tkllayer[4] = name;
	tkllayer[5] = client->name;
	tkllayer[6] = NULL;
	cmd_tkl(&me, NULL, 6, tkllayer);

	if (!my_find_tkl_nameban(name))
	{
		rpc_response(client, request, result);
	} else {
		/* Actually this may not be an internal error, it could be an
		 * incorrect request, such as asking to remove a config-based ban.
		 */
		rpc_error(client, request, JSON_RPC_ERROR_INTERNAL_ERROR, "Unable to remove item");
	}
	json_decref(result);
}

RPC_CALL_FUNC(rpc_name_ban_add)
{
	json_t *result, *list, *item;
	const char *name;
	const char *str;
	TKL *tkl;
	const char *reason;
	time_t tkl_expire_at;
	time_t tkl_set_at = TStime();

	REQUIRE_PARAM_STRING("name", name);
	REQUIRE_PARAM_STRING("reason", reason);

	/* Duration / expiry time */
	if ((str = json_object_get_string(params, "duration_string")))
	{
		tkl_expire_at = config_checkval(str, CFG_TIME);
		if (tkl_expire_at > 0)
			tkl_expire_at = TStime() + tkl_expire_at;
	} else
	if ((str = json_object_get_string(params, "expire_at")))
	{
		tkl_expire_at = server_time_to_unix_time(str);
	} else
	{
		/* Never expire */
		tkl_expire_at = 0;
	}

	if ((tkl_expire_at != 0) && (tkl_expire_at < TStime()))
	{
		rpc_error_fmt(client, request, JSON_RPC_ERROR_INVALID_PARAMS, "Error: the specified expiry time is before current time (before now)");
		return;
	}

	if (my_find_tkl_nameban(name))
	{
		rpc_error(client, request, JSON_RPC_ERROR_NOT_FOUND, "Ban already exists");
		return;
	}

	tkl = tkl_add_nameban(TKL_NAME|TKL_GLOBAL, name, 0, reason,
	                        client->name, tkl_expire_at, tkl_set_at,
	                        0);

	if (!tkl)
	{
		rpc_error(client, request, JSON_RPC_ERROR_INTERNAL_ERROR, "Unable to add item");
		return;
	}

	tkl_added(client, tkl);

	result = json_object();
	json_expand_tkl(result, "tkl", tkl, 1);
	rpc_response(client, request, result);
	json_decref(result);
}
