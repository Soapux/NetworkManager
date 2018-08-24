/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2018 Red Hat, Inc.
 */

#ifndef __NM_SETTINGS_STORAGE_H__
#define __NM_SETTINGS_STORAGE_H__

typedef enum {
	NM_SETTINGS_STORAGE_COMMIT_REASON_NONE        = 0,
	NM_SETTINGS_STORAGE_COMMIT_REASON_USER_ACTION = (1LL << 0),
	NM_SETTINGS_STORAGE_COMMIT_REASON_ID_CHANGED  = (1LL << 1),
} NMSettingsStorageCommitReason;

#define NM_TYPE_SETTINGS_STORAGE            (nm_settings_storage_get_type ())
#define NM_SETTINGS_STORAGE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), NM_TYPE_SETTINGS_STORAGE, NMSettingsStorage))
#define NM_SETTINGS_STORAGE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), NM_TYPE_SETTINGS_STORAGE, NMSettingsStorageClass))
#define NM_IS_SETTINGS_STORAGE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NM_TYPE_SETTINGS_STORAGE))
#define NM_IS_SETTINGS_STORAGE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), NM_TYPE_SETTINGS_STORAGE))
#define NM_SETTINGS_STORAGE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), NM_TYPE_SETTINGS_STORAGE, NMSettingsStorageClass))

#define NM_SETTINGS_STORAGE_PLUGIN "pugin"

struct _NMSettingsStoragePrivate;

typedef struct {
	GObject parent;
	struct _NMSettingsStoragePrivate *_priv;
} NMSettingsStorage;

typedef struct {
	GObjectClass parent;

	NMConnection *(*load_connection) (NMSettingsStorage *storage,
	                                  GError **error);

	gboolean (*commit_changes) (NMSettingsStorage *self,
	                            NMConnection *connection,
	                            NMSettingsStorageCommitReason commit_reason,
	                            NMConnection **out_reread_connection,
	                            char **out_logmsg_change,
	                            GError **error);

	gboolean (*delete) (NMSettingsStorage *self,
	                    GError **error);

	gboolean (*supports_secrets) (NMSettingsStorage *self,
	                              const char *setting_name);
} NMSettingsStorageClass;

GType nm_settings_storage_get_type (void);

struct _NMSettingsPlugin;
struct _NMSettingsPlugin *nm_settings_storage_get_plugin (NMSettingsStorage *self);

#endif /* __NM_SETTINGS_STORAGE_H__ */
