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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2010 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 * Copyright (C) 2005 Ray Strode
 *
 * Some code borrowed from HAL:  
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2004 Novell, Inc.
 */

/* for struct ucred */
#include <config.h>

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if.h>
#include <linux/unistd.h>
#include <unistd.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gi18n.h>

#include "NetworkManager.h"
#include "nm-system.h"
#include "nm-netlink-monitor.h"
#include "nm-logging.h"
#include "nm-netlink.h"

#define EVENT_CONDITIONS      ((GIOCondition) (G_IO_IN | G_IO_PRI))
#define ERROR_CONDITIONS      ((GIOCondition) (G_IO_ERR | G_IO_NVAL))
#define DISCONNECT_CONDITIONS ((GIOCondition) (G_IO_HUP))

#define NM_NETLINK_MONITOR_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
                                           NM_TYPE_NETLINK_MONITOR, \
                                           NMNetlinkMonitorPrivate))

typedef struct {
	struct nl_handle *nlh;
	struct nl_cb *    nlh_cb;
	struct nl_cache * nlh_link_cache;

	GIOChannel *	  io_channel;
	guint             event_id;

	guint request_status_id;
} NMNetlinkMonitorPrivate;

enum {
	NOTIFICATION = 0,
	CARRIER_ON,
	CARRIER_OFF,
	ERROR,
	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };


G_DEFINE_TYPE (NMNetlinkMonitor, nm_netlink_monitor, G_TYPE_OBJECT);

static void
link_msg_handler (struct nl_object *obj, void *arg)
{
	NMNetlinkMonitor *self = NM_NETLINK_MONITOR (arg);
	GError *error;
	struct rtnl_link *filter;
	struct rtnl_link *link_obj;
	guint flags;
	guint ifidx;

	filter = rtnl_link_alloc ();
	if (!filter) {
		error = g_error_new (NM_NETLINK_MONITOR_ERROR,
		                     NM_NETLINK_MONITOR_ERROR_BAD_ALLOC,
		                     _("error processing netlink message: %s"),
		                     nl_geterror ());
		g_signal_emit (self, signals[ERROR], 0, error);
		g_error_free (error);
		return;
	}

	/* Ensure it's a link object */
	if (nl_object_match_filter(obj, OBJ_CAST (filter)) == 0)
		goto out;

	link_obj = (struct rtnl_link *) obj;
	flags = rtnl_link_get_flags (link_obj);
	ifidx = rtnl_link_get_ifindex (link_obj);

	nm_log_dbg (LOGD_HW, "netlink link message: iface idx %d flags 0x%X", ifidx, flags);

	/* IFF_LOWER_UP is the indicator of carrier status since kernel commit
	 * b00055aacdb172c05067612278ba27265fcd05ce in 2.6.17.
	 */
	if (flags & IFF_LOWER_UP)
		g_signal_emit (self, signals[CARRIER_ON], 0, ifidx);
	else
		g_signal_emit (self, signals[CARRIER_OFF], 0, ifidx);

out:
	rtnl_link_put (filter);
}

static int
netlink_event_input (struct nl_msg *msg, void *arg)
{
	NMNetlinkMonitor *self = NM_NETLINK_MONITOR (arg);
	NMNetlinkMonitorPrivate *priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	struct nlmsghdr *hdr = nlmsg_hdr (msg);
	struct ucred *creds = nlmsg_get_creds (msg);
	guint32 local_port;
	gboolean accept_msg = FALSE;

	/* Only messages sent from the kernel */
	if (!creds || creds->uid != 0) {
		nm_log_dbg (LOGD_HW, "ignoring netlink message from UID %d",
		            creds ? creds->uid : -1);
		return NL_STOP;
	}

	/* Accept any messages from the kernel */
	if (hdr->nlmsg_pid == 0)
		accept_msg = TRUE;

	/* And any multicast message directed to our netlink PID, since multicast
	 * currently requires CAP_ADMIN to use.
	 */
	local_port = nl_socket_get_local_port (priv->nlh);
	if ((hdr->nlmsg_pid == local_port) && (hdr->nlmsg_flags & NLM_F_MULTI))
		accept_msg = TRUE;

	if (accept_msg == FALSE) {
		nm_log_dbg (LOGD_HW, "ignoring netlink message from PID %d (local PID %d, multicast %d)",
		            hdr->nlmsg_pid,
		            local_port,
		            (hdr->nlmsg_flags & NLM_F_MULTI));
		return NL_STOP;
	}

	/* Let clients handle generic messages */
	g_signal_emit (self, signals[NOTIFICATION], 0, msg);

	/* Parse carrier messages */
	nl_msg_parse (msg, &link_msg_handler, self);

	return NL_OK;
}

static gboolean
event_handler (GIOChannel *channel,
               GIOCondition io_condition,
               gpointer user_data)
{
	NMNetlinkMonitor *self = (NMNetlinkMonitor *) user_data;
	NMNetlinkMonitorPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (NM_IS_NETLINK_MONITOR (self), TRUE);

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_val_if_fail (priv->event_id > 0, TRUE);

	if (io_condition & ERROR_CONDITIONS) {
		const char *err_msg;
		int err_code = 0;
		socklen_t err_len = sizeof (err_code);

		/* Grab error information */	 
		if (getsockopt (g_io_channel_unix_get_fd (channel), 
						SOL_SOCKET, SO_ERROR, (void *) &err_code, &err_len))
			err_msg = strerror (err_code);
		else
			err_msg = _("error occurred while waiting for data on socket");

		error = g_error_new (NM_NETLINK_MONITOR_ERROR,
		                     NM_NETLINK_MONITOR_ERROR_WAITING_FOR_SOCKET_DATA,
		                     "%s", err_msg);
		g_signal_emit (self, signals[ERROR], 0, error);
		g_error_free (error);
		return TRUE;
	} else if (io_condition & DISCONNECT_CONDITIONS)
		return FALSE;

	g_return_val_if_fail (!(io_condition & ~EVENT_CONDITIONS), FALSE);

	/* Process the netlink messages */
	if (nl_recvmsgs_default (priv->nlh) < 0) {
		error = g_error_new (NM_NETLINK_MONITOR_ERROR,
		                     NM_NETLINK_MONITOR_ERROR_PROCESSING_MESSAGE,
		                     _("error processing netlink message: %s"),
		                     nl_geterror ());
		g_signal_emit (self, signals[ERROR], 0, error);
		g_error_free (error);
	}

	return TRUE;
}

gboolean
nm_netlink_monitor_open_connection (NMNetlinkMonitor *self, GError **error)
{
	NMNetlinkMonitorPrivate *priv;
	GError *channel_error = NULL;
	GIOFlags channel_flags;
	int fd;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_NETLINK_MONITOR (self), FALSE);

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_val_if_fail (priv->io_channel == NULL, FALSE);

	priv->nlh_cb = nl_cb_alloc (NL_CB_VERBOSE);
	priv->nlh = nl_handle_alloc_cb (priv->nlh_cb);
	if (!priv->nlh) {
		g_set_error (error, NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_NETLINK_ALLOC_HANDLE,
		             _("unable to allocate netlink handle for monitoring link status: %s"),
		             nl_geterror ());
		goto error;
	}

	nl_disable_sequence_check (priv->nlh);
	nl_socket_modify_cb (priv->nlh, NL_CB_VALID, NL_CB_CUSTOM, netlink_event_input, self);
	if (nl_connect (priv->nlh, NETLINK_ROUTE) < 0) {
		g_set_error (error, NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_NETLINK_CONNECT,
		             _("unable to connect to netlink for monitoring link status: %s"),
		             nl_geterror ());
		goto error;
	}

	/* Enable unix socket peer credentials which we use for verifying that the
	 * sender of the message is actually the kernel.
	 */
	if (nl_set_passcred (priv->nlh, 1) < 0) {
		g_set_error (error, NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_NETLINK_CONNECT,
		             _("unable to enable netlink handle credential passing: %s"),
		             nl_geterror ());
		goto error;
	}

	if (nl_socket_add_membership (priv->nlh, RTNLGRP_LINK) < 0) {
		g_set_error (error, NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_NETLINK_JOIN_GROUP,
		             _("unable to join netlink group for monitoring link status: %s"),
		             nl_geterror ());
		goto error;
	}

	if ((priv->nlh_link_cache = rtnl_link_alloc_cache (priv->nlh)) == NULL) {
		g_set_error (error, NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_NETLINK_ALLOC_LINK_CACHE,
		             _("unable to allocate netlink link cache for monitoring link status: %s"),
		             nl_geterror ());
		goto error;
	}

	nl_cache_mngt_provide (priv->nlh_link_cache);

	fd = nl_socket_get_fd (priv->nlh);
	priv->io_channel = g_io_channel_unix_new (fd);

	g_io_channel_set_encoding (priv->io_channel, NULL, &channel_error);
	/* Encoding is NULL, so no conversion error can possibly occur */
	g_assert (channel_error == NULL);

	g_io_channel_set_close_on_unref (priv->io_channel, TRUE);
	channel_flags = g_io_channel_get_flags (priv->io_channel);
	channel_error = NULL;
	g_io_channel_set_flags (priv->io_channel,
	                        channel_flags | G_IO_FLAG_NONBLOCK,
	                        &channel_error);
	if (channel_error != NULL) {
		g_propagate_error (error, channel_error);
		goto error;
	}

	return TRUE;

error:
	if (priv->io_channel)
		nm_netlink_monitor_close_connection (self);

	if (priv->nlh_link_cache) {
		nl_cache_free (priv->nlh_link_cache);
		priv->nlh_link_cache = NULL;
	}

	if (priv->nlh) {
		nl_handle_destroy (priv->nlh);
		priv->nlh = NULL;
	}

	if (priv->nlh_cb) {
		nl_cb_put (priv->nlh_cb);
		priv->nlh_cb = NULL;
	}
	return FALSE;
}

void
nm_netlink_monitor_close_connection (NMNetlinkMonitor  *self)
{
	NMNetlinkMonitorPrivate *priv;

	g_return_if_fail (NM_IS_NETLINK_MONITOR (self));

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_if_fail (priv->io_channel != NULL);

	if (priv->event_id)
		nm_netlink_monitor_detach (self);

	g_io_channel_shutdown (priv->io_channel,
			       TRUE /* flush pending data */,
			       NULL);

	g_io_channel_unref (priv->io_channel);
	priv->io_channel = NULL;
}

void
nm_netlink_monitor_attach (NMNetlinkMonitor *self)
{
	NMNetlinkMonitorPrivate *priv;

	g_return_if_fail (NM_IS_NETLINK_MONITOR (self));

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_if_fail (priv->nlh != NULL);
	g_return_if_fail (priv->event_id == 0);

	priv->event_id = g_io_add_watch (priv->io_channel,
	                                 (EVENT_CONDITIONS | ERROR_CONDITIONS | DISCONNECT_CONDITIONS),
	                                 event_handler, self);
}

void
nm_netlink_monitor_detach (NMNetlinkMonitor *self)
{
	NMNetlinkMonitorPrivate *priv;

	g_return_if_fail (NM_IS_NETLINK_MONITOR (self));

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_if_fail (priv->event_id > 0);

	g_source_remove (priv->event_id);
	priv->event_id = 0;
}

static gboolean
deferred_emit_carrier_state (gpointer user_data)
{
	NMNetlinkMonitor *self = NM_NETLINK_MONITOR (user_data);
	NMNetlinkMonitorPrivate *priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);

	priv->request_status_id = 0;

	/* Update the link cache with latest state, and if there are no errors
	 * emit the link states for all the interfaces in the cache.
	 */
	if (nl_cache_refill (priv->nlh, priv->nlh_link_cache)) {
		nm_log_err (LOGD_HW, "error updating link cache: %s", nl_geterror ());
	} else
		nl_cache_foreach_filter (priv->nlh_link_cache, NULL, link_msg_handler, self);

	return FALSE;
}

gboolean
nm_netlink_monitor_request_status (NMNetlinkMonitor *self, GError **error)
{
	NMNetlinkMonitorPrivate *priv;

	g_return_val_if_fail (NM_IS_NETLINK_MONITOR (self), FALSE);

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);
	g_return_val_if_fail (priv->event_id > 0, FALSE);

	/* Schedule the carrier state emission */
	if (!priv->request_status_id)
		priv->request_status_id = g_idle_add (deferred_emit_carrier_state, self);

	return TRUE;
}

typedef struct {
	NMNetlinkMonitor *self;
	struct rtnl_link *filter;
	GError *error;
	guint32 flags;
} GetFlagsInfo;

static void
get_flags_sync_cb (struct nl_object *obj, void *arg)
{
	GetFlagsInfo *info = arg;

	/* Ensure this cache item matches our filter */
	if (nl_object_match_filter (obj, OBJ_CAST (info->filter)) != 0)
		info->flags = rtnl_link_get_flags ((struct rtnl_link *) obj);
}

gboolean
nm_netlink_monitor_get_flags_sync (NMNetlinkMonitor *self,
                                   guint32 ifindex,
                                   guint32 *ifflags,
                                   GError **error)
{
	NMNetlinkMonitorPrivate *priv;
	GetFlagsInfo info;
	struct rtnl_link *filter;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_NETLINK_MONITOR (self), FALSE);
	g_return_val_if_fail (ifflags != NULL, FALSE);

	priv = NM_NETLINK_MONITOR_GET_PRIVATE (self);

	/* Update the link cache with the latest information */
	if (nl_cache_refill (priv->nlh, priv->nlh_link_cache)) {
		g_set_error (error,
		             NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_LINK_CACHE_UPDATE,
		             _("error updating link cache: %s"),
		             nl_geterror ());
		return FALSE;
	}

	/* HACK: Apparently to get it working we have to refill the cache twice;
	 * otherwise some kernels (or maybe libnl?) only send a few of the
	 * interfaces in the refill request.
	 */
	if (nl_cache_refill (priv->nlh, priv->nlh_link_cache)) {
		g_set_error (error,
		             NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_LINK_CACHE_UPDATE,
		             _("error updating link cache: %s"),
		             nl_geterror ());
		return FALSE;
	}

	/* Set up the filter */
	filter = rtnl_link_alloc ();
	if (!filter) {
		g_set_error (error,
		             NM_NETLINK_MONITOR_ERROR,
		             NM_NETLINK_MONITOR_ERROR_BAD_ALLOC,
		             _("error processing netlink message: %s"),
		             nl_geterror ());
		return FALSE;
	}
	rtnl_link_set_ifindex (filter, ifindex);

	memset (&info, 0, sizeof (info));
	info.self = self;
	info.filter = filter;
	info.error = NULL;
	nl_cache_foreach_filter (priv->nlh_link_cache, NULL, get_flags_sync_cb, &info);

	rtnl_link_put (filter);

	if (info.error) {
		if (error)
			*error = info.error;
		else
			g_error_free (info.error);
		return FALSE;
	} else
		*ifflags = info.flags;

	return TRUE; /* success */
}

/***************************************************************/

NMNetlinkMonitor *
nm_netlink_monitor_get (void)
{
	static NMNetlinkMonitor *singleton = NULL;

	if (!singleton)
		singleton = NM_NETLINK_MONITOR (g_object_new (NM_TYPE_NETLINK_MONITOR, NULL));
	else
		g_object_ref (singleton);

	return singleton;
}

static void
nm_netlink_monitor_init (NMNetlinkMonitor *self)
{
}

static void
finalize (GObject *object)
{
	NMNetlinkMonitorPrivate *priv = NM_NETLINK_MONITOR_GET_PRIVATE (object);

	if (priv->request_status_id)
		g_source_remove (priv->request_status_id);

	if (priv->io_channel)
		nm_netlink_monitor_close_connection (NM_NETLINK_MONITOR (object));

	if (priv->nlh_link_cache) {
		nl_cache_free (priv->nlh_link_cache);
		priv->nlh_link_cache = NULL;
	}

	if (priv->nlh) {
		nl_handle_destroy (priv->nlh);
		priv->nlh = NULL;
	}

	if (priv->nlh_cb) {
		nl_cb_put (priv->nlh_cb);
		priv->nlh_cb = NULL;
	}

	G_OBJECT_CLASS (nm_netlink_monitor_parent_class)->finalize (object);
}

static void
nm_netlink_monitor_class_init (NMNetlinkMonitorClass *monitor_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (monitor_class);

	g_type_class_add_private (monitor_class, sizeof (NMNetlinkMonitorPrivate));

	/* Virtual methods */
	object_class->finalize = finalize;

	/* Signals */
	signals[NOTIFICATION] =
		g_signal_new ("notification",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (NMNetlinkMonitorClass, notification),
		              NULL, NULL, g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);

	signals[CARRIER_ON] =
		g_signal_new ("carrier-on",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (NMNetlinkMonitorClass, carrier_on),
		              NULL, NULL, g_cclosure_marshal_VOID__INT,
		              G_TYPE_NONE, 1, G_TYPE_INT);

	signals[CARRIER_OFF] =
		g_signal_new ("carrier-off",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (NMNetlinkMonitorClass, carrier_off),
		              NULL, NULL, g_cclosure_marshal_VOID__INT,
		              G_TYPE_NONE, 1, G_TYPE_INT);

	signals[ERROR] =
		g_signal_new ("error",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (NMNetlinkMonitorClass, error),
		              NULL, NULL, g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);
}

GQuark
nm_netlink_monitor_error_quark (void)
{
	static GQuark error_quark = 0;

	if (G_UNLIKELY (error_quark == 0))
		error_quark = g_quark_from_static_string ("nm-netlink-monitor-error-quark");
	return error_quark;
}

