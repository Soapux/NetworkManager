/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service - keyfile plugin
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
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2008 - 2012 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nms-keyfile-storage.h"

#include "nms-keyfile-reader.h"
#include "nms-keyfile-writer.h"
#include "nms-keyfile-utils.h"

/*****************************************************************************/

typedef struct {
	char *filename;
} NMSKeyfileStoragePrivate;

struct _NMSKeyfileStorage {
	NMSettingsStorage parent;
	NMSKeyfileStoragePrivate _priv;
	struct {
		bool dirty:1;
	} _intern;
};

struct _NMSKeyfileStorageClass {
	NMSettingsStorageClass parent;
};

G_DEFINE_TYPE (NMSKeyfileStorage, nms_keyfile_storage, NM_TYPE_SETTINGS_STORAGE)

#define NMS_KEYFILE_STORAGE_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMSKeyfileStorage, NMS_IS_KEYFILE_STORAGE)

/*****************************************************************************/

static NMConnection *
load_connection (NMSettingsStorage *storage,
                 GError **error)
{
	NMSKeyfileStorage *self = NMS_KEYFILE_STORAGE (storage);
	NMSKeyfileStoragePrivate *priv = NMS_KEYFILE_STORAGE_GET_PRIVATE (self);
	NMConnection *connection;

	connection = nms_keyfile_reader_from_file (priv->filename, error);
	if (!connection)
		return NULL;

	nm_assert (nm_connection_verify (connection, NULL));
	nm_assert (nm_connection_get_uuid (connection));

	return connection;
}

static gboolean
commit_changes (NMSettingsStorage *storage,
                NMConnection *connection,
                NMSettingsStorageCommitReason commit_reason,
                NMConnection **out_reread_connection,
                char **out_logmsg_change,
                GError **error)
{
	NMSKeyfileStorage *self = NMS_KEYFILE_STORAGE (storage);
	NMSKeyfileStoragePrivate *priv = NMS_KEYFILE_STORAGE_GET_PRIVATE (self);
	gs_free char *filename = NULL;
	gs_unref_object NMConnection *reread = NULL;
	gboolean reread_same = FALSE;

	nm_assert (out_reread_connection && !*out_reread_connection);
	nm_assert (!out_logmsg_change || !*out_logmsg_change);

	if (!nms_keyfile_writer_connection (connection,
	                                    priv->filename,
	                                    NM_FLAGS_ALL (commit_reason,   NM_SETTINGS_STORAGE_COMMIT_REASON_USER_ACTION
	                                                                 | NM_SETTINGS_STORAGE_COMMIT_REASON_ID_CHANGED),
	                                    &filename,
	                                    &reread,
	                                    &reread_same,
	                                    error))
		return FALSE;

	if (!nm_streq0 (filename, priv->filename)) {
		gs_free char *old_filename = g_steal_pointer (&priv->filename);

		priv->filename = g_steal_pointer (&filename);

		if (old_filename) {
			NM_SET_OUT (out_logmsg_change,
			            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s) and rename from \"%s\"",
			                             priv->filename,
			                             nm_connection_get_id (connection),
			                             nm_connection_get_uuid (connection),
			                             old_filename));
		} else {
			NM_SET_OUT (out_logmsg_change,
			            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s) and persist connection",
			                             priv->filename,
			                             nm_connection_get_id (connection),
			                             nm_connection_get_uuid (connection)));
		}
	} else {
		NM_SET_OUT (out_logmsg_change,
		            g_strdup_printf ("keyfile: update \"%s\" (\"%s\", %s)",
		                             priv->filename,
		                             nm_connection_get_id (connection),
		                             nm_connection_get_uuid (connection)));
	}

	if (reread && !reread_same)
		*out_reread_connection = g_steal_pointer (&reread);

	return TRUE;
}

static gboolean
delete (NMSettingsStorage *storage,
        GError **error)
{
	NMSKeyfileStorage *self = NMS_KEYFILE_STORAGE (storage);
	NMSKeyfileStoragePrivate *priv = NMS_KEYFILE_STORAGE_GET_PRIVATE (self);

	if (priv->filename)
		(void) unlink (priv->filename);
	return TRUE;
}

/*****************************************************************************/

static void
nms_keyfile_storage_init (NMSKeyfileStorage *self)
{
}

NMSKeyfileStorage *
nms_keyfile_storage_new (const char *filename)
{
	NMSKeyfileStorage *self;
	NMSKeyfileStoragePrivate *priv;

	g_return_val_if_fail (!filename || filename[0] == '/', NULL);

	self = g_object_new (NMS_TYPE_KEYFILE_STORAGE, NULL);

	priv = NMS_KEYFILE_STORAGE_GET_PRIVATE (self);

	priv->filename = g_strdup (filename);
	return self;
}

static void
finalize (GObject *object)
{
	NMSKeyfileStorage *self = NMS_KEYFILE_STORAGE (object);
	NMSKeyfileStoragePrivate *priv = NMS_KEYFILE_STORAGE_GET_PRIVATE (self);

	g_free (priv->filename);

	G_OBJECT_CLASS (nms_keyfile_storage_parent_class)->finalize (object);
}

static void
nms_keyfile_storage_class_init (NMSKeyfileStorageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMSettingsStorageClass *settings_class = NM_SETTINGS_STORAGE_CLASS (klass);

	object_class->finalize = finalize;

	settings_class->load_connection = load_connection;
	settings_class->commit_changes  = commit_changes;
	settings_class->delete          = delete;
}
