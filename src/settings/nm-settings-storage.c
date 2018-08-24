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

#include "nm-default.h"

#include "nm-settings-storage.h"

#include "nm-settings-plugin.h"

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE_BASE (
	PROP_PLUGIN,
);

typedef struct _NMSettingsStoragePrivate {
	NMSettingsPlugin *plugin;
} NMSettingsStoragePrivate;

G_DEFINE_ABSTRACT_TYPE (NMSettingsStorage, nm_settings_storage, G_TYPE_OBJECT)

#define NM_SETTINGS_STORAGE_GET_PRIVATE(self) _NM_GET_PRIVATE_PTR(self, NMSettingsStorage, NM_IS_SETTINGS_STORAGE)

/*****************************************************************************/

NMSettingsPlugin *
nm_settings_storage_get_plugin (NMSettingsStorage *self)
{
	return NM_SETTINGS_STORAGE_GET_PRIVATE (self)->plugin;
}

/*****************************************************************************/

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMSettingsStorage *self = NM_SETTINGS_STORAGE (object);
	NMSettingsStoragePrivate *priv = NM_SETTINGS_STORAGE_GET_PRIVATE (self);

	switch (prop_id) {
	case PROP_PLUGIN:
		/* construct-only */
		priv->plugin = g_object_ref (g_value_get_object (value));
		nm_assert (NM_IS_SETTINGS_PLUGIN (priv->plugin));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*****************************************************************************/

static void
nm_settings_storage_init (NMSettingsStorage *self)
{
	NMSettingsStoragePrivate *priv;

	priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NM_TYPE_SETTINGS_STORAGE, NMSettingsStoragePrivate);

	self->_priv = priv;
}

static void
finalize (GObject *object)
{
	NMSettingsStoragePrivate *priv = NM_SETTINGS_STORAGE_GET_PRIVATE (object);

	g_object_unref (priv->plugin);

	G_OBJECT_CLASS (nm_settings_storage_parent_class)->finalize (object);
}

static void
nm_settings_storage_class_init (NMSettingsStorageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMSettingsStoragePrivate));

	object_class->set_property = set_property;
	object_class->finalize = finalize;

	obj_properties[PROP_PLUGIN] =
	    g_param_spec_object (NM_SETTINGS_STORAGE_PLUGIN, "", "",
	                         NM_TYPE_SETTINGS_PLUGIN,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties (object_class, _PROPERTY_ENUMS_LAST, obj_properties);
}
