/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2015-2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-acd-manager.h"

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "platform/nm-platform.h"
#include "nm-utils.h"
#include "NetworkManagerUtils.h"
#include "n-acd/src/n-acd.h"

/*****************************************************************************/

typedef enum {
	STATE_INIT,
	STATE_PROBING,
	STATE_PROBE_DONE,
	STATE_ANNOUNCING,
} State;

typedef struct {
	in_addr_t address;
	gboolean duplicate;
	NAcdProbe *probe;
} AddressInfo;

enum {
	PROBE_TERMINATED,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct {
	int            ifindex;
	guint8         hwaddr[ETH_ALEN];
	State          state;
	GHashTable    *addresses;
	guint          completed;
	NAcd          *acd;
	GIOChannel    *channel;
	guint          event_id;
} NMAcdManagerPrivate;

struct _NMAcdManager {
	GObject parent;
	NMAcdManagerPrivate _priv;
};

struct _NMAcdManagerClass {
	GObjectClass parent;
};

G_DEFINE_TYPE (NMAcdManager, nm_acd_manager, G_TYPE_OBJECT)

#define NM_ACD_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMAcdManager, NM_IS_ACD_MANAGER)

/*****************************************************************************/

#define _NMLOG_DOMAIN         LOGD_IP4
#define _NMLOG_PREFIX_NAME    "acd"
#define _NMLOG(level, ...) \
    G_STMT_START { \
        char _sbuf[64]; \
        int _ifindex = (self) ? NM_ACD_MANAGER_GET_PRIVATE (self)->ifindex : 0; \
        \
        nm_log ((level), _NMLOG_DOMAIN, \
                nm_platform_link_get_name (NM_PLATFORM_GET, _ifindex), \
                NULL, \
                "%s%s: " _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
                _NMLOG_PREFIX_NAME, \
                self ? nm_sprintf_buf (_sbuf, "[%p,%d]", self, _ifindex) : "" \
                _NM_UTILS_MACRO_REST (__VA_ARGS__)); \
    } G_STMT_END

/*****************************************************************************/

static const char *
_acd_event_to_string (unsigned int event)
{
	switch (event) {
	case N_ACD_EVENT_READY:
		return "ready";
	case N_ACD_EVENT_USED:
		return "used";
	case N_ACD_EVENT_DEFENDED:
		return "defended";
	case N_ACD_EVENT_CONFLICT:
		return "conflict";
	case N_ACD_EVENT_DOWN:
		return "down";
	}
	return NULL;
}

#define acd_event_to_string(event) NM_UTILS_LOOKUP_STR (_acd_event_to_string, event)

static const char *
_acd_error_to_string (int error)
{
	if (error < 0)
		return g_strerror (-error);

	switch (error) {
	case _N_ACD_E_SUCCESS:
		return "success";
	case N_ACD_E_PREEMPTED:
		return "preempted";
	case N_ACD_E_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return NULL;
}

#define acd_error_to_string(error) NM_UTILS_LOOKUP_STR (_acd_error_to_string, error)

/*****************************************************************************/

/**
 * nm_acd_manager_add_address:
 * @self: a #NMAcdManager
 * @address: an IP address
 *
 * Add @address to the list of IP addresses to probe.

 * Returns: %TRUE on success, %FALSE if the address was already in the list
 */
gboolean
nm_acd_manager_add_address (NMAcdManager *self, in_addr_t address)
{
	NMAcdManagerPrivate *priv;
	AddressInfo *info;

	g_return_val_if_fail (NM_IS_ACD_MANAGER (self), FALSE);
	priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	g_return_val_if_fail (priv->state == STATE_INIT, FALSE);

	if (g_hash_table_lookup (priv->addresses, GUINT_TO_POINTER (address)))
		return FALSE;

	info = g_slice_new0 (AddressInfo);
	info->address = address;

	g_hash_table_insert (priv->addresses, GUINT_TO_POINTER (address), info);

	return TRUE;
}

static gboolean
acd_event (GIOChannel *source, GIOCondition condition, gpointer data)
{
	NMAcdManager *self = data;
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	NAcdEvent *event;
	AddressInfo *info;
	char address_str[INET_ADDRSTRLEN];
	gs_free char *hwaddr_str = NULL;
	int r;

	if (n_acd_dispatch (priv->acd))
		return G_SOURCE_CONTINUE;

	while (!n_acd_pop_event (priv->acd, &event) && event) {
		switch (event->event) {
		case N_ACD_EVENT_READY:
			n_acd_probe_get_userdata (event->ready.probe, (void **) &info);
			info->duplicate = FALSE;
			if (priv->state == STATE_ANNOUNCING) {
				/* fake probe ended, start announcing */
				r = n_acd_probe_announce (info->probe, N_ACD_DEFEND_ONCE);
				if (r) {
					_LOGW ("couldn't announce address %s on interface '%s': %s",
					       nm_utils_inet4_ntop (info->address, address_str),
					       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
					       acd_error_to_string (r));
				} else {
					_LOGD ("announcing address %s",
					       nm_utils_inet4_ntop (info->address, address_str));
				}
			}
			break;
		case N_ACD_EVENT_USED:
			n_acd_probe_get_userdata (event->used.probe, (void **) &info);
			info->duplicate = TRUE;
			break;
		case N_ACD_EVENT_DEFENDED:
			n_acd_probe_get_userdata (event->defended.probe, (void **) &info);
			_LOGD ("defended address %s from host %s",
			       nm_utils_inet4_ntop (info->address, address_str),
			       (hwaddr_str = nm_utils_hwaddr_ntoa (event->defended.sender,
			                                           event->defended.n_sender)));
			return G_SOURCE_CONTINUE;
		case N_ACD_EVENT_CONFLICT:
			n_acd_probe_get_userdata (event->conflict.probe, (void **) &info);
			_LOGW ("conflict for address %s detected with host %s on interface '%s'",
			       nm_utils_inet4_ntop (info->address, address_str),
			       (hwaddr_str = nm_utils_hwaddr_ntoa (event->defended.sender,
			                                           event->defended.n_sender)),
			       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex));
			return G_SOURCE_CONTINUE;
		default:
			_LOGD ("unhandled event '%s'", acd_event_to_string (event->event));
			return G_SOURCE_CONTINUE;
		}

		if (   priv->state == STATE_PROBING
		    && ++priv->completed == g_hash_table_size (priv->addresses)) {
			priv->state = STATE_PROBE_DONE;
			g_signal_emit (self, signals[PROBE_TERMINATED], 0);
		}
	}

	return G_SOURCE_CONTINUE;
}

static gboolean
acd_probe_add (NMAcdManager *self,
               AddressInfo *info,
               guint64 timeout)
{
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	NAcdProbeConfig *probe_config;
	int r;

	r = n_acd_probe_config_new (&probe_config);
	if (r) {
		_LOGW ("could not create probe config for %s on interface '%s': %s",
		       nm_utils_inet4_ntop (info->address, NULL),
		       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
		       acd_error_to_string (r));
		return FALSE;
	}

	n_acd_probe_config_set_ip (probe_config, (struct in_addr) { info->address });
	n_acd_probe_config_set_timeout (probe_config, timeout);

	r = n_acd_probe (priv->acd, &info->probe, probe_config);
	if (r) {
		_LOGW ("could not start probe for %s on interface '%s': %s",
		       nm_utils_inet4_ntop (info->address, NULL),
		       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
		       acd_error_to_string (r));
		n_acd_probe_config_free (probe_config);
		return FALSE;
	}

	n_acd_probe_set_userdata (info->probe, info);
	n_acd_probe_config_free (probe_config);

	return TRUE;
}

static int
acd_init (NMAcdManager *self)
{
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	NAcdConfig *config;
	int r;

	if (priv->acd)
		return 0;

	r = n_acd_config_new (&config);
	if (r)
		return r;

	n_acd_config_set_ifindex (config, priv->ifindex);
	n_acd_config_set_transport (config, N_ACD_TRANSPORT_ETHERNET);
	n_acd_config_set_mac (config, priv->hwaddr, ETH_ALEN);

	r = n_acd_new (&priv->acd, config);
	n_acd_config_free (config);
	return r;
}

/**
 * nm_acd_manager_start_probe:
 * @self: a #NMAcdManager
 * @timeout: maximum probe duration in milliseconds
 * @error: location to store error, or %NULL
 *
 * Start probing IP addresses for duplicates; when the probe terminates a
 * PROBE_TERMINATED signal is emitted.
 *
 * Returns: %TRUE if at least one probe could be started, %FALSE otherwise
 */
gboolean
nm_acd_manager_start_probe (NMAcdManager *self, guint timeout)
{
	NMAcdManagerPrivate *priv;
	GHashTableIter iter;
	AddressInfo *info;
	gboolean success = FALSE;
	int fd, r;

	g_return_val_if_fail (NM_IS_ACD_MANAGER (self), FALSE);
	priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	g_return_val_if_fail (priv->state == STATE_INIT, FALSE);

	r = acd_init (self);
	if (r) {
		_LOGW ("couldn't init ACD for probing on interface '%s': %s",
		       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
		       acd_error_to_string (r));
		return FALSE;
	}

	priv->completed = 0;

	g_hash_table_iter_init (&iter, priv->addresses);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &info))
		success |= acd_probe_add (self, info, timeout);

	if (success)
		priv->state = STATE_PROBING;

	n_acd_get_fd (priv->acd, &fd);
	priv->channel = g_io_channel_unix_new (fd);
	priv->event_id = g_io_add_watch (priv->channel, G_IO_IN, acd_event, self);

	return success;
}

/**
 * nm_acd_manager_reset:
 * @self: a #NMAcdManager
 *
 * Stop any operation in progress and reset @self to the initial state.
 */
void
nm_acd_manager_reset (NMAcdManager *self)
{
	NMAcdManagerPrivate *priv;

	g_return_if_fail (NM_IS_ACD_MANAGER (self));
	priv = NM_ACD_MANAGER_GET_PRIVATE (self);

	g_hash_table_remove_all (priv->addresses);

	priv->state = STATE_INIT;
}

/**
 * nm_acd_manager_destroy:
 * @self: the #NMAcdManager
 *
 * Calls nm_acd_manager_reset() and unrefs @self.
 */
void
nm_acd_manager_destroy (NMAcdManager *self)
{
	g_return_if_fail (NM_IS_ACD_MANAGER (self));

	nm_acd_manager_reset (self);
	g_object_unref (self);
}

/**
 * nm_acd_manager_check_address:
 * @self: a #NMAcdManager
 * @address: an IP address
 *
 * Check if an IP address is duplicate. @address must have been added with
 * nm_acd_manager_add_address().
 *
 * Returns: %TRUE if the address is not duplicate, %FALSE otherwise
 */
gboolean
nm_acd_manager_check_address (NMAcdManager *self, in_addr_t address)
{
	NMAcdManagerPrivate *priv;
	AddressInfo *info;

	g_return_val_if_fail (NM_IS_ACD_MANAGER (self), FALSE);
	priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	g_return_val_if_fail (   priv->state == STATE_INIT
	                      || priv->state == STATE_PROBE_DONE, FALSE);

	info = g_hash_table_lookup (priv->addresses, GUINT_TO_POINTER (address));
	g_return_val_if_fail (info, FALSE);

	return !info->duplicate;
}

/**
 * nm_acd_manager_announce_addresses:
 * @self: a #NMAcdManager
 *
 * Start announcing addresses.
 */
void
nm_acd_manager_announce_addresses (NMAcdManager *self)
{
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	GHashTableIter iter;
	AddressInfo *info;
	int r;

	r = acd_init (self);
	if (r) {
		_LOGW ("couldn't init ACD for announcing address %s on interface '%s': %s",
		       nm_utils_inet4_ntop (info->address, NULL),
		       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
		       acd_error_to_string (r));
		return;
	}

	if (priv->state == STATE_INIT) {
		/* n-acd can't announce without probing, therefore let's
		 * start a fake probe with zero timeout and then perform
		 * the announcement. */
		g_hash_table_iter_init (&iter, priv->addresses);
		while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &info))
			acd_probe_add (self, info, 0);
		priv->state = STATE_ANNOUNCING;
	} else if (priv->state == STATE_ANNOUNCING) {
		g_hash_table_iter_init (&iter, priv->addresses);
		while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &info)) {
			if (info->duplicate)
				continue;
			r = n_acd_probe_announce (info->probe, N_ACD_DEFEND_ONCE);
			if (r) {
				_LOGW ("couldn't announce address %s on interface '%s': %s",
				       nm_utils_inet4_ntop (info->address, NULL),
				       nm_platform_link_get_name (NM_PLATFORM_GET, priv->ifindex),
				       acd_error_to_string (r));
			} else
				_LOGD ("announcing address %s", nm_utils_inet4_ntop (info->address, NULL));
		}
	}
}

static void
destroy_address_info (gpointer data)
{
	AddressInfo *info = (AddressInfo *) data;

	n_acd_probe_free (info->probe);

	g_slice_free (AddressInfo, info);
}

/*****************************************************************************/

static void
nm_acd_manager_init (NMAcdManager *self)
{
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);

	priv->addresses = g_hash_table_new_full (nm_direct_hash, NULL,
	                                         NULL, destroy_address_info);
	priv->state = STATE_INIT;
}

NMAcdManager *
nm_acd_manager_new (int ifindex, const guint8 *hwaddr, guint hwaddr_len)
{
	NMAcdManager *self;
	NMAcdManagerPrivate *priv;

	g_return_val_if_fail (ifindex > 0, NULL);
	g_return_val_if_fail (hwaddr, NULL);
	g_return_val_if_fail (hwaddr_len == ETH_ALEN, NULL);

	self = g_object_new (NM_TYPE_ACD_MANAGER, NULL);
	priv = NM_ACD_MANAGER_GET_PRIVATE (self);
	priv->ifindex = ifindex;
	memcpy (priv->hwaddr, hwaddr, ETH_ALEN);

	return self;
}

static void
dispose (GObject *object)
{
	NMAcdManager *self = NM_ACD_MANAGER (object);
	NMAcdManagerPrivate *priv = NM_ACD_MANAGER_GET_PRIVATE (self);

	g_clear_pointer (&priv->addresses, g_hash_table_destroy);
	g_clear_pointer (&priv->channel, g_io_channel_unref);
	nm_clear_g_source (&priv->event_id);
	nm_clear_pointer (&priv->acd, n_acd_unref);

	G_OBJECT_CLASS (nm_acd_manager_parent_class)->dispose (object);
}

static void
nm_acd_manager_class_init (NMAcdManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = dispose;

	signals[PROBE_TERMINATED] =
	    g_signal_new (NM_ACD_MANAGER_PROBE_TERMINATED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  0, NULL, NULL, NULL,
	                  G_TYPE_NONE, 0);
}
