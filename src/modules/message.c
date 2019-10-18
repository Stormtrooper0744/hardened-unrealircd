/*
 *   Unreal Internet Relay Chat Daemon, src/modules/message.c
 *   (C) 2000-2001 Carsten V. Munk and the UnrealIRCd Team
 *   Moved to modules by Fish (Justin Hammond)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

char *_StripColors(unsigned char *text);
char *_StripControlCodes(unsigned char *text);

int ban_version(Client *client, char *text);

CMD_FUNC(cmd_private);
CMD_FUNC(cmd_notice);
void cmd_message(Client *client, MessageTag *recv_mtags, int parc, char *parv[], int notice);
int _can_send_to_channel(Client *client, Channel *channel, char **msgtext, char **errmsg, int notice);
int can_send_to_user(Client *client, Client *target, char **msgtext, char **errmsg, int notice);

/* Place includes here */
#define MSG_PRIVATE     "PRIVMSG"       /* PRIV */
#define MSG_NOTICE      "NOTICE"        /* NOTI */

ModuleHeader MOD_HEADER
  = {
	"message",	/* Name of module */
	"5.0", /* Version */
	"private message and notice", /* Short description of module */
	"UnrealIRCd Team",
	"unrealircd-5",
    };

MOD_TEST()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	EfunctionAddPChar(modinfo->handle, EFUNC_STRIPCOLORS, _StripColors);
	EfunctionAddPChar(modinfo->handle, EFUNC_STRIPCONTROLCODES, _StripControlCodes);
	EfunctionAdd(modinfo->handle, EFUNC_CAN_SEND_TO_CHANNEL, _can_send_to_channel);
	return MOD_SUCCESS;
}

/* This is called on module init, before Server Ready */
MOD_INIT()
{
	CommandAdd(modinfo->handle, MSG_PRIVATE, cmd_private, 2, CMD_USER|CMD_SERVER|CMD_RESETIDLE|CMD_VIRUS);
	CommandAdd(modinfo->handle, MSG_NOTICE, cmd_notice, 2, CMD_USER|CMD_SERVER);
	MARK_AS_OFFICIAL_MODULE(modinfo);
	return MOD_SUCCESS;
}

/* Is first run when server is 100% ready */
MOD_LOAD()
{
	return MOD_SUCCESS;
}

/* Called when module is unloaded */
MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

char *get_dcc_filename(const char *text);
static int can_dcc(Client *client, char *target, Client *targetcli, char *filename, char **errmsg);
static int can_dcc_soft(Client *from, Client *to, char *filename, char **errmsg);

#define CANPRIVMSG_CONTINUE		100
#define CANPRIVMSG_SEND			101
/** Check if PRIVMSG's are permitted from a person to another person.
 * client:	source client
 * target:	target client
 * notice:	1 if notice, 0 if privmsg
 * text:	Pointer to a pointer to a text [in, out]
 * cmd:		Pointer to a pointer which contains the command to use [in, out]
 */
int can_send_to_user(Client *client, Client *target, char **msgtext, char **errmsg, int notice)
{
	int ret;
	Hook *h;
	int n;
	static char errbuf[256];

	*errmsg = NULL;

	if (IsVirus(client))
	{
		ircsnprintf(errbuf, sizeof(errbuf), "You are only allowed to talk in '%s'", SPAMFILTER_VIRUSCHAN);
		*errmsg = errbuf;
		return 0;
	}

	if (**msgtext == '\001')
	{
		char *filename = get_dcc_filename(*msgtext);
		if (filename)
		{
			if (MyUser(client) && !can_dcc(client, target->name, target, filename, errmsg))
				return 0;
			if (MyUser(target) && !can_dcc_soft(client, target, filename, errmsg))
				return 0;
		}
	}

	if (MyUser(client) && target_limit_exceeded(client, target, target->name))
	{
		/* target_limit_exceeded() is an exception, in the sense that
		 * it will send a different numeric. So we don't set errmsg.
		 */
		return 0;
	}

	if (is_silenced(client, target))
	{
		RunHook3(HOOKTYPE_SILENCED, client, target, notice);
		/* Silently discarded, no error message */
		return 0;
	}

	// Possible FIXME: make match_spamfilter also use errmsg, or via a wrapper? or use same numeric?
	if (MyUser(client) && match_spamfilter(client, *msgtext, (notice ? SPAMF_USERNOTICE : SPAMF_USERMSG), target->name, 0, NULL))
		return 0;

	n = HOOK_CONTINUE;
	for (h = Hooks[HOOKTYPE_CAN_SEND_TO_USER]; h; h = h->next)
	{
		n = (*(h->func.intfunc))(client, target, msgtext, errmsg, notice);
		if (n == HOOK_DENY)
		{
			if (!*errmsg)
			{
				ircd_log(LOG_ERROR, "Module %s did not set errmsg!!!", h->owner->header->name);
				abort();
			}
			return 0;
		}
		if (!*msgtext || !**msgtext)
			return 0;
	}

	/* This may happen, if nothing is left to send anymore (don't send empty messages) */
	if (!*msgtext || !**msgtext)
		return 0;

	return 1;
}

#ifdef PREFIX_AQ
 #define PREFIX_REST (PREFIX_ADMIN|PREFIX_OWNER)
#else
 #define PREFIX_REST (0)
#endif

/** Convert a string of prefixes (like "+%@") to values (like PREFIX_VOICE|PREFIX_HALFOP|PREFIX_OP).
 * @param str	The string containing the prefixes and the channel name.
 * @param end	The position of the hashmark (#)
 * @returns A value of PREFIX_*, potentially OR'ed if there are multiple values.
 */
int prefix_string_to_values(char *str, char *end)
{
	char *p;
	int prefix = 0;

	for (p = str; p != end; p++)
	{
		switch (*p)
		{
			case '+':
				prefix |= PREFIX_VOICE | PREFIX_HALFOP | PREFIX_OP | PREFIX_REST;
				break;
			case '%':
				prefix |= PREFIX_HALFOP | PREFIX_OP | PREFIX_REST;
				break;
			case '@':
				prefix |= PREFIX_OP | PREFIX_REST;
				break;
#ifdef PREFIX_AQ
			case '&':
				prefix |= PREFIX_ADMIN | PREFIX_OWNER;
				break;
			case '~':
				prefix |= PREFIX_OWNER;
				break;
#else
			case '&':
				prefix |= PREFIX_OP | PREFIX_REST;
				break;
			case '~':
				prefix |= PREFIX_OP | PREFIX_REST;
				break;
#endif
			default:
				break;	/* ignore it :P */
		}
	}
	return prefix;
}

/** Find out the lowest prefix to use, so @&~#chan becomes @#chan.
 * @param prefix	One or more of PREFIX_* values (OR'ed)
 * @returns A single character
 * @note prefix must be >0, so must contain at least one PREFIX_xx value!
 */
char prefix_values_to_char(int prefix)
{
	if (prefix & PREFIX_VOICE)
		return '+';
	if (prefix & PREFIX_HALFOP)
		return '%';
	if (prefix & PREFIX_OP)
		return '@';
#ifdef PREFIX_AQ
	if (prefix & PREFIX_ADMIN)
		return '&';
	if (prefix & PREFIX_OWNER)
		return '~';
#endif
	abort();
}

/** Check if user is allowed to send to a prefix (eg: @#channel).
 * @param client	The client (sender)
 * @param channel	The target channel
 * @param prefix	The prefix mask (eg: PREFIX_CHANOP)
 */
int can_send_to_prefix(Client *client, Channel *channel, int prefix)
{
	Membership *lp;

	if (op_can_override("channel:override:message:prefix",client,channel,NULL))
		return 1;

	lp = find_membership_link(client->user->channel, channel);

	/* Check if user is allowed to send. RULES:
	 * Need at least voice (+) in order to send to +,% or @
	 * Need at least ops (@) in order to send to & or ~
	 */
	if (!lp || !(lp->flags & (CHFL_VOICE|CHFL_HALFOP|CHFL_CHANOP|CHFL_CHANOWNER|CHFL_CHANADMIN)))
	{
		sendnumeric(client, ERR_CHANOPRIVSNEEDED, channel->chname);
		return 0;
	}

	if (!(prefix & PREFIX_OP) && ((prefix & PREFIX_OWNER) || (prefix & PREFIX_ADMIN)) &&
	    !(lp->flags & (CHFL_CHANOP|CHFL_CHANOWNER|CHFL_CHANADMIN)))
	{
		sendnumeric(client, ERR_CHANOPRIVSNEEDED, channel->chname);
		return 0;
	}

	return 1;
}

/*
** cmd_message (used in cmd_private() and cmd_notice())
** the general function to deliver MSG's between users/channels
**
**	parv[1] = receiver list
**	parv[2] = message text
**
** massive cleanup
** rev argv 6/91
**
*/
void cmd_message(Client *client, MessageTag *recv_mtags, int parc, char *parv[], int notice)
{
	Client *target;
	Channel *channel;
	char *targetstr, *p, *p2, *pc, *text, *errmsg;
	int  prefix = 0;
	char pfixchan[CHANNELLEN + 4];
	int ret;
	int ntargets = 0;
	char *cmd = notice ? "NOTICE" : "PRIVMSG";
	int maxtargets = max_targets_for_command(cmd);
	Hook *h;
	MessageTag *mtags;
	int sendflags;

	/* Force a labeled-response, even if we don't send anything
	 * and the request was sent to other servers (which won't
	 * reply either :D).
	 */
	labeled_response_force = 1;

	if (parc < 2 || *parv[1] == '\0')
	{
		sendnumeric(client, ERR_NORECIPIENT, cmd);
		return;
	}

	if (parc < 3 || *parv[2] == '\0')
	{
		sendnumeric(client, ERR_NOTEXTTOSEND);
		return;
	}

	if (MyConnect(client))
		parv[1] = (char *)canonize(parv[1]);

	for (p = NULL, targetstr = strtoken(&p, parv[1], ","); targetstr; targetstr = strtoken(&p, NULL, ","))
	{
		if (MyUser(client) && (++ntargets > maxtargets))
		{
			sendnumeric(client, ERR_TOOMANYTARGETS, targetstr, maxtargets, cmd);
			break;
		}
		/* The nicks "ircd" and "irc" are special (and reserved) */
		if (!strcasecmp(targetstr, "ircd") && MyUser(client))
			return;

		if (!strcasecmp(targetstr, "irc") && MyUser(client))
		{
			/* When ban version { } is enabled the IRCd sends a CTCP VERSION request
			 * from the "IRC" nick. So we need to handle CTCP VERSION replies to "IRC".
			 */
			if (!strncmp(parv[2], "\1VERSION ", 9))
				ban_version(client, parv[2] + 9);
			else if (!strncmp(parv[2], "\1SCRIPT ", 8))
				ban_version(client, parv[2] + 8);
			return;
		}

		p2 = strchr(targetstr, '#');
		prefix = 0;

		/* Message to channel */
		if (p2 && (channel = find_channel(p2, NULL)))
		{
			prefix = prefix_string_to_values(targetstr, p2);
			if (prefix)
			{
				if (MyUser(client) && !can_send_to_prefix(client, channel, prefix))
					continue;
				/* Now find out the lowest prefix and rewrite the target.
				 * Eg: @&~#chan becomes @#chan
				 */
				pfixchan[0] = prefix_values_to_char(prefix);
				strlcpy(pfixchan+1, channel->chname, sizeof(pfixchan)-1);
				targetstr = pfixchan;
			} else {
				/* Replace target so the privmsg always goes to the "official" channel name */
				strlcpy(pfixchan, channel->chname, sizeof(pfixchan));
				targetstr = pfixchan;
			}

			if (MyUser(client) && (*parv[2] == '\001'))
			{
				char *errmsg = NULL;
				char *filename = get_dcc_filename(parv[2]);
				if (filename && !can_dcc(client, channel->chname, NULL, filename, &errmsg))
				{
					if (IsDead(client))
						return;
					if (!IsDead(client) && !notice)
						sendnumeric(client, ERR_CANNOTSENDTOCHAN, channel->chname, errmsg, p2);
					continue;
				}
			}

			if (IsVirus(client) && strcasecmp(channel->chname, SPAMFILTER_VIRUSCHAN))
			{
				sendnotice(client, "You are only allowed to talk in '%s'", SPAMFILTER_VIRUSCHAN);
				continue;
			}

			text = parv[2];
			errmsg = NULL;
			if (MyUser(client) && !IsULine(client))
			{
				if (!can_send_to_channel(client, channel, &text, &errmsg, notice))
				{
					/* Send the error message, but only if:
					 * 1) The user has not been killed
					 * 2) The user is still in the channel (might be kicked)
					 * 3) It is not a NOTICE
					 */
					if (IsDead(client))
						return;
					if (!IsDead(client) && find_membership_link(client->user->channel, channel) && !notice && errmsg)
						sendnumeric(client, ERR_CANNOTSENDTOCHAN, channel->chname, errmsg, p2);
					continue; /* skip delivery to this target */
				}
			}
			mtags = NULL;
			sendflags = SEND_ALL;

			if (!strchr(CHANCMDPFX,parv[2][0]))
				sendflags |= SKIP_DEAF;

			if ((*parv[2] == '\001') && strncmp(&parv[2][1], "ACTION ", 7))
				sendflags |= SKIP_CTCP;

			if (MyUser(client) && match_spamfilter(client, text, notice ? SPAMF_CHANNOTICE : SPAMF_CHANMSG, channel->chname, 0, NULL))
				return;

			new_message(client, recv_mtags, &mtags);

			RunHook5(HOOKTYPE_PRE_CHANMSG, client, channel, mtags, text, notice);

			if (!text)
			{
				free_message_tags(mtags);
				continue;
			}

			sendto_channel(channel, client, client,
				       prefix, 0, sendflags, mtags,
				       notice ? ":%s NOTICE %s :%s" : ":%s PRIVMSG %s :%s",
				       client->name, targetstr, text);

			RunHook8(HOOKTYPE_CHANMSG, client, channel, sendflags, prefix, targetstr, mtags, text, notice);

			free_message_tags(mtags);

			continue;
		}
		else if (p2)
		{
			sendnumeric(client, ERR_NOSUCHNICK, p2);
			continue;
		}


		/* Message to $servermask */
		if (*targetstr == '$')
		{
			MessageTag *mtags = NULL;

			if (!ValidatePermissionsForPath("chat:notice:global", client, NULL, NULL, NULL))
			{
				/* Apparently no other IRCd does this, but I think it's confusing not to
				 * send an error message, especially with our new privilege system.
				 * Error message could be more descriptive perhaps.
				 */
				sendnumeric(client, ERR_NOPRIVILEGES);
				continue;
			}
			new_message(client, recv_mtags, &mtags);
			sendto_match_butone(IsServer(client->direction) ? client->direction : NULL,
			    client, targetstr + 1,
			    (*targetstr == '#') ? MATCH_HOST :
			    MATCH_SERVER,
			    mtags,
			    ":%s %s %s :%s", client->name, cmd, targetstr, parv[2]);
			free_message_tags(mtags);
			continue;
		}

		/* nickname addressed? */
		target = hash_find_nickatserver(targetstr, NULL);
		if (target)
		{
			char *errmsg = NULL;
			text = parv[2];
			if (!can_send_to_user(client, target, &text, &errmsg, notice))
			{
				/* Message is discarded */
				if (IsDead(client))
					return;
				if (!notice && errmsg)
					sendnumeric(client, ERR_CANTSENDTOUSER, target->name, errmsg);
			} else
			{
				/* We may send the message */
				MessageTag *mtags = NULL;

				/* Inform sender that recipient is away, if this is so */
				if (!notice && MyConnect(client) && target->user && target->user->away)
					sendnumeric(client, RPL_AWAY, target->name, target->user->away);

				new_message(client, recv_mtags, &mtags);
				labeled_response_inhibit = 1;
				if (MyUser(target))
				{
					/* Deliver to end-user */
					sendto_prefix_one(target, client, mtags, ":%s %s %s :%s",
							  client->name, cmd, target->name, text);
				} else {
					/* Send to another server */
					sendto_prefix_one(target, client, mtags, ":%s %s %s :%s",
							  client->id, cmd, target->id, text);
				}
				labeled_response_inhibit = 0;
				RunHook5(HOOKTYPE_USERMSG, client, target, mtags, text, notice);
				free_message_tags(mtags);
				continue;
			}
			continue; /* Message has been delivered or rejected, continue with next target */
		}

		/* If nick@server -and- the @server portion was set::services-server then send a special message */
		if (!target && SERVICES_NAME)
		{
			char *server = strchr(targetstr, '@');
			if (server && strncasecmp(server + 1, SERVICES_NAME, strlen(SERVICES_NAME)) == 0)
			{
				sendnumeric(client, ERR_SERVICESDOWN, targetstr);
				continue;
			}
		}

		/* nothing, nada, not anything found */
		sendnumeric(client, ERR_NOSUCHNICK, targetstr);
		continue;
	}
}

/*
** cmd_private
**	parv[1] = receiver list
**	parv[2] = message text
*/
CMD_FUNC(cmd_private)
{
	cmd_message(client, recv_mtags, parc, parv, 0);
}

/*
** cmd_notice
**	parv[1] = receiver list
**	parv[2] = notice text
*/
CMD_FUNC(cmd_notice)
{
	cmd_message(client, recv_mtags, parc, parv, 1);
}

/** Make a viewable dcc filename.
 * This is to protect a bit against tricks like 'flood-it-off-the-buffer'
 * and color 1,1 etc...
 */
char *dcc_displayfile(char *f)
{
	static char buf[512];
	char *i, *o = buf;
	size_t n = strlen(f);

	if (n < 300)
	{
		for (i = f; *i; i++)
			if (*i < 32)
				*o++ = '?';
			else
				*o++ = *i;
		*o = '\0';
		return buf;
	}

	/* Else, we show it as: [first 256 chars]+"[..TRUNCATED..]"+[last 20 chars] */
	for (i = f; i < f+256; i++)
		if (*i < 32)
			*o++ = '?';
		else
			*o++ = *i;
	strcpy(o, "[..TRUNCATED..]");
	o += sizeof("[..TRUNCATED..]");
	for (i = f+n-20; *i; i++)
		if (*i < 32)
			*o++ = '?';
		else
			*o++ = *i;
	*o = '\0';
	return buf;
}

char *get_dcc_filename(const char *text)
{
	static char filename[BUFSIZE+1];
	char *end;
	int size_string;

	if (*text != '\001')
		return 0;

	if (!strncasecmp(text+1, "DCC SEND ", 9))
		text = text + 10;
	else if (!strncasecmp(text+1, "DCC RESUME ", 11))
		text = text + 12;
	else
		return 0;

	for (; *text == ' '; text++); /* skip leading spaces */

	if (*text == '"' && *(text+1))
		end = strchr(text+1, '"');
	else
		end = strchr(text, ' ');

	if (!end || (end < text))
		return 0;

	size_string = (int)(end - text);

	if (!size_string || (size_string > (BUFSIZE - 1)))
		return 0;

	strlcpy(filename, text, size_string+1);
	return filename;
}

/** Checks if a DCC SEND is allowed.
 * @param client      Sending client
 * @param target      Target name (user or channel)
 * @param targetcli   Target client (NULL in case of channel!)
 * @param text        The entire message
 * @returns 1 if DCC SEND allowed, 0 if rejected
 */
static int can_dcc(Client *client, char *target, Client *targetcli, char *filename, char **errmsg)
{
	ConfigItem_deny_dcc *fl;
	static char errbuf[256];
	int size_string, ret;

	/* User (IRCOp) may bypass send restrictions */
	if (ValidatePermissionsForPath("immune:dcc",client,targetcli,NULL,NULL))
		return 1;

	/* User (IRCOp) likes to receive bad dcc's */
	if (targetcli && ValidatePermissionsForPath("self:getbaddcc",targetcli,NULL,NULL,NULL))
		return 1;

	/* Check if user is already blocked (from the past) */
	if (IsDCCBlock(client))
	{
		*errmsg = "*** You are blocked from sending files as you have tried to "
		          "send a forbidden file - reconnect to regain ability to send";
		return 0;
	}

	if (match_spamfilter(client, filename, SPAMF_DCC, target, 0, NULL))
		return 0;

	if ((fl = dcc_isforbidden(client, filename)))
	{
		char *displayfile = dcc_displayfile(filename);

		RunHook5(HOOKTYPE_DCC_DENIED, client, target, filename, displayfile, fl);

		ircsnprintf(errbuf, sizeof(errbuf), "Cannot DCC SEND file: %s", fl->reason);
		*errmsg = errbuf;
		SetDCCBlock(client);
		return 0;
	}

	/* Channel dcc (???) and discouraged? just block */
	if (!targetcli && ((fl = dcc_isdiscouraged(client, filename))))
	{
		ircsnprintf(errbuf, sizeof(errbuf), "Cannot DCC SEND file: %s", fl->reason);
		*errmsg = errbuf;
		return 0;
	}

	/* If we get here, the file is allowed */
	return 1;
}

/** Checks if a DCC is allowed by DCCALLOW rules (only SOFT bans are checked).
 * PARAMETERS:
 * from:		the sender client (possibly remote)
 * to:			the target client (always local)
 * text:		the whole msg
 * RETURNS:
 * 1:			allowed
 * 0:			block
 */
static int can_dcc_soft(Client *from, Client *to, char *filename, char **errmsg)
{
	ConfigItem_deny_dcc *fl;
	char *displayfile;
	static char errbuf[256];

	/* User (IRCOp) may bypass send restrictions */
	if (ValidatePermissionsForPath("immune:dcc",from,to,NULL,NULL))
		return 1;

	/* User (IRCOp) likes to receive bad dcc's */
	if (ValidatePermissionsForPath("self:getbaddcc",to,NULL,NULL,NULL))
		return 1;

	/* On the 'soft' blocklist ? */
	if (!(fl = dcc_isdiscouraged(from, filename)))
		return 1; /* No, so is OK */

	/* If on DCCALLOW list then the user is OK with it */
	if (on_dccallow_list(to, from))
		return 1;

	/* Soft-blocked */
	displayfile = dcc_displayfile(filename);

	ircsnprintf(errbuf, sizeof(errbuf), "Cannot DCC SEND file: %s", fl->reason);
	*errmsg = errbuf;

	/* Inform target ('to') about the /DCCALLOW functionality */
	sendnotice(to, "%s (%s@%s) tried to DCC SEND you a file named '%s', the request has been blocked.",
		from->name, from->user->username, GetHost(from), displayfile);
	if (!IsDCCNotice(to))
	{
		SetDCCNotice(to);
		sendnotice(to, "Files like these might contain malicious content (viruses, trojans). "
			"Therefore, you must explicitly allow anyone that tries to send you such files.");
		sendnotice(to, "If you trust %s, and want him/her to send you this file, you may obtain "
			"more information on using the dccallow system by typing '/DCCALLOW HELP'", from->name);
	}
	return 0;
}

/* Taken from xchat by Peter Zelezny
 * changed very slightly by codemastr
 * RGB color stripping support added -- codemastr
 */

char *_StripColors(unsigned char *text)
{
	int i = 0, len = strlen(text), save_len=0;
	char nc = 0, col = 0, rgb = 0, *save_text=NULL;
	static unsigned char new_str[4096];

	while (len > 0) 
	{
		if ((col && isdigit(*text) && nc < 2) || (col && *text == ',' && nc < 3)) 
		{
			nc++;
			if (*text == ',')
				nc = 0;
		}
		/* Syntax for RGB is ^DHHHHHH where H is a hex digit.
		 * If < 6 hex digits are specified, the code is displayed
		 * as text
		 */
		else if ((rgb && isxdigit(*text) && nc < 6) || (rgb && *text == ',' && nc < 7))
		{
			nc++;
			if (*text == ',')
				nc = 0;
		}
		else 
		{
			if (col)
				col = 0;
			if (rgb)
			{
				if (nc != 6)
				{
					text = save_text+1;
					len = save_len-1;
					rgb = 0;
					continue;
				}
				rgb = 0;
			}
			if (*text == '\003') 
			{
				col = 1;
				nc = 0;
			}
			else if (*text == '\004')
			{
				save_text = text;
				save_len = len;
				rgb = 1;
				nc = 0;
			}
			else if (*text != '\026') /* (strip reverse too) */
			{
				new_str[i] = *text;
				i++;
			}
		}
		text++;
		len--;
	}
	new_str[i] = 0;
	if (new_str[0] == '\0')
		return NULL;
	return new_str;
}

/* strip color, bold, underline, and reverse codes from a string */
char *_StripControlCodes(unsigned char *text) 
{
	int i = 0, len = strlen(text), save_len=0;
	char nc = 0, col = 0, rgb = 0, *save_text=NULL;
	static unsigned char new_str[4096];
	while (len > 0) 
	{
		if ( col && ((isdigit(*text) && nc < 2) || (*text == ',' && nc < 3)))
		{
			nc++;
			if (*text == ',')
				nc = 0;
		}
		/* Syntax for RGB is ^DHHHHHH where H is a hex digit.
		 * If < 6 hex digits are specified, the code is displayed
		 * as text
		 */
		else if ((rgb && isxdigit(*text) && nc < 6) || (rgb && *text == ',' && nc < 7))
		{
			nc++;
			if (*text == ',')
				nc = 0;
		}
		else 
		{
			if (col)
				col = 0;
			if (rgb)
			{
				if (nc != 6)
				{
					text = save_text+1;
					len = save_len-1;
					rgb = 0;
					continue;
				}
				rgb = 0;
			}
			switch (*text)
			{
			case 3:
				/* color */
				col = 1;
				nc = 0;
				break;
			case 4:
				/* RGB */
				save_text = text;
				save_len = len;
				rgb = 1;
				nc = 0;
				break;
			case 2:
				/* bold */
				break;
			case 31:
				/* underline */
				break;
			case 22:
				/* reverse */
				break;
			case 15:
				/* plain */
				break;
			case 29:
				/* italic */
				break;
			case 30:
				/* strikethrough */
				break;
			case 17:
				/* monospace */
				break;
			case 0xe2:
				if (!strncmp(text+1, "\x80\x8b", 2))
				{
					/* +2 means we skip 3 */
					text += 2;
					len  -= 2;
					break;
				}
				/*fallthrough*/
			default:
				new_str[i] = *text;
				i++;
				break;
			}
		}
		text++;
		len--;
	}
	new_str[i] = 0;
	return new_str;
}

/** Check ban version { } blocks, returns 1  if banned and  0 if not. */
int ban_version(Client *client, char *text)
{
	int len;
	ConfigItem_ban *ban;

	len = strlen(text);
	if (!len)
		return 0;

	if (text[len-1] == '\1')
		text[len-1] = '\0'; /* remove CTCP REPLY terminator (ASCII 1) */

	if ((ban = Find_ban(NULL, text, CONF_BAN_VERSION)))
	{
		if (IsSoftBanAction(ban->action) && IsLoggedIn(client))
			return 0; /* soft ban does not apply to us, we are logged in */

		if (find_tkl_exception(TKL_BAN_VERSION, client))
			return 0; /* we are exempt */

		place_host_ban(client, ban->action, ban->reason, BAN_VERSION_TKL_TIME);
		return 1;
	}

	return 0;
}

/** Can user send a message to this channel?
 * @param client    The client
 * @param channel   The channel
 * @param msgtext The message to send (MAY be changed, even if user is allowed to send)
 * @param errmsg  The error message (will be filled in)
 * @param notice  If it's a NOTICE then this is set to 1. Set to 0 for PRIVMSG.
 * @returns Returns 1 if the user is allowed to send, otherwise 0.
 * (note that this behavior was reversed in UnrealIRCd versions <5.x.
 */
int _can_send_to_channel(Client *client, Channel *channel, char **msgtext, char **errmsg, int notice)
{
	Membership *lp;
	int  member, i = 0;
	Hook *h;

	if (!MyUser(client))
		return 1;

	*errmsg = NULL;

	member = IsMember(client, channel);

	if (channel->mode.mode & MODE_NOPRIVMSGS && !member)
	{
		/* Channel does not accept external messages (+n).
		 * Reject, unless HOOKTYPE_CAN_BYPASS_NO_EXTERNAL_MSGS tells otherwise.
		 */
		for (h = Hooks[HOOKTYPE_CAN_BYPASS_CHANNEL_MESSAGE_RESTRICTION]; h; h = h->next)
		{
			i = (*(h->func.intfunc))(client, channel, BYPASS_CHANMSG_EXTERNAL);
			if (i != HOOK_CONTINUE)
				break;
		}
		if (i != HOOK_ALLOW)
		{
			*errmsg = "No external channel messages";
			return 0;
		}
	}

	lp = find_membership_link(client->user->channel, channel);
	if (channel->mode.mode & MODE_MODERATED &&
	    !op_can_override("channel:override:message:moderated",client,channel,NULL) &&
	    (!lp /* FIXME: UGLY */
	    || !(lp->flags & (CHFL_CHANOP | CHFL_VOICE | CHFL_CHANOWNER | CHFL_HALFOP | CHFL_CHANADMIN))))
	{
		/* Channel is moderated (+m).
		 * Reject, unless HOOKTYPE_CAN_BYPASS_MODERATED tells otherwise.
		 */
		for (h = Hooks[HOOKTYPE_CAN_BYPASS_CHANNEL_MESSAGE_RESTRICTION]; h; h = h->next)
		{
			i = (*(h->func.intfunc))(client, channel, BYPASS_CHANMSG_MODERATED);
			if (i != HOOK_CONTINUE)
				break;
		}
		if (i != HOOK_ALLOW)
		{
			*errmsg = "You need voice (+v)";
			return 0;
		}
	}

	/* Modules can plug in as well */
	for (h = Hooks[HOOKTYPE_CAN_SEND_TO_CHANNEL]; h; h = h->next)
	{
		i = (*(h->func.intfunc))(client, channel, lp, msgtext, errmsg, notice);
		if (i != HOOK_CONTINUE)
		{
			if (!*errmsg)
			{
				ircd_log(LOG_ERROR, "Module %s did not set errmsg!!!", h->owner->header->name);
				abort();
			}
			break;
		}
		if (!*msgtext || !**msgtext)
			return 0;
	}

	if (i != HOOK_CONTINUE)
	{
		if (!*errmsg)
			*errmsg = "You are banned";
		return 0;
	}
	if (!*msgtext || !**msgtext)
		return 0;


	/* Now we are going to check bans */

	/* ..but first: exempt ircops */
	if (op_can_override("channel:override:message:ban",client,channel,NULL))
		return 1;

	if ((!lp
	    || !(lp->flags & (CHFL_CHANOP | CHFL_VOICE | CHFL_CHANOWNER |
	    CHFL_HALFOP | CHFL_CHANADMIN))) && MyUser(client)
	    && is_banned(client, channel, BANCHK_MSG, msgtext, errmsg))
	{
		/* Modules can set 'errmsg', otherwise we default to this: */
		if (!*errmsg)
			*errmsg = "You are banned";
		return 0;
	}

	return 1;
}
