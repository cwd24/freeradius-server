/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 if the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_sql.c
 * @brief Implements SQL 'users' file, and SQL accounting.
 *
 * @copyright 2012  Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2000,2006  The FreeRADIUS server project
 * @copyright 2000  Mike Machado <mike@innercite.com>
 * @copyright 2000  Alan DeKok <aland@ox.org>
 */
RCSID("$Id$")

#include <ctype.h>

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/token.h>
#include <freeradius-devel/rad_assert.h>

#include <sys/stat.h>

#include "rlm_sql.h"

/*
 *	So we can do pass2 xlat checks on the queries.
 */
static const CONF_PARSER query_config[] = {

	{ "query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_MULTI, rlm_sql_config_t, accounting.query), NULL },

	{NULL, -1, 0, NULL, NULL}
};

/*
 *	For now hard-code the subsections.  This isn't perfect, but it
 *	helps the average case.
 */
static const CONF_PARSER type_config[] = {

	{ "accounting-on", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) query_config },
	{ "accounting-off", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) query_config },
	{ "start", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) query_config },
	{ "interim-update", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) query_config },
	{ "stop", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) query_config },

	{NULL, -1, 0, NULL, NULL}
};



static const CONF_PARSER acct_config[] = {
	{ "reference", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT, rlm_sql_config_t, accounting.reference), ".query" },
	{ "logfile", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, accounting.logfile), NULL },

	{ "type", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) type_config },

	{NULL, -1, 0, NULL, NULL}
};

static const CONF_PARSER postauth_config[] = {
	{ "reference", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT, rlm_sql_config_t, postauth.reference), ".query" },
	{ "logfile", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, postauth.logfile), NULL },

	{ "query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_MULTI, rlm_sql_config_t, postauth.query), NULL },

	{NULL, -1, 0, NULL, NULL}
};

static const CONF_PARSER module_config[] = {
	{ "driver", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, sql_driver_name), "rlm_sql_null" },
	{ "server", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, sql_server), "localhost" },
	{ "port", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, sql_port), "" },
	{ "login", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, sql_login), "" },
	{ "password", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_SECRET, rlm_sql_config_t, sql_password), "" },
	{ "radius_db", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, sql_db), "radius" },
	{ "read_groups", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_sql_config_t, read_groups), "yes" },
	{ "read_profiles", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_sql_config_t, read_profiles), "yes" },
	{ "readclients", FR_CONF_OFFSET(PW_TYPE_BOOLEAN | PW_TYPE_DEPRECATED, rlm_sql_config_t, do_clients), NULL },
	{ "read_clients", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_sql_config_t, do_clients), "no" },
	{ "deletestalesessions", FR_CONF_OFFSET(PW_TYPE_BOOLEAN | PW_TYPE_DEPRECATED, rlm_sql_config_t, deletestalesessions), NULL },
	{ "delete_stale_sessions", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_sql_config_t, deletestalesessions), "yes" },
	{ "sql_user_name", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, query_user), "" },
	{ "logfile", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, logfile), NULL },
	{ "default_user_profile", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, default_profile), "" },
	{ "nas_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_DEPRECATED, rlm_sql_config_t, client_query), NULL },
	{ "client_query", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, client_query), "SELECT id,nasname,shortname,type,secret FROM nas" },
	{ "open_query", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, open_query), NULL },

	{ "authorize_check_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_check_query), NULL },
	{ "authorize_reply_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_reply_query), NULL },

	{ "authorize_group_check_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_group_check_query), NULL },
	{ "authorize_group_reply_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_group_reply_query), NULL },
	{ "group_membership_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, groupmemb_query), NULL },
#ifdef WITH_SESSION_MGMT
	{ "simul_count_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, simul_count_query), NULL },
	{ "simul_verify_query", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_XLAT | PW_TYPE_NOT_EMPTY, rlm_sql_config_t, simul_verify_query), NULL },
#endif
	{ "safe-characters", FR_CONF_OFFSET(PW_TYPE_STRING | PW_TYPE_DEPRECATED, rlm_sql_config_t, allowed_chars), NULL },
	{ "safe_characters", FR_CONF_OFFSET(PW_TYPE_STRING, rlm_sql_config_t, allowed_chars), "@abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_: /" },

	/*
	 *	This only works for a few drivers.
	 */
	{ "query_timeout", FR_CONF_OFFSET(PW_TYPE_INTEGER, rlm_sql_config_t, query_timeout), NULL },

	{ "accounting", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) acct_config },

	{ "post-auth", FR_CONF_POINTER(PW_TYPE_SUBSECTION, NULL), (void const *) postauth_config },

	{NULL, -1, 0, NULL, NULL}
};

/*
 *	Fall-Through checking function from rlm_files.c
 */
static sql_fall_through_t fall_through(VALUE_PAIR *vp)
{
	VALUE_PAIR *tmp;
	tmp = pairfind(vp, PW_FALL_THROUGH, 0, TAG_ANY);

	return tmp ? tmp->vp_integer : FALL_THROUGH_DEFAULT;
}

/*
 *	Yucky prototype.
 */
static int generate_sql_clients(rlm_sql_t *inst);
static size_t sql_escape_func(REQUEST *, char *out, size_t outlen, char const *in, void *arg);

/*
 *			SQL xlat function
 *
 *  For selects the first value of the first column will be returned,
 *  for inserts, updates and deletes the number of rows affected will be
 *  returned instead.
 */
static ssize_t sql_xlat(void *instance, REQUEST *request, char const *query, char *out, size_t freespace)
{
	rlm_sql_handle_t *handle = NULL;
	rlm_sql_row_t row;
	rlm_sql_t *inst = instance;
	ssize_t ret = 0;
	size_t len = 0;

	/*
	 *	Add SQL-User-Name attribute just in case it is needed
	 *	We could search the string fmt for SQL-User-Name to see if this is
	 * 	needed or not
	 */
	sql_set_user(inst, request, NULL);

	handle = fr_connection_get(inst->pool);
	if (!handle) {
		return 0;
	}

	rlm_sql_query_log(inst, request, NULL, query);

	/*
	 *	If the query starts with any of the following prefixes,
	 *	then return the number of rows affected
	 */
	if ((strncasecmp(query, "insert", 6) == 0) ||
	    (strncasecmp(query, "update", 6) == 0) ||
	    (strncasecmp(query, "delete", 6) == 0)) {
		int numaffected;
		char buffer[21]; /* 64bit max is 20 decimal chars + null byte */

		if (rlm_sql_query(&handle, inst, query) != RLM_SQL_OK) {
			char const *error = (inst->module->sql_error)(handle, inst->config);
			REDEBUG("SQL query failed: %s", error);

			ret = -1;
			goto finish;
		}

		numaffected = (inst->module->sql_affected_rows)(handle, inst->config);
		if (numaffected < 1) {
			RDEBUG("SQL query affected no rows");

			goto finish;
		}

		/*
		 *	Don't chop the returned number if freespace is
		 *	too small.  This hack is necessary because
		 *	some implementations of snprintf return the
		 *	size of the written data, and others return
		 *	the size of the data they *would* have written
		 *	if the output buffer was large enough.
		 */
		snprintf(buffer, sizeof(buffer), "%d", numaffected);

		len = strlen(buffer);
		if (len >= freespace){
			RDEBUG("rlm_sql (%s): Can't write result, insufficient string space", inst->config->xlat_name);

			(inst->module->sql_finish_query)(handle, inst->config);

			ret = -1;
			goto finish;
		}

		memcpy(out, buffer, len + 1); /* we did bounds checking above */
		ret = len;

		(inst->module->sql_finish_query)(handle, inst->config);

		goto finish;
	} /* else it's a SELECT statement */

	if (rlm_sql_select_query(&handle, inst, query) != RLM_SQL_OK) {
		ret = -1;  /* error handled by rlm_sql_select_query */

		goto finish;
	}

	ret = rlm_sql_fetch_row(&row, &handle, inst);
	if (ret) {
		REDEBUG("SQL query failed");
		(inst->module->sql_finish_select_query)(handle, inst->config);
		ret = -1;

		goto finish;
	}

	if (!row) {
		RDEBUG("SQL query returned no results");
		(inst->module->sql_finish_select_query)(handle, inst->config);
		ret = -1;

		goto finish;
	}

	if (!row[0]){
		RDEBUG("NULL value in first column of result");
		(inst->module->sql_finish_select_query)(handle, inst->config);
		ret = -1;

		goto finish;
	}

	len = strlen(row[0]);
	if (len >= freespace){
		RDEBUG("Insufficient string space");
		(inst->module->sql_finish_select_query)(handle, inst->config);

		ret = -1;
		goto finish;
	}

	strlcpy(out, row[0], freespace);
	ret = len;

	(inst->module->sql_finish_select_query)(handle, inst->config);

finish:
	fr_connection_release(inst->pool, handle);

	return ret;
}

static int generate_sql_clients(rlm_sql_t *inst)
{
	rlm_sql_handle_t *handle;
	rlm_sql_row_t row;
	unsigned int i = 0;
	RADCLIENT *c;

	DEBUG("rlm_sql (%s): Processing generate_sql_clients",
	      inst->config->xlat_name);

	DEBUG("rlm_sql (%s) in generate_sql_clients: query is %s",
	      inst->config->xlat_name, inst->config->client_query);

	handle = fr_connection_get(inst->pool);
	if (!handle) {
		return -1;
	}

	if (rlm_sql_select_query(&handle, inst, inst->config->client_query) != RLM_SQL_OK) return -1;

	while (rlm_sql_fetch_row(&row, &handle, inst) == 0) {
		char *server = NULL;
		i++;

		if (!row) break;

		/*
		 *  The return data for each row MUST be in the following order:
		 *
		 *  0. Row ID (currently unused)
		 *  1. Name (or IP address)
		 *  2. Shortname
		 *  3. Type
		 *  4. Secret
		 *  5. Virtual Server (optional)
		 */
		if (!row[0]){
			ERROR("rlm_sql (%s): No row id found on pass %d",inst->config->xlat_name,i);
			continue;
		}
		if (!row[1]){
			ERROR("rlm_sql (%s): No nasname found for row %s",inst->config->xlat_name,row[0]);
			continue;
		}
		if (!row[2]){
			ERROR("rlm_sql (%s): No short name found for row %s",inst->config->xlat_name,row[0]);
			continue;
		}
		if (!row[4]){
			ERROR("rlm_sql (%s): No secret found for row %s",inst->config->xlat_name,row[0]);
			continue;
		}

		if (((inst->module->sql_num_fields)(handle, inst->config) > 5) && (row[5] != NULL) && *row[5]) {
			server = row[5];
		}

		DEBUG("rlm_sql (%s): Adding client %s (%s) to %s clients list",
		      inst->config->xlat_name,
		      row[1], row[2], server ? server : "global");

		/* FIXME: We should really pass a proper ctx */
		c = client_afrom_query(NULL,
				      row[1],	/* identifier */
				      row[4],	/* secret */
				      row[2],	/* shortname */
				      row[3],	/* type */
				      server,	/* server */
				      false);	/* require message authenticator */
		if (!c) {
			continue;
		}

		if (!client_add(NULL, c)) {
			WARN("Failed to add client, possible duplicate?");

			client_free(c);
			continue;
		}

		DEBUG("rlm_sql (%s): Client \"%s\" (%s) added", c->longname, c->shortname,
		      inst->config->xlat_name);
	}

	(inst->module->sql_finish_select_query)(handle, inst->config);
	fr_connection_release(inst->pool, handle);

	return 0;
}


/*
 *	Translate the SQL queries.
 */
static size_t sql_escape_func(UNUSED REQUEST *request, char *out, size_t outlen,
			      char const *in, void *arg)
{
	rlm_sql_t *inst = arg;
	size_t len = 0;

	while (in[0]) {
		size_t utf8_len;

		/*
		 *	Allow all multi-byte UTF8 characters.
		 */
		utf8_len = fr_utf8_char((uint8_t const *) in);
		if (utf8_len > 1) {
			if (outlen <= utf8_len) break;

			memcpy(out, in, utf8_len);
			in += utf8_len;
			out += utf8_len;

			outlen -= utf8_len;
			len += utf8_len;
			continue;
		}

		/*
		 *	Because we register our own escape function
		 *	we're now responsible for escaping all special
		 *	chars in an xlat expansion or attribute value.
		 */
		switch (in[0]) {
		case '\n':
			if (outlen <= 2) break;
			out[0] = '\'';
			out[1] = 'n';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;

		case '\r':
			if (outlen <= 2) break;
			out[0] = '\'';
			out[1] = 'r';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;

		case '\t':
			if (outlen <= 2) break;
			out[0] = '\'';
			out[1] = 't';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;
		}

		/*
		 *	Non-printable characters get replaced with their
		 *	mime-encoded equivalents.
		 */
		if ((in[0] < 32) ||
		    strchr(inst->config->allowed_chars, *in) == NULL) {
			/*
			 *	Only 3 or less bytes available.
			 */
			if (outlen <= 3) {
				break;
			}

			snprintf(out, outlen, "=%02X", (unsigned char) in[0]);
			in++;
			out += 3;
			outlen -= 3;
			len += 3;
			continue;
		}

		/*
		 *	Only one byte left.
		 */
		if (outlen <= 1) {
			break;
		}

		/*
		 *	Allowed character.
		 */
		*out = *in;
		out++;
		in++;
		outlen--;
		len++;
	}
	*out = '\0';
	return len;
}

/*
 *	Set the SQL user name.
 *
 *	We don't call the escape function here. The resulting string
 *	will be escaped later in the queries xlat so we don't need to
 *	escape it twice. (it will make things wrong if we have an
 *	escape candidate character in the username)
 */
int sql_set_user(rlm_sql_t *inst, REQUEST *request, char const *username)
{
	char *expanded = NULL;
	VALUE_PAIR *vp = NULL;
	char const *sqluser;
	ssize_t len;

	rad_assert(request->packet != NULL);

	if (username != NULL) {
		sqluser = username;
	} else if (inst->config->query_user[0] != '\0') {
		sqluser = inst->config->query_user;
	} else {
		return 0;
	}

	len = radius_axlat(&expanded, request, sqluser, NULL, NULL);
	if (len < 0) {
		return -1;
	}

	vp = pairalloc(request->packet, inst->sql_user);
	if (!vp) {
		talloc_free(expanded);
		return -1;
	}

	pairstrsteal(vp, expanded);
	RDEBUG2("SQL-User-Name set to '%s'", vp->vp_strvalue);
	vp->op = T_OP_SET;
	radius_pairmove(request, &request->packet->vps, vp, false);	/* needs to be pair move else op is not respected */

	return 0;
}

/*
 *	Do a set/unset user, so it's a bit clearer what's going on.
 */
#define sql_unset_user(_i, _r) pairdelete(&_r->packet->vps, _i->sql_user->attr, _i->sql_user->vendor, TAG_ANY)

static int sql_get_grouplist(rlm_sql_t *inst, rlm_sql_handle_t **handle, REQUEST *request,
			     rlm_sql_grouplist_t **phead)
{
	char    *expanded = NULL;
	int     num_groups = 0;
	rlm_sql_row_t row;
	rlm_sql_grouplist_t *entry;
	int ret;

	/* NOTE: sql_set_user should have been run before calling this function */

	entry = *phead = NULL;

	if (!inst->config->groupmemb_query) return 0;\
	if (radius_axlat(&expanded, request, inst->config->groupmemb_query, sql_escape_func, inst) < 0) return -1;

	ret = rlm_sql_select_query(handle, inst, expanded);
	talloc_free(expanded);
	if (ret != RLM_SQL_OK) return -1;

	while (rlm_sql_fetch_row(&row, handle, inst) == 0) {
		if (!row) break;

		if (!row[0]){
			RDEBUG("row[0] returned NULL");
			(inst->module->sql_finish_select_query)(*handle, inst->config);
			talloc_free(entry);
			return -1;
		}

		if (!*phead) {
			*phead = talloc_zero(*handle, rlm_sql_grouplist_t);
			entry = *phead;
		} else {
			entry->next = talloc_zero(*phead, rlm_sql_grouplist_t);
			entry = entry->next;
		}
		entry->next = NULL;
		entry->name = talloc_typed_strdup(entry, row[0]);

		num_groups++;
	}

	(inst->module->sql_finish_select_query)(*handle, inst->config);

	return num_groups;
}


/*
 * sql groupcmp function. That way we can do group comparisons (in the users file for example)
 * with the group memberships reciding in sql
 * The group membership query should only return one element which is the username. The returned
 * username will then be checked with the passed check string.
 */

static int CC_HINT(nonnull (1, 2, 4)) sql_groupcmp(void *instance, REQUEST *request, UNUSED VALUE_PAIR *request_vp,
						   VALUE_PAIR *check, UNUSED VALUE_PAIR *check_pairs,
						   UNUSED VALUE_PAIR **reply_pairs)
{
	rlm_sql_handle_t *handle;
	rlm_sql_t *inst = instance;
	rlm_sql_grouplist_t *head, *entry;

	RDEBUG("sql_groupcmp");

	if (check->length == 0){
		RDEBUG("sql_groupcmp: Illegal group name");
		return 1;
	}

	/*
	 *	Set, escape, and check the user attr here
	 */
	if (sql_set_user(inst, request, NULL) < 0)
		return 1;

	/*
	 *	Get a socket for this lookup
	 */
	handle = fr_connection_get(inst->pool);
	if (!handle) {
		return 1;
	}

	/*
	 *	Get the list of groups this user is a member of
	 */
	if (sql_get_grouplist(inst, &handle, request, &head) < 0) {
		REDEBUG("Error getting group membership");
		fr_connection_release(inst->pool, handle);
		return 1;
	}

	for (entry = head; entry != NULL; entry = entry->next) {
		if (strcmp(entry->name, check->vp_strvalue) == 0){
			RDEBUG("sql_groupcmp finished: User is a member of group %s",
			       check->vp_strvalue);
			talloc_free(head);
			fr_connection_release(inst->pool, handle);
			return 0;
		}
	}

	/* Free the grouplist */
	talloc_free(head);
	fr_connection_release(inst->pool, handle);

	RDEBUG("sql_groupcmp finished: User is NOT a member of group %s", check->vp_strvalue);

	return 1;
}

static rlm_rcode_t rlm_sql_process_groups(rlm_sql_t *inst, REQUEST *request, rlm_sql_handle_t **handle,
					  sql_fall_through_t *do_fall_through)
{
	rlm_rcode_t		rcode = RLM_MODULE_NOOP;
	VALUE_PAIR		*check_tmp = NULL, *reply_tmp = NULL, *sql_group = NULL;
	rlm_sql_grouplist_t	*head = NULL, *entry = NULL;

	char			*expanded = NULL;
	int			rows;

	rad_assert(request->packet != NULL);

	/*
	 *	Get the list of groups this user is a member of
	 */
	rows = sql_get_grouplist(inst, handle, request, &head);
	if (rows < 0) {
		REDEBUG("Error retrieving group list");

		return RLM_MODULE_FAIL;
	}
	if (rows == 0) {
		RDEBUG2("User not found in any groups");
		rcode = RLM_MODULE_NOTFOUND;
		goto finish;
	}
	rad_assert(head);

	RDEBUG2("User found in the group table");

	entry = head;
	do {
		/*
		 *	Add the Sql-Group attribute to the request list so we know
		 *	which group we're retrieving attributes for
		 */
		sql_group = pairmake_packet("Sql-Group", entry->name, T_OP_EQ);
		if (!sql_group) {
			REDEBUG("Error creating Sql-Group attribute");
			rcode = RLM_MODULE_FAIL;
			goto finish;
		}

		if (inst->config->authorize_group_check_query) {
			vp_cursor_t cursor;
			VALUE_PAIR *vp;

			/*
			 *	Expand the group query
			 */
			if (radius_axlat(&expanded, request, inst->config->authorize_group_check_query,
					 sql_escape_func, inst) < 0) {
				REDEBUG("Error generating query");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			rows = sql_getvpdata(request, inst, request, handle, &check_tmp, expanded);
			TALLOC_FREE(expanded);
			if (rows < 0) {
				REDEBUG("Error retrieving check pairs for group %s", entry->name);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			/*
			 *	If we got check rows we need to process them before we decide to process the reply rows
			 */
			if ((rows > 0) &&
			    (paircompare(request, request->packet->vps, check_tmp, &request->reply->vps) != 0)) {
				pairfree(&check_tmp);
				pairdelete(&request->packet->vps, PW_SQL_GROUP, 0, TAG_ANY);

				continue;
			}

			RDEBUG2("Group \"%s\": Conditional check items matched", entry->name);
			rcode = RLM_MODULE_OK;

			RDEBUG2("Group \"%s\": Merging assignment check items", entry->name);
			RINDENT();
			for (vp = fr_cursor_init(&cursor, &check_tmp);
			     vp;
			     vp = fr_cursor_next(&cursor)) {
			 	if (!fr_assignment_op[vp->op]) continue;

			 	rdebug_pair(2, request, vp);
			}
			REXDENT();
			radius_pairmove(request, &request->config_items, check_tmp, true);
			check_tmp = NULL;
		}

		if (inst->config->authorize_group_reply_query) {
			/*
			 *	Now get the reply pairs since the paircompare matched
			 */
			if (radius_axlat(&expanded, request, inst->config->authorize_group_reply_query,
					 sql_escape_func, inst) < 0) {
				REDEBUG("Error generating query");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			rows = sql_getvpdata(request->reply, inst, request, handle, &reply_tmp, expanded);
			TALLOC_FREE(expanded);
			if (rows < 0) {
				REDEBUG("Error retrieving reply pairs for group %s", entry->name);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}
			*do_fall_through = fall_through(reply_tmp);

			RDEBUG2("Group \"%s\": Merging reply items", entry->name);
			rcode = RLM_MODULE_OK;

			rdebug_pair_list(L_DBG_LVL_2, request, reply_tmp, NULL);

			radius_pairmove(request, &request->reply->vps, reply_tmp, true);
			reply_tmp = NULL;
		}

		pairdelete(&request->packet->vps, PW_SQL_GROUP, 0, TAG_ANY);
		entry = entry->next;
	} while (entry != NULL && (*do_fall_through == FALL_THROUGH_YES));

finish:
	talloc_free(head);
	pairdelete(&request->packet->vps, PW_SQL_GROUP, 0, TAG_ANY);

	return rcode;
}


static int mod_detach(void *instance)
{
	rlm_sql_t *inst = instance;

	if (inst->pool) fr_connection_pool_delete(inst->pool);

	/*
	 *  We need to explicitly free all children, so if the driver
	 *  parented any memory off the instance, their destructors
	 *  run before we unload the bytecode for them.
	 *
	 *  If we don't do this, we get a SEGV deep inside the talloc code
	 *  when it tries to call a destructor that no longer exists.
	 */
	talloc_free_children(inst);

	/*
	 *  Decrements the reference count. The driver object won't be unloaded
	 *  until all instances of rlm_sql that use it have been destroyed.
	 */
	if (inst->handle) dlclose(inst->handle);

	return 0;
}

static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_sql_t *inst = instance;

	/*
	 *	Hack...
	 */
	inst->config = &inst->myconfig;
	inst->cs = conf;

	inst->config->xlat_name = cf_section_name2(conf);
	if (!inst->config->xlat_name) {
		inst->config->xlat_name = cf_section_name1(conf);
	} else {
		char *group_name;
		DICT_ATTR const *da;
		ATTR_FLAGS flags;

		/*
		 *	Allocate room for <instance>-SQL-Group
		 */
		group_name = talloc_typed_asprintf(inst, "%s-SQL-Group", inst->config->xlat_name);
		DEBUG("rlm_sql (%s): Creating new attribute %s",
		      inst->config->xlat_name, group_name);

		memset(&flags, 0, sizeof(flags));
		if (dict_addattr(group_name, -1, 0, PW_TYPE_STRING, flags) < 0) {
			ERROR("rlm_sql (%s): Failed to create "
			       "attribute %s: %s", inst->config->xlat_name, group_name,
			       fr_strerror());
			return -1;
		}

		da = dict_attrbyname(group_name);
		if (!da) {
			ERROR("rlm_sql (%s): Failed to create "
			       "attribute %s", inst->config->xlat_name, group_name);
			return -1;
		}

		if (inst->config->groupmemb_query) {
			DEBUG("rlm_sql (%s): Registering sql_groupcmp for %s",
			      inst->config->xlat_name, group_name);
			paircompare_register(da, dict_attrbyvalue(PW_USER_NAME, 0),
					     false, sql_groupcmp, inst);
		}
	}

	rad_assert(inst->config->xlat_name);

	/*
	 *	This will always exist, as cf_section_parse_init()
	 *	will create it if it doesn't exist.  However, the
	 *	"reference" config item won't exist in an auto-created
	 *	configuration.  So if that doesn't exist, we ignore
	 *	the whole subsection.
	 */
	inst->config->accounting.cs = cf_section_sub_find(conf, "accounting");
	inst->config->accounting.reference_cp = (cf_pair_find(inst->config->accounting.cs, "reference") != NULL);

	inst->config->postauth.cs = cf_section_sub_find(conf, "post-auth");
	inst->config->postauth.reference_cp = (cf_pair_find(inst->config->postauth.cs, "reference") != NULL);

	/*
	 *	Cache the SQL-User-Name DICT_ATTR, so we can be slightly
	 *	more efficient about creating SQL-User-Name attributes.
	 */
	inst->sql_user = dict_attrbyname("SQL-User-Name");
	if (!inst->sql_user) {
		return -1;
	}

	/*
	 *	Export these methods, too.  This avoids RTDL_GLOBAL.
	 */
	inst->sql_set_user		= sql_set_user;
	inst->sql_escape_func		= sql_escape_func;
	inst->sql_query			= rlm_sql_query;
	inst->sql_select_query		= rlm_sql_select_query;
	inst->sql_fetch_row		= rlm_sql_fetch_row;

	/*
	 *	Register the SQL xlat function
	 */
	xlat_register(inst->config->xlat_name, sql_xlat, sql_escape_func, inst);

	/*
	 *	Sanity check for crazy people.
	 */
	if (strncmp(inst->config->sql_driver_name, "rlm_sql_", 8) != 0) {
		ERROR("rlm_sql (%s): \"%s\" is NOT an SQL driver!", inst->config->xlat_name, inst->config->sql_driver_name);
		return -1;
	}

	/*
	 *	Load the appropriate driver for our database
	 */
	inst->handle = lt_dlopenext(inst->config->sql_driver_name);
	if (!inst->handle) {
		ERROR("Could not link driver %s: %s", inst->config->sql_driver_name, dlerror());
		ERROR("Make sure it (and all its dependent libraries!) are in the search path of your system's ld");
		return -1;
	}

	inst->module = (rlm_sql_module_t *) dlsym(inst->handle,
						  inst->config->sql_driver_name);
	if (!inst->module) {
		ERROR("Could not link symbol %s: %s", inst->config->sql_driver_name, dlerror());
		return -1;
	}

	if (inst->module->mod_instantiate) {
		CONF_SECTION *cs;
		char const *name;

		name = strrchr(inst->config->sql_driver_name, '_');
		if (!name) {
			name = inst->config->sql_driver_name;
		} else {
			name++;
		}

		cs = cf_section_sub_find(conf, name);
		if (!cs) {
			cs = cf_section_alloc(conf, name, NULL);
			if (!cs) {
				return -1;
			}
		}

		/*
		 *	It's up to the driver to register a destructor
		 */
		if (inst->module->mod_instantiate(cs, inst->config) < 0) {
			return -1;
		}
	}

	inst->lf = fr_logfile_init(inst);
	if (!inst->lf) {
		cf_log_err_cs(conf, "Failed creating log file context");
		return -1;
	}

	INFO("rlm_sql (%s): Driver %s (module %s) loaded and linked", inst->config->xlat_name,
	     inst->config->sql_driver_name, inst->module->name);

	/*
	 *	Initialise the connection pool for this instance
	 */
	INFO("rlm_sql (%s): Attempting to connect to database \"%s\"", inst->config->xlat_name, inst->config->sql_db);

	inst->pool = fr_connection_pool_module_init(inst->cs, inst, mod_conn_create, NULL, NULL);
	if (!inst->pool) return -1;

	if (inst->config->groupmemb_query) {
		paircompare_register(dict_attrbyvalue(PW_SQL_GROUP, 0),
				     dict_attrbyvalue(PW_USER_NAME, 0), false, sql_groupcmp, inst);
	}

	if (inst->config->do_clients) {
		if (generate_sql_clients(inst) == -1){
			ERROR("Failed to load clients from SQL");
			return -1;
		}
	}

	return RLM_MODULE_OK;
}


static rlm_rcode_t CC_HINT(nonnull) mod_authorize(void *instance, REQUEST *request)
{
	rlm_rcode_t rcode = RLM_MODULE_NOOP;

	rlm_sql_t *inst = instance;
	rlm_sql_handle_t  *handle;

	VALUE_PAIR *check_tmp = NULL;
	VALUE_PAIR *reply_tmp = NULL;
	VALUE_PAIR *user_profile = NULL;

	bool	user_found = false;

	sql_fall_through_t do_fall_through = FALL_THROUGH_DEFAULT;

	int	rows;

	char	*expanded = NULL;

	rad_assert(request->packet != NULL);
	rad_assert(request->reply != NULL);

	if (!inst->config->authorize_check_query && !inst->config->authorize_reply_query &&
	    !inst->config->read_groups && !inst->config->read_profiles) {
		RWDEBUG("No authorization checks configured, returning noop");

		return RLM_MODULE_NOOP;
	}

	/*
	 *	Set, escape, and check the user attr here
	 */
	if (sql_set_user(inst, request, NULL) < 0) {
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Reserve a socket
	 *
	 *	After this point use goto error or goto release to cleanup socket temporary pairlists and
	 *	temporary attributes.
	 */
	handle = fr_connection_get(inst->pool);
	if (!handle) {
		rcode = RLM_MODULE_FAIL;
		goto error;
	}

	/*
	 *	Query the check table to find any conditions associated with this user/realm/whatever...
	 */
	if (inst->config->authorize_check_query) {
		vp_cursor_t cursor;
		VALUE_PAIR *vp;

		if (radius_axlat(&expanded, request, inst->config->authorize_check_query,
				 sql_escape_func, inst) < 0) {
			REDEBUG("Error generating query");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		rows = sql_getvpdata(request, inst, request, &handle, &check_tmp, expanded);
		TALLOC_FREE(expanded);
		if (rows < 0) {
			REDEBUG("SQL query error getting check attributes");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		if (rows == 0) goto skipreply;	/* Don't need to free VPs we don't have */

		/*
		 *	Only do this if *some* check pairs were returned
		 */
		RDEBUG2("User found in radcheck table");
		user_found = true;
		if (paircompare(request, request->packet->vps, check_tmp, &request->reply->vps) != 0) {
			pairfree(&check_tmp);
			check_tmp = NULL;
			goto skipreply;
		}

		RDEBUG2("Conditional check items matched, merging assignment check items");
		RINDENT();
		for (vp = fr_cursor_init(&cursor, &check_tmp);
		     vp;
		     vp = fr_cursor_next(&cursor)) {
			if (!fr_assignment_op[vp->op]) continue;

			rdebug_pair(2, request, vp);
		}
		REXDENT();
		radius_pairmove(request, &request->config_items, check_tmp, true);

		rcode = RLM_MODULE_OK;
		check_tmp = NULL;
	}

	if (inst->config->authorize_reply_query) {
		/*
		 *	Now get the reply pairs since the paircompare matched
		 */
		if (radius_axlat(&expanded, request, inst->config->authorize_reply_query,
				 sql_escape_func, inst) < 0) {
			REDEBUG("Error generating query");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		rows = sql_getvpdata(request->reply, inst, request, &handle, &reply_tmp, expanded);
		TALLOC_FREE(expanded);
		if (rows < 0) {
			REDEBUG("SQL query error getting reply attributes");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		if (rows == 0) goto skipreply;

		do_fall_through = fall_through(reply_tmp);

		RDEBUG2("User found in radreply table, merging reply items");
		user_found = true;

		rdebug_pair_list(L_DBG_LVL_2, request, reply_tmp, NULL);

		radius_pairmove(request, &request->reply->vps, reply_tmp, true);

		rcode = RLM_MODULE_OK;
		reply_tmp = NULL;
	}

skipreply:
	if ((do_fall_through == FALL_THROUGH_YES) ||
	    (inst->config->read_groups && (do_fall_through == FALL_THROUGH_DEFAULT))) {
		rlm_rcode_t ret;

		RDEBUG3("... falling-through to group processing");
		ret = rlm_sql_process_groups(inst, request, &handle, &do_fall_through);
		switch (ret) {
		/*
		 *	Nothing bad happened, continue...
		 */
		case RLM_MODULE_UPDATED:
			rcode = RLM_MODULE_UPDATED;
			/* FALL-THROUGH */
		case RLM_MODULE_OK:
			if (rcode != RLM_MODULE_UPDATED) {
				rcode = RLM_MODULE_OK;
			}
			/* FALL-THROUGH */
		case RLM_MODULE_NOOP:
			user_found = true;
			break;

		case RLM_MODULE_NOTFOUND:
			break;

		default:
			rcode = ret;
			goto release;
		}
	}

	/*
	 *	Repeat the above process with the default profile or User-Profile
	 */
	if ((do_fall_through == FALL_THROUGH_YES) ||
	    (inst->config->read_profiles && (do_fall_through == FALL_THROUGH_DEFAULT))) {
		rlm_rcode_t ret;

		/*
		 *  Check for a default_profile or for a User-Profile.
		 */
		RDEBUG3("... falling-through to profile processing");
		user_profile = pairfind(request->config_items, PW_USER_PROFILE, 0, TAG_ANY);

		char const *profile = user_profile ?
				      user_profile->vp_strvalue :
				      inst->config->default_profile;

		if (!profile || !*profile) {
			goto release;
		}

		RDEBUG2("Checking profile %s", profile);

		if (sql_set_user(inst, request, profile) < 0) {
			REDEBUG("Error setting profile");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		ret = rlm_sql_process_groups(inst, request, &handle, &do_fall_through);
		switch (ret) {
		/*
		 *	Nothing bad happened, continue...
		 */
		case RLM_MODULE_UPDATED:
			rcode = RLM_MODULE_UPDATED;
			/* FALL-THROUGH */
		case RLM_MODULE_OK:
			if (rcode != RLM_MODULE_UPDATED) {
				rcode = RLM_MODULE_OK;
			}
			/* FALL-THROUGH */
		case RLM_MODULE_NOOP:
			user_found = true;
			break;

		case RLM_MODULE_NOTFOUND:
			break;

		default:
			rcode = ret;
			goto release;
		}
	}

	/*
	 *	At this point the key (user) hasn't be found in the check table, the reply table
	 *	or the group mapping table, and there was no matching profile.
	 */
release:
	if (!user_found) {
		rcode = RLM_MODULE_NOTFOUND;
	}

	fr_connection_release(inst->pool, handle);
	sql_unset_user(inst, request);

	return rcode;

error:
	pairfree(&check_tmp);
	pairfree(&reply_tmp);
	sql_unset_user(inst, request);

	fr_connection_release(inst->pool, handle);

	return rcode;
}

/*
 *	Generic function for failing between a bunch of queries.
 *
 *	Uses the same principle as rlm_linelog, expanding the 'reference' config
 *	item using xlat to figure out what query it should execute.
 *
 *	If the reference matches multiple config items, and a query fails or
 *	doesn't update any rows, the next matching config item is used.
 *
 */
static int acct_redundant(rlm_sql_t *inst, REQUEST *request, sql_acct_section_t *section)
{
	rlm_rcode_t		rcode = RLM_MODULE_OK;

	rlm_sql_handle_t	*handle = NULL;
	int			sql_ret;
	int			numaffected = 0;

	CONF_ITEM		*item;
	CONF_PAIR 		*pair;
	char const		*attr = NULL;
	char const		*value;

	char			path[MAX_STRING_LEN];
	char			*p = path;
	char			*expanded = NULL;

	rad_assert(section);

	if (section->reference[0] != '.') {
		*p++ = '.';
	}

	if (radius_xlat(p, sizeof(path) - (p - path), request, section->reference, NULL, NULL) < 0) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	item = cf_reference_item(NULL, section->cs, path);
	if (!item) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	if (cf_item_is_section(item)){
		REDEBUG("Sections are not supported as references");
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	pair = cf_itemtopair(item);
	attr = cf_pair_attr(pair);

	RDEBUG2("Using query template '%s'", attr);

	handle = fr_connection_get(inst->pool);
	if (!handle) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	sql_set_user(inst, request, NULL);

	while (true) {
		value = cf_pair_value(pair);
		if (!value) {
			RDEBUG("Ignoring null query");
			rcode = RLM_MODULE_NOOP;

			goto finish;
		}

		if (radius_axlat(&expanded, request, value, sql_escape_func, inst) < 0) {
			rcode = RLM_MODULE_FAIL;

			goto finish;
		}

		if (!*expanded) {
			RDEBUG("Ignoring null query");
			rcode = RLM_MODULE_NOOP;
			talloc_free(expanded);

			goto finish;
		}

		rlm_sql_query_log(inst, request, section, expanded);

		/*
		 *  If rlm_sql_query cannot use the socket it'll try and
		 *  reconnect. Reconnecting will automatically release
		 *  the current socket, and try to select a new one.
		 *
		 *  If we get RLM_SQL_RECONNECT it means all connections in the pool
		 *  were exhausted, and we couldn't create a new connection,
		 *  so we do not need to call fr_connection_release.
		 */
		sql_ret = rlm_sql_query(&handle, inst, expanded);
		TALLOC_FREE(expanded);
		if (sql_ret == RLM_SQL_RECONNECT) {
			rcode = RLM_MODULE_FAIL;
			goto finish;
		}
		rad_assert(handle);

		/*
		 *  Assume all other errors are incidental, and just meant our
		 *  operation failed and its not a client or SQL syntax error.
		 *
		 *  @fixme We should actually be able to distinguish between key
		 *  constraint violations (which we expect) and other errors.
		 */
		if (sql_ret == RLM_SQL_OK) {
			numaffected = (inst->module->sql_affected_rows)(handle, inst->config);
			if (numaffected > 0) {
				break;	/* A query succeeded, were done! */
			}

			RDEBUG("No records updated");
		}

		(inst->module->sql_finish_query)(handle, inst->config);

		/*
		 *  We assume all entries with the same name form a redundant
		 *  set of queries.
		 */
		pair = cf_pair_find_next(section->cs, pair, attr);

		if (!pair) {
			RDEBUG("No additional queries configured");
			rcode = RLM_MODULE_NOOP;

			goto finish;
		}

		RDEBUG("Trying next query...");
	}

	(inst->module->sql_finish_query)(handle, inst->config);

finish:
	talloc_free(expanded);
	fr_connection_release(inst->pool, handle);
	sql_unset_user(inst, request);

	return rcode;
}

#ifdef WITH_ACCOUNTING

/*
 *	Accounting: Insert or update session data in our sql table
 */
static rlm_rcode_t CC_HINT(nonnull) mod_accounting(void *instance, REQUEST * request) {
	rlm_sql_t *inst = instance;

	if (inst->config->accounting.reference_cp) {
		return acct_redundant(inst, request, &inst->config->accounting);
	}

	return RLM_MODULE_NOOP;
}

#endif

#ifdef WITH_SESSION_MGMT
/*
 *	See if a user is already logged in. Sets request->simul_count to the
 *	current session count for this user.
 *
 *	Check twice. If on the first pass the user exceeds his
 *	max. number of logins, do a second pass and validate all
 *	logins by querying the terminal server (using eg. SNMP).
 */

static rlm_rcode_t CC_HINT(nonnull) mod_checksimul(void *instance, REQUEST * request) {
	rlm_rcode_t		rcode = RLM_MODULE_OK;
	rlm_sql_handle_t 	*handle = NULL;
	rlm_sql_t		*inst = instance;
	rlm_sql_row_t		row;
	int			check = 0;
	uint32_t		ipno = 0;
	char const     		*call_num = NULL;
	VALUE_PAIR		*vp;
	int			ret;
	uint32_t		nas_addr = 0;
	uint32_t		nas_port = 0;

	char 			*expanded = NULL;

	/* If simul_count_query is not defined, we don't do any checking */
	if (!inst->config->simul_count_query) return RLM_MODULE_NOOP;

	if ((!request->username) || (request->username->length == '\0')) {
		REDEBUG("Zero Length username not permitted");

		return RLM_MODULE_INVALID;
	}


	if(sql_set_user(inst, request, NULL) < 0) {
		return RLM_MODULE_FAIL;
	}

	if (radius_axlat(&expanded, request, inst->config->simul_count_query, sql_escape_func, inst) < 0) {
		sql_unset_user(inst, request);
		return RLM_MODULE_FAIL;
	}

	/* initialize the sql socket */
	handle = fr_connection_get(inst->pool);
	if (!handle) {
		talloc_free(expanded);
		sql_unset_user(inst, request);
		return RLM_MODULE_FAIL;
	}

	if (rlm_sql_select_query(&handle, inst, expanded) != RLM_SQL_OK) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	ret = rlm_sql_fetch_row(&row, &handle, inst);
	if (ret != 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}
	if (!row) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	request->simul_count = atoi(row[0]);

	(inst->module->sql_finish_select_query)(handle, inst->config);
	TALLOC_FREE(expanded);

	if (request->simul_count < request->simul_max) {
		rcode = RLM_MODULE_OK;
		goto finish;
	}

	/*
	 *	Looks like too many sessions, so let's start verifying
	 *	them, unless told to rely on count query only.
	 */
	if (!inst->config->simul_verify_query) {
		rcode = RLM_MODULE_OK;

		goto finish;
	}

	if (radius_axlat(&expanded, request, inst->config->simul_verify_query, sql_escape_func, inst) < 0) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	if (rlm_sql_select_query(&handle, inst, expanded) != RLM_SQL_OK) goto finish;

	/*
	 *      Setup some stuff, like for MPP detection.
	 */
	request->simul_count = 0;

	if ((vp = pairfind(request->packet->vps, PW_FRAMED_IP_ADDRESS, 0, TAG_ANY)) != NULL) {
		ipno = vp->vp_ipaddr;
	}

	if ((vp = pairfind(request->packet->vps, PW_CALLING_STATION_ID, 0, TAG_ANY)) != NULL) {
		call_num = vp->vp_strvalue;
	}

	while (rlm_sql_fetch_row(&row, &handle, inst) == 0) {
		if (!row) break;

		if (!row[2]){
			RDEBUG("Cannot zap stale entry. No username present in entry");
			rcode = RLM_MODULE_FAIL;

			goto finish;
		}

		if (!row[1]){
			RDEBUG("Cannot zap stale entry. No session id in entry");
			rcode = RLM_MODULE_FAIL;

			goto finish;
		}

		if (row[3]) {
			nas_addr = inet_addr(row[3]);
		}

		if (row[4]) {
			nas_port = atoi(row[4]);
		}

		check = rad_check_ts(nas_addr, nas_port, row[2], row[1]);
		if (check == 0) {
			/*
			 *	Stale record - zap it.
			 */
			if (inst->config->deletestalesessions == true) {
				uint32_t framed_addr = 0;
				char proto = 0;
				int sess_time = 0;

				if (row[5])
					framed_addr = inet_addr(row[5]);
				if (row[7]){
					if (strcmp(row[7], "PPP") == 0)
						proto = 'P';
					else if (strcmp(row[7], "SLIP") == 0)
						proto = 'S';
				}
				if (row[8])
					sess_time = atoi(row[8]);
				session_zap(request, nas_addr, nas_port,
					    row[2], row[1], framed_addr,
					    proto, sess_time);
			}
		}
		else if (check == 1) {
			/*
			 *	User is still logged in.
			 */
			++request->simul_count;

			/*
			 *      Does it look like a MPP attempt?
			 */
			if (row[5] && ipno && inet_addr(row[5]) == ipno) {
				request->simul_mpp = 2;
			} else if (row[6] && call_num && !strncmp(row[6],call_num,16)) {
				request->simul_mpp = 2;
			}
		} else {
			/*
			 *      Failed to check the terminal server for
			 *      duplicate logins: return an error.
			 */
			REDEBUG("Failed to check the terminal server for user '%s'.", row[2]);

			rcode = RLM_MODULE_FAIL;
			goto finish;
		}
	}

	finish:

	(inst->module->sql_finish_select_query)(handle, inst->config);
	fr_connection_release(inst->pool, handle);
	talloc_free(expanded);
	sql_unset_user(inst, request);

	/*
	 *	The Auth module apparently looks at request->simul_count,
	 *	not the return value of this module when deciding to deny
	 *	a call for too many sessions.
	 */
	return rcode;
}
#endif

/*
 *	Postauth: Write a record of the authentication attempt
 */
static rlm_rcode_t CC_HINT(nonnull) mod_post_auth(void *instance, REQUEST * request) {
	rlm_sql_t *inst = instance;

	if (inst->config->postauth.reference_cp) {
		return acct_redundant(inst, request, &inst->config->postauth);
	}

	return RLM_MODULE_NOOP;
}

/*
 *	Execute postauth_query after authentication
 */


/* globally exported name */
module_t rlm_sql = {
	RLM_MODULE_INIT,
	"SQL",
	RLM_TYPE_THREAD_SAFE,	/* type: reserved */
	sizeof(rlm_sql_t),
	module_config,
	mod_instantiate,	/* instantiation */
	mod_detach,		/* detach */
	{
		NULL,			/* authentication */
		mod_authorize,	/* authorization */
		NULL,			/* preaccounting */
#ifdef WITH_ACCOUNTING
		mod_accounting,	/* accounting */
#else
		NULL,
#endif
#ifdef WITH_SESSION_MGMT
		mod_checksimul,	/* checksimul */
#else
		NULL,
#endif
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		mod_post_auth	/* post-auth */
	},
};
