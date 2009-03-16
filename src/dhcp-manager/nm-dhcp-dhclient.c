/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-dhcp-dhclient.c - dhclient specific hooks for NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2008 Red Hat, Inc.
 */

#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <config.h>

#include "nm-dhcp-manager.h"
#include "nm-utils.h"


#define NM_DHCP_MANAGER_PID_FILENAME	"dhclient"
#define NM_DHCP_MANAGER_PID_FILE_EXT	"pid"

#if defined(TARGET_DEBIAN)
#define NM_DHCP_MANAGER_LEASE_DIR       LOCALSTATEDIR "/lib/dhcp3"
#elif defined(TARGET_SUSE)
#define NM_DHCP_MANAGER_LEASE_DIR       LOCALSTATEDIR "/lib/dhcp"
#else
#define NM_DHCP_MANAGER_LEASE_DIR       LOCALSTATEDIR "/lib/dhclient"
#endif
#define NM_DHCP_MANAGER_LEASE_FILENAME	"dhclient"
#define NM_DHCP_MANAGER_LEASE_FILE_EXT	"lease"

#define ACTION_SCRIPT_PATH	LIBEXECDIR "/nm-dhcp-client.action"


static char *
get_pidfile_for_iface (const char * iface)
{
	return g_strdup_printf ("%s/%s-%s.%s",
	                        NM_DHCP_MANAGER_RUN_DIR,
	                        NM_DHCP_MANAGER_PID_FILENAME,
	                        iface,
	                        NM_DHCP_MANAGER_PID_FILE_EXT);
}


static char *
get_leasefile_for_iface (const char * iface)
{
	return g_strdup_printf ("%s/%s-%s.%s",
	                        NM_DHCP_MANAGER_LEASE_DIR,
	                        NM_DHCP_MANAGER_LEASE_FILENAME,
	                        iface,
	                        NM_DHCP_MANAGER_LEASE_FILE_EXT);
}



#define DHCP_CLIENT_ID_TAG "send dhcp-client-identifier"
#define DHCP_CLIENT_ID_FORMAT DHCP_CLIENT_ID_TAG " \"%s\"; # added by NetworkManager"

#define DHCP_HOSTNAME_TAG "send host-name"
#define DHCP_HOSTNAME_FORMAT DHCP_HOSTNAME_TAG " \"%s\"; # added by NetworkManager"

static gboolean
merge_dhclient_config (NMDHCPDevice *device,
                       NMSettingIP4Config *s_ip4,
                       const char *contents,
                       const char *orig,
                       GError **error)
{
	GString *new_contents;
	gboolean success = FALSE;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (device->iface != NULL, FALSE);
	
	new_contents = g_string_new (_("# Created by NetworkManager\n"));

	/* Add existing options, if any, but ignore stuff NM will replace. */
	if (contents) {
		char **lines = NULL, **line;

		g_string_append_printf (new_contents, _("# Merged from %s\n\n"), orig);

		lines = g_strsplit_set (contents, "\n\r", 0);
		for (line = lines; lines && *line; line++) {
			gboolean ignore = FALSE;

			if (!strlen (g_strstrip (*line)))
				continue;

			if (   s_ip4
			    && nm_setting_ip4_config_get_dhcp_client_id (s_ip4)
			    && !strncmp (*line, DHCP_CLIENT_ID_TAG, strlen (DHCP_CLIENT_ID_TAG)))
				ignore = TRUE;

			if (   s_ip4
			    && nm_setting_ip4_config_get_dhcp_hostname (s_ip4)
			    && !strncmp (*line, DHCP_HOSTNAME_TAG, strlen (DHCP_HOSTNAME_TAG)))
				ignore = TRUE;

			if (!ignore) {
				g_string_append (new_contents, *line);
				g_string_append_c (new_contents, '\n');
			}
		}

		if (lines)
			g_strfreev (lines);
	} else
		g_string_append_c (new_contents, '\n');

	/* Add NM options from connection */
	if (s_ip4) {
		const char *tmp;

		tmp = nm_setting_ip4_config_get_dhcp_client_id (s_ip4);
		if (tmp)
			g_string_append_printf (new_contents, DHCP_CLIENT_ID_FORMAT "\n", tmp);

		tmp = nm_setting_ip4_config_get_dhcp_hostname (s_ip4);
		if (tmp)
			g_string_append_printf (new_contents, DHCP_HOSTNAME_FORMAT "\n", tmp);
	}

	if (g_file_set_contents (device->conf_file, new_contents->str, -1, error))
		success = TRUE;

	g_string_free (new_contents, TRUE);
	return success;
}

/* NM provides interface-specific options; thus the same dhclient config
 * file cannot be used since DHCP transactions can happen in parallel.
 * Since some distros don't have default per-interface dhclient config files,
 * read their single config file and merge that into a custom per-interface
 * config file along with the NM options.
 */
static gboolean
create_dhclient_config (NMDHCPDevice *device, NMSettingIP4Config *s_ip4)
{
	char *orig = NULL, *contents = NULL;
	GError *error = NULL;
	gboolean success = FALSE;
	char *tmp;

	g_return_val_if_fail (device != NULL, FALSE);

#if defined(TARGET_SUSE)
	orig = g_strdup (SYSCONFDIR "/dhclient.conf");
#elif defined(TARGET_DEBIAN)
	orig = g_strdup (SYSCONFDIR "/dhcp3/dhclient.conf");
#else
	orig = g_strdup_printf (SYSCONFDIR "/dhclient-%s.conf", device->iface);
#endif

	if (!orig) {
		nm_warning ("%s: not enough memory for dhclient options.", device->iface);
		return FALSE;
	}

	tmp = g_strdup_printf ("nm-dhclient-%s.conf", device->iface);
	device->conf_file = g_build_filename ("/var", "run", tmp, NULL);
	g_free (tmp);

	if (!g_file_test (orig, G_FILE_TEST_EXISTS))
		goto out;

	if (!g_file_get_contents (orig, &contents, NULL, &error)) {
		nm_warning ("%s: error reading dhclient configuration %s: %s",
		            device->iface, orig, error->message);
		g_error_free (error);
		goto out;
	}

out:
	error = NULL;
	if (merge_dhclient_config (device, s_ip4, contents, orig, &error))
		success = TRUE;
	else {
		nm_warning ("%s: error creating dhclient configuration: %s",
		            device->iface, error->message);
		g_error_free (error);
	}

	g_free (contents);
	g_free (orig);
	return success;
}


static void
dhclient_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}


GPid
nm_dhcp_client_start (NMDHCPDevice *device, NMSettingIP4Config *s_ip4)
{
	GPtrArray *dhclient_argv = NULL;
	GPid pid = 0;
	GError *error = NULL;
	char *pid_contents = NULL;

	if (!g_file_test (DHCP_CLIENT_PATH, G_FILE_TEST_EXISTS)) {
		nm_warning (DHCP_CLIENT_PATH " does not exist.");
		goto out;
	}

	device->pid_file = get_pidfile_for_iface (device->iface);
	if (!device->pid_file) {
		nm_warning ("%s: not enough memory for dhclient options.", device->iface);
		goto out;
	}

	device->lease_file = get_leasefile_for_iface (device->iface);
	if (!device->lease_file) {
		nm_warning ("%s: not enough memory for dhclient options.", device->iface);
		goto out;
	}

	if (!create_dhclient_config (device, s_ip4))
		goto out;

	/* Kill any existing dhclient bound to this interface */
	if (g_file_get_contents (device->pid_file, &pid_contents, NULL, NULL)) {
		unsigned long int tmp = strtoul (pid_contents, NULL, 10);

		if (!((tmp == ULONG_MAX) && (errno == ERANGE)))
			nm_dhcp_client_stop (device, (pid_t) tmp);
		remove (device->pid_file);
	}

	dhclient_argv = g_ptr_array_new ();
	g_ptr_array_add (dhclient_argv, (gpointer) DHCP_CLIENT_PATH);

	g_ptr_array_add (dhclient_argv, (gpointer) "-d");

	g_ptr_array_add (dhclient_argv, (gpointer) "-sf");	/* Set script file */
	g_ptr_array_add (dhclient_argv, (gpointer) ACTION_SCRIPT_PATH );

	g_ptr_array_add (dhclient_argv, (gpointer) "-pf");	/* Set pid file */
	g_ptr_array_add (dhclient_argv, (gpointer) device->pid_file);

	g_ptr_array_add (dhclient_argv, (gpointer) "-lf");	/* Set lease file */
	g_ptr_array_add (dhclient_argv, (gpointer) device->lease_file);

	g_ptr_array_add (dhclient_argv, (gpointer) "-cf");	/* Set interface config file */
	g_ptr_array_add (dhclient_argv, (gpointer) device->conf_file);

	g_ptr_array_add (dhclient_argv, (gpointer) device->iface);
	g_ptr_array_add (dhclient_argv, NULL);

	if (!g_spawn_async (NULL, (char **) dhclient_argv->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                    &dhclient_child_setup, NULL, &pid, &error)) {
		nm_warning ("dhclient failed to start.  error: '%s'", error->message);
		g_error_free (error);
		goto out;
	}

	nm_info ("dhclient started with pid %d", pid);

out:
	g_free (pid_contents);
	g_ptr_array_free (dhclient_argv, TRUE);
	return pid;
}

static const char **
process_rfc3442_route (const char **octets, NMIP4Route **out_route)
{
	const char **o = octets;
	int addr_len = 0, i = 0;
	long int tmp;
	NMIP4Route *route;
	char *next_hop;
	struct in_addr tmp_addr;

	if (!*o)
		return o; /* no prefix */

	tmp = strtol (*o, NULL, 10);
	if (tmp < 0 || tmp > 32)  /* 32 == max IP4 prefix length */
		return o;

	route = nm_ip4_route_new ();
	nm_ip4_route_set_prefix (route, (guint32) tmp);
	o++;

	if (tmp > 0)
		addr_len = ((tmp - 1) / 8) + 1;

	/* ensure there's at least the address + next hop left */
	if (g_strv_length ((char **) o) < addr_len + 4)
		goto error;

	if (tmp) {
		const char *addr[4] = { "0", "0", "0", "0" };
		char *str_addr;

		for (i = 0; i < addr_len; i++)
			addr[i] = *o++;

		str_addr = g_strjoin (".", addr[0], addr[1], addr[2], addr[3], NULL);
		if (inet_pton (AF_INET, str_addr, &tmp_addr) <= 0) {
			g_free (str_addr);
			goto error;
		}
		tmp_addr.s_addr &= nm_utils_ip4_prefix_to_netmask ((guint32) tmp);
		nm_ip4_route_set_dest (route, tmp_addr.s_addr);
	}

	/* Handle next hop */
	next_hop = g_strjoin (".", o[0], o[1], o[2], o[3], NULL);
	if (inet_pton (AF_INET, next_hop, &tmp_addr) <= 0) {
		g_free (next_hop);
		goto error;
	}
	nm_ip4_route_set_next_hop (route, tmp_addr.s_addr);
	g_free (next_hop);

	*out_route = route;
	return o + 4; /* advance to past the next hop */

error:
	nm_ip4_route_unref (route);
	return o;
}

gboolean
nm_dhcp_client_process_classless_routes (GHashTable *options,
                                         NMIP4Config *ip4_config,
                                         guint32 *gwaddr)
{
	const char *str;
	char **octets, **o;
	gboolean have_routes = FALSE;
	NMIP4Route *route = NULL;

	/* dhclient doesn't have actual support for rfc3442 classless static routes
	 * upstream.  Thus, people resort to defining the option in dhclient.conf
	 * and using arbitrary formats like so:
	 *
	 * option rfc3442-classless-static-routes code 121 = array of unsigned integer 8;
	 *
	 * See https://lists.isc.org/pipermail/dhcp-users/2008-December/007629.html
	 */

	str = g_hash_table_lookup (options, "new_rfc3442_classless_static_routes");
	/* Microsoft version; same as rfc3442 but with a different option # (249) */
	if (!str)
		str = g_hash_table_lookup (options, "new_ms_classless_static_routes");

	if (!str || !strlen (str))
		return FALSE;

	o = octets = g_strsplit (str, " ", 0);
	if (g_strv_length (octets) < 5) {
		nm_warning ("Ignoring invalid classless static routes '%s'", str);
		goto out;
	}

	while (*o) {
		route = NULL;
		o = (char **) process_rfc3442_route ((const char **) o, &route);
		if (!route) {
			nm_warning ("Ignoring invalid classless static routes");
			break;
		}

		have_routes = TRUE;
		if (nm_ip4_route_get_prefix (route) == 0) {
			/* gateway passed as classless static route */
			*gwaddr = nm_ip4_route_get_next_hop (route);
			nm_ip4_route_unref (route);
		} else {
			char addr[INET_ADDRSTRLEN + 1];
			char nh[INET_ADDRSTRLEN + 1];
			struct in_addr tmp;

			/* normal route */
			nm_ip4_config_take_route (ip4_config, route);

			tmp.s_addr = nm_ip4_route_get_dest (route);
			inet_ntop (AF_INET, &tmp, addr, sizeof (addr));
			tmp.s_addr = nm_ip4_route_get_next_hop (route);
			inet_ntop (AF_INET, &tmp, nh, sizeof (nh));
			nm_info ("  classless static route %s/%d gw %s",
			         addr, nm_ip4_route_get_prefix (route), nh);
		}
	}

out:
	g_strfreev (octets);
	return have_routes;
}

