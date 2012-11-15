/*	$OpenBSD: map_static.c,v 1.9 2012/11/12 14:58:53 eric Exp $	*/

/*
 * Copyright (c) 2012 Gilles Chehade <gilles@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#include <sys/types.h>
#include "sys-queue.h"
#include "sys-tree.h"
#include <sys/param.h>
#include <sys/socket.h>

#include <ctype.h>
#include <err.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smtpd.h"
#include "log.h"


/* static backend */
static int table_static_config(struct table *, const char *);
static int table_static_update(struct table *, const char *);
static void *table_static_open(struct table *);
static int table_static_lookup(void *, const char *, enum table_service, void **);
static int   table_static_compare(void *, const char *, enum table_service,
    int(*)(const char *, const char *));
static void  table_static_close(void *);

static int	table_static_credentials(const char *, char *, size_t, void **);
static int	table_static_alias(const char *, char *, size_t, void **);
static int	table_static_virtual(const char *, char *, size_t, void **);
static int	table_static_netaddr(const char *, char *, size_t, void **);

struct table_backend table_backend_static = {
	K_ALIAS|K_VIRTUAL|K_CREDENTIALS|K_NETADDR,
	table_static_config,
	table_static_open,
	table_static_update,
	table_static_close,
	table_static_lookup,
	table_static_compare
};

static int
table_static_config(struct table *table, const char *config)
{
	/* no config ? ok */
	if (config == NULL)
		return 1;

	return table_config_parser(table, config);
}

static int
table_static_update(struct table *table, const char *config)
{
	struct table   *t;
	char		name[MAX_LINE_SIZE];

	/* no config ? ok */
	if (config == NULL)
		goto ok;

	t = table_create(table->t_src, NULL, config);
	if (! t->t_backend->config(t, config))
		goto err;

	/* update successful, swap table names */
	strlcpy(name, table->t_name, sizeof name);
	strlcpy(table->t_name, t->t_name, sizeof table->t_name);
	strlcpy(t->t_name, name, sizeof t->t_name);

	/* swap, table id */
	table->t_id = table->t_id ^ t->t_id;
	t->t_id     = table->t_id ^ t->t_id;
	table->t_id = table->t_id ^ t->t_id;

	/* destroy former table */
	table_destroy(table);

ok:
	log_info("info: Table \"%s\" successfully updated", name);
	return 1;

err:
	table_destroy(t);
	log_info("info: Failed to update table \"%s\"", name);
	return 0;
}

static void *
table_static_open(struct table *table)
{
	return table;
}

static void
table_static_close(void *hdl)
{
	return;
}

static int
table_static_lookup(void *hdl, const char *key, enum table_service kind, void **retp)
{
	struct table	*m  = hdl;
	struct mapel	*me = NULL;
	char		*line;
	size_t		 len;
	int		ret;

	line = NULL;
	TAILQ_FOREACH(me, &m->t_contents, me_entry)
	    if (strcmp(key, me->me_key) == 0) {
		    line = me->me_val;
		    break;
	    }

	if (retp == NULL)
		return me ? 1 : 0;

	if (me == NULL) {
		*retp = NULL;
		return 0;
	}

	if ((line = strdup(line)) == NULL)
		return -1;

	len = strlen(line);
	switch (kind) {
	case K_ALIAS:
		ret = table_static_alias(key, line, len, retp);
		break;

	case K_CREDENTIALS:
		ret = table_static_credentials(key, line, len, retp);
		break;

	case K_VIRTUAL:
		ret = table_static_virtual(key, line, len, retp);
		break;

	case K_NETADDR:
		ret = table_static_netaddr(key, line, len, retp);
		break;

	default:
		ret = -1;
	}

	free(line);

	return ret;
}

static int
table_static_compare(void *hdl, const char *key, enum table_service kind,
    int(*func)(const char *, const char *))
{
	struct table	*m   = hdl;
	struct mapel	*me  = NULL;
	int		 ret = 0;

	TAILQ_FOREACH(me, &m->t_contents, me_entry) {
		if (! func(key, me->me_key))
			continue;
		ret = 1;
		break;
	}

	return ret;
}

static int
table_static_credentials(const char *key, char *line, size_t len, void **retp)
{
	struct table_credentials *credentials = NULL;
	char *p;

	/* credentials are stored as user:password */
	if (len < 3)
		return -1;

	/* too big to fit in a smtp session line */
	if (len >= MAX_LINE_SIZE)
		return -1;

	p = strchr(line, ':');
	if (p == NULL)
		return -1;

	if (p == line || p == line + len - 1)
		return -1;
	*p++ = '\0';

	credentials = xcalloc(1, sizeof *credentials,
	    "table_static_credentials");
	if (strlcpy(credentials->username, line, sizeof(credentials->username))
	    >= sizeof(credentials->username))
		goto err;

	if (strlcpy(credentials->password, p, sizeof(credentials->password))
	    >= sizeof(credentials->password))
		goto err;

	*retp = credentials;
	return 1;

err:
	*retp = NULL;
	free(credentials);
	return -1;
}

static int
table_static_alias(const char *key, char *line, size_t len, void **retp)
{
	char			*subrcpt;
	char			*endp;
	struct table_alias	*table_alias = NULL;
	struct expandnode	 xn;

	table_alias = xcalloc(1, sizeof *table_alias, "table_static_alias");

	while ((subrcpt = strsep(&line, ",")) != NULL) {
		/* subrcpt: strip initial whitespace. */
		while (isspace((int)*subrcpt))
			++subrcpt;
		if (*subrcpt == '\0')
			goto error;

		/* subrcpt: strip trailing whitespace. */
		endp = subrcpt + strlen(subrcpt) - 1;
		while (subrcpt < endp && isspace((int)*endp))
			*endp-- = '\0';

		if (! alias_parse(&xn, subrcpt))
			goto error;

		expand_insert(&table_alias->expand, &xn);
		table_alias->nbnodes++;
	}
	*retp = table_alias;
	return 1;

error:
	*retp = NULL;
	expand_free(&table_alias->expand);
	free(table_alias);
	return -1;
}

static int
table_static_virtual(const char *key, char *line, size_t len, void **retp)
{
	char			*subrcpt;
	char			*endp;
	struct table_virtual	*table_virtual = NULL;
	struct expandnode	 xn;

	/* domain key, discard value */
	if (strchr(key, '@') == NULL) {
		*retp = NULL;
		return 1;
	}

	table_virtual = xcalloc(1, sizeof *table_virtual,
	    "table_static_virtual");
	while ((subrcpt = strsep(&line, ",")) != NULL) {
		/* subrcpt: strip initial whitespace. */
		while (isspace((int)*subrcpt))
			++subrcpt;
		if (*subrcpt == '\0')
			goto error;

		/* subrcpt: strip trailing whitespace. */
		endp = subrcpt + strlen(subrcpt) - 1;
		while (subrcpt < endp && isspace((int)*endp))
			*endp-- = '\0';

		if (! alias_parse(&xn, subrcpt))
			goto error;

		expand_insert(&table_virtual->expand, &xn);
		table_virtual->nbnodes++;
	}

	*retp = table_virtual;
	return 1;

error:
	*retp = NULL;
	expand_free(&table_virtual->expand);
	free(table_virtual);
	return 0;
}


static int
table_static_netaddr(const char *key, char *line, size_t len, void **retp)
{
	struct table_netaddr	*table_netaddr = NULL;

	table_netaddr = xcalloc(1, sizeof *table_netaddr,
	    "table_static_netaddr");

	if (! text_to_netaddr(&table_netaddr->netaddr, line))
		goto error;

	*retp = table_netaddr;
	return 1;

error:
	*retp = NULL;
	free(table_netaddr);
	return 0;
}
