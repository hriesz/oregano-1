/*
 * schematic.c
 *
 *
 * Authors:
 *  Richard Hult <rhult@hem.passagen.se>
 *  Ricardo Markiewicz <rmarkie@fi.uba.ar>
 *  Andres de Barbara <adebarbara@fi.uba.ar>
 *
 * Web page: http://arrakis.lug.fi.uba.ar/
 *
 * Copyright (C) 1999-2001  Richard Hult
 * Copyright (C) 2003,2005  LUGFI
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <gtk/gtk.h>
#include <glib.h>
#include "schematic.h"
#include "node-store.h"
//#include "load-schematic.h"
#include "file-manager.h"
#include "settings.h"
#include "sim-settings.h"
#include "simulation.h"
#include "errors.h"

struct _SchematicPriv {
	char *title;
	char *filename;
	char *author;
	char *comments;
	char *netlist_filename;

	/*
	 * Data for various dialogs.
	 */
	gpointer settings;
	gpointer sim_settings;
	gpointer simulation;

	GList *items;

	NodeStore  *store;
	GHashTable *symbols;
	GHashTable *refdes_values;
/*	NetList    *netlist;*/

	double zoom;

	gboolean dirty;

	/* FIXME: Make an object. */
	GList *log;
};

typedef enum {
	REFDATA_SINGLE,
	REFDATA_RANGE,
	REFDATA_MIN,
	REFDATA_MAX
} RefDataType;

typedef struct {
	RefDataType type;
	union {
		int nr;
		struct { int min; int max; } range;
	} u;
} RefData;

enum {
	TITLE_CHANGED,
	ITEM_DATA_ADDED,
	LOG_UPDATED,
	DOT_ADDED,
	DOT_REMOVED,
	LAST_SCHEMATIC_DESTROYED,
	LAST_SIGNAL
};

static void schematic_init(Schematic *schematic);
static void schematic_class_init(SchematicClass	*klass);
static void schematic_finalize(GObject *object);
static void schematic_dispose(GObject *object);
static void item_data_destroy_callback (gpointer s, GObject *data);
static void part_moved_callback (ItemData *data, SheetPos *pos, Schematic *sm);

static int  schematic_get_lowest_available_refdes (Schematic *schematic,
	char *prefix);
static void schematic_set_lowest_available_refdes (Schematic *schematic,
	char *prefix, int num);

static GObjectClass *parent_class = NULL;
static guint schematic_signals[LAST_SIGNAL] = { 0 };

static GList *schematic_list = NULL;
static int schematic_count_ = 0;

GType
schematic_get_type (void)
{
	static GType schematic_type = 0;

	if (!schematic_type) {
		static const GTypeInfo schematic_info = {
			sizeof (SchematicClass),
			NULL,
			NULL,
			(GClassInitFunc) schematic_class_init,
			NULL,
			NULL,
			sizeof (Schematic),
			0,
			(GInstanceInitFunc) schematic_init,
			NULL
		};

		schematic_type =
			g_type_register_static (G_TYPE_OBJECT, "Schematic",
				&schematic_info,0);
	}

	return schematic_type;
}

static void
schematic_class_init (SchematicClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	parent_class = g_type_class_peek_parent(klass);

	schematic_signals[TITLE_CHANGED] = g_signal_new ("title_changed",
		TYPE_SCHEMATIC,
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET(SchematicClass,title_changed),
		NULL, NULL,
		g_cclosure_marshal_VOID__STRING,
		G_TYPE_NONE, 1,
		G_TYPE_STRING );

	schematic_signals[LAST_SCHEMATIC_DESTROYED] = g_signal_new ("last_schematic_destroyed",
		TYPE_SCHEMATIC,
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET(SchematicClass,last_schematic_destroyed),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);

	schematic_signals[ITEM_DATA_ADDED] = g_signal_new ("item_data_added",
		TYPE_SCHEMATIC,
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET(SchematicClass,item_data_added),
	  NULL, NULL,
	  g_cclosure_marshal_VOID__POINTER,
	  G_TYPE_NONE, 1, G_TYPE_POINTER);

	schematic_signals[DOT_ADDED] = g_signal_new ("dot_added",
		TYPE_SCHEMATIC,
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET(SchematicClass,dot_added),
		NULL, NULL,
		g_cclosure_marshal_VOID__POINTER,
		G_TYPE_NONE, 1,
		G_TYPE_POINTER);

	schematic_signals[DOT_REMOVED] = g_signal_new ("dot_removed",
		TYPE_SCHEMATIC,
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET(SchematicClass,dot_removed),
		NULL, NULL, g_cclosure_marshal_VOID__POINTER,
		G_TYPE_NONE, 1, G_TYPE_POINTER);

	schematic_signals[LOG_UPDATED] = g_signal_new ("log_updated",
	  TYPE_SCHEMATIC,
	  G_SIGNAL_RUN_FIRST,
	  G_STRUCT_OFFSET(SchematicClass,log_updated),
	  NULL, NULL, g_cclosure_marshal_VOID__VOID,
	  G_TYPE_NONE,0);

	object_class->finalize = schematic_finalize;
	object_class->dispose = schematic_dispose;
}

static void
dot_added_callback (NodeStore *store, SheetPos *pos, Schematic *schematic)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	g_signal_emit_by_name (G_OBJECT(schematic), "dot_added", pos);
}

static void
dot_removed_callback (NodeStore *store, SheetPos *pos, Schematic *schematic)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	g_signal_emit_by_name (G_OBJECT(schematic), "dot_removed", pos);
}

static void
schematic_init(Schematic *schematic)
{
	SchematicPriv *priv;

	priv = schematic->priv = g_new0 (SchematicPriv, 1);

	priv->symbols = g_hash_table_new (g_str_hash, g_str_equal);
	/* FIXME: use own str_equal (lib::sym)*/
	priv->refdes_values = g_hash_table_new (g_str_hash, g_str_equal);
	priv->store = node_store_new ();
	priv->dirty = FALSE;

	g_signal_connect_object (
		G_OBJECT (priv->store),
		"dot_added",
		G_CALLBACK(dot_added_callback),
		G_OBJECT (schematic),
		G_CONNECT_AFTER);

	g_signal_connect_object (
		G_OBJECT (priv->store),
		"dot_removed",
		G_CALLBACK(dot_removed_callback),
		G_OBJECT (schematic),
		G_CONNECT_AFTER);

	priv->sim_settings = sim_settings_new (schematic);
	priv->settings = settings_new (schematic);
	priv->simulation = simulation_new (schematic);

	priv->filename = NULL;
	priv->netlist_filename = NULL;
	priv->author = g_strdup ("");
	priv->comments = g_strdup ("");
}

Schematic *
schematic_new (void)
{
	Schematic *schematic;

	schematic = SCHEMATIC(g_object_new (TYPE_SCHEMATIC, NULL));

	schematic_count_++;
	schematic_list = g_list_prepend (schematic_list, schematic);

	return schematic;
}

static void
schematic_dispose (GObject *object)
{
	Schematic *schematic;
	GList *list;

	schematic = SCHEMATIC(object);

	/* Disconnect weak item signal */
	for(list=schematic->priv->items; list; list=list->next)
		g_object_weak_unref(G_OBJECT(list->data),
			item_data_destroy_callback, G_OBJECT(schematic));

	schematic_count_--;
	schematic_list = g_list_remove (schematic_list, schematic);

	if (schematic_count_ == 0) {
		g_signal_emit_by_name(G_OBJECT (schematic),
			"last_schematic_destroyed", NULL);
	}

	G_OBJECT_CLASS(parent_class)->dispose(G_OBJECT(schematic));
}

static void
schematic_finalize(GObject *object)
{
	Schematic *sm;

	sm = SCHEMATIC(object);
	if (sm->priv) {
		g_free(sm->priv->simulation);
		g_hash_table_destroy(sm->priv->symbols);
		g_hash_table_destroy(sm->priv->refdes_values);

		g_free(sm->priv);
		sm->priv = NULL;
	}

	G_OBJECT_CLASS(parent_class)->finalize(G_OBJECT(sm));
}

/*
 *
 * Get/set functions.
 *
 */

char *
schematic_get_title (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->title;
}

char *
schematic_get_author (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->author;
}

char *
schematic_get_comments (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->comments;
}

void
schematic_set_title (Schematic *schematic, const gchar *title)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (!title) return;

	if (schematic->priv->title)
		g_free (schematic->priv->title);
	schematic->priv->title = g_strdup (title);

	g_signal_emit_by_name (G_OBJECT (schematic),
		"title_changed", schematic->priv->title);
}

void
schematic_set_author (Schematic *schematic, const gchar *author)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (!author) return;

	if (schematic->priv->author)
		g_free (schematic->priv->author);
	schematic->priv->author = g_strdup (author);
}

void
schematic_set_comments (Schematic *schematic, const gchar *comments)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (schematic->priv->comments)
		g_free (schematic->priv->comments);
	schematic->priv->comments = g_strdup (comments);
}

char *
schematic_get_filename (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->filename;
}

void
schematic_set_filename (Schematic *schematic, const gchar *filename)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	g_free (schematic->priv->filename);
	schematic->priv->filename = g_strdup (filename);

/*	g_signal_emit_by_name (G_OBJECT (schematic),
	                       "filename_changed", schematic->priv->filename); */
}

char *
schematic_get_netlist_filename (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->netlist_filename;
}

void
schematic_set_netlist_filename (Schematic *schematic, char *filename)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (schematic->priv->netlist_filename)
		g_free (schematic->priv->netlist_filename);

	schematic->priv->netlist_filename = g_strdup (filename);

/*	g_signal_emit_by_name (G_OBJECT (schematic),
			"netlist_filename_changed", schematic->priv->filename);
*/
}

double
schematic_get_zoom (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, 1.0);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), 1.0);

	return schematic->priv->zoom;
}

/*
 * FIXME: different zoom level on views...
 */
void
schematic_set_zoom (Schematic *schematic, double zoom)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	schematic->priv->zoom = zoom;

/*  g_signal_emit_by_name (G_OBJECT (schematic),
	                       "zoom_changed", schematic->priv->zoom); */
}

NodeStore *
schematic_get_store (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->store;
}

gpointer
schematic_get_settings (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->settings;
}

gpointer
schematic_get_sim_settings (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->sim_settings;
}

gpointer
schematic_get_simulation (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->simulation;
}

/**
 * Actually prepends but that doesn't matter here.
 * This is an internal storage only.
 */
void
schematic_log_append (Schematic *schematic, const char *message)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	schematic->priv->log = g_list_prepend (schematic->priv->log,
		g_strdup (message));
}

void
schematic_log_show (Schematic *schematic)
{
	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	g_signal_emit_by_name (G_OBJECT(schematic),
		"log_updated", schematic->priv->log);
}

void
schematic_log_clear (Schematic *schematic)
{
	GList *log;

	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	for (log = schematic->priv->log; log; log = log->next)
		g_free (log->data);

	g_list_free (schematic->priv->log);
	schematic->priv->log = NULL;
}

GList *
schematic_get_log_text (Schematic *schematic)
{
	g_return_val_if_fail (schematic != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), NULL);

	return schematic->priv->log;
}

int
schematic_count (void)
{
	return schematic_count_;
}

Schematic *
schematic_read (char *name, GError **out_error)
{
	Schematic *new_sm;
	int ret;
	char *fname;
	GError *error = NULL;
	FileType *ft;

	g_return_val_if_fail (name != NULL, NULL);

	fname = g_filename_from_uri  (name, NULL, &error);

	if (!fname) {
		fname = name;
		if (error) {
			g_error_free (error);
			error = NULL;
		}
	}

	if (!g_file_test (fname, G_FILE_TEST_EXISTS)) {
		g_set_error (out_error, OREGANO_ERROR, OREGANO_SCHEMATIC_FILE_NOT_FOUND,
			_("File (%s) does not exists."), fname);
		return NULL;
	}

	/* Get File Handler */
	ft = file_manager_get_handler (fname);
	if (ft == NULL) {
		g_set_error (out_error, OREGANO_ERROR, OREGANO_SCHEMATIC_FILE_NOT_FOUND,
			_("Unknown file format for (%s)."), fname);
		return NULL;
	}

	new_sm = schematic_new ();

	/* TODO : Add GError-like error reporting! */
	ret = ft->load_func (new_sm, fname, &error);

	if (error != NULL) {
		g_propagate_error (out_error, error);
		g_object_unref(G_OBJECT(new_sm));
		return;
	}

	if (ret) {
		g_object_unref(G_OBJECT(new_sm));
		new_sm = NULL;
		g_set_error (out_error, OREGANO_ERROR, OREGANO_SCHEMATIC_FILE_NOT_FOUND,
			_("Load fails!."), fname);
	} else
		schematic_set_dirty (new_sm, FALSE);

	return new_sm;
}

gint
schematic_save_file (Schematic *sm, GError **error)
{
	FileType *ft;
	GError *internal_error = NULL;

	g_return_val_if_fail (sm != NULL, FALSE);

	ft = file_manager_get_handler (schematic_get_filename (sm));

	if (ft == NULL) {
		g_set_error (error, OREGANO_ERROR, OREGANO_SCHEMATIC_FILE_NOT_FOUND,
			_("Unknown file format for (%s)."), schematic_get_filename (sm));
		return;
	}

	if (ft->save_func (sm, &internal_error)) {
		schematic_set_dirty (sm, FALSE);
		return TRUE;
	}

	g_propagate_error (error, internal_error);

	g_error_free (internal_error);
	return FALSE; // Save fails!
}

void
schematic_add_item (Schematic *sm, ItemData *data)
{
	NodeStore *store;
	char *prefix, *refdes;
	int num;

	g_return_if_fail (sm != NULL);
	g_return_if_fail (IS_SCHEMATIC (sm));
	g_return_if_fail (data != NULL);
	g_return_if_fail (IS_ITEM_DATA (data));

	store = sm->priv->store;
	g_object_set(G_OBJECT(data), "store", store, NULL);
	if (item_data_register(data) == -1) {
		/* Item does not be added */
		g_object_unref (G_OBJECT (data));
		return;
	}

	/*
	 * Some items need a reference designator. Find a good one.
	 */
	prefix = item_data_get_refdes_prefix (data);
	if (prefix != NULL) {
		num = schematic_get_lowest_available_refdes (sm, prefix);
		refdes = g_strdup_printf ("%s%d", prefix, num);
		item_data_set_property (data, "refdes", refdes);

		schematic_set_lowest_available_refdes (sm, prefix, num + 1);

		g_free (prefix);
		g_free (refdes);
	}

	sm->priv->items = g_list_prepend(sm->priv->items, data);
	g_object_weak_ref(G_OBJECT(data), item_data_destroy_callback, G_OBJECT(sm));

	g_signal_connect_object(G_OBJECT(data),
		"moved",
		G_CALLBACK(part_moved_callback),
		sm,
		0);

	sm->priv->dirty = TRUE;

	g_signal_emit_by_name(G_OBJECT(sm), "item_data_added", data);
}

void
schematic_parts_foreach (Schematic *schematic,
	ForeachItemDataFunc func, gpointer user_data)
{
	GList *list;

	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (func == NULL)
		return;

	for (list = node_store_get_parts (schematic->priv->store); list; list = list->next) {
		func (list->data, user_data);
	}
}

void
schematic_wires_foreach (Schematic *schematic,
	ForeachItemDataFunc func, gpointer user_data)
{
	GList *list;

	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (func == NULL)
		return;

	for (list = node_store_get_wires (schematic->priv->store); list; list = list->next) {
		func (list->data, user_data);
	}
}

void
schematic_items_foreach (Schematic *schematic,
	ForeachItemDataFunc func, gpointer user_data)
{
	GList *list;

	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));

	if (func == NULL)
		return;

	for (list = schematic->priv->items; list; list = list->next) {
		func (list->data, user_data);
	}
}

GList *
schematic_get_items (Schematic *sm)
{
	g_return_val_if_fail (sm != NULL, NULL);
	g_return_val_if_fail (IS_SCHEMATIC (sm), NULL);

	return sm->priv->items;
}

static void
item_data_destroy_callback (gpointer s, GObject *data)
{
	Schematic *sm = SCHEMATIC(s);
	schematic_set_dirty (sm, TRUE);
	if (sm->priv) {
		sm->priv->items = g_list_remove (sm->priv->items, data);
	}
}

static int
schematic_get_lowest_available_refdes (Schematic *schematic, char *prefix)
{
	gpointer key, value;

	g_return_val_if_fail (schematic != NULL, -1);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), -1);
	g_return_val_if_fail (prefix != NULL, -1);

	if ( g_hash_table_lookup_extended (schematic->priv->refdes_values,
		     prefix, &key, &value) ) {
		return GPOINTER_TO_INT (value);
	} else {
		return 1;
	}
}

static void
schematic_set_lowest_available_refdes (Schematic *schematic,
	char *prefix, int num)
{
	gpointer key, value;

	g_return_if_fail (schematic != NULL);
	g_return_if_fail (IS_SCHEMATIC (schematic));
	g_return_if_fail (prefix != NULL);

	/*
	 * If there already is a key, use it, otherwise copy the prefix and
	 * use as key.
	 */
	if (!g_hash_table_lookup_extended (schematic->priv->refdes_values,
		prefix, &key, &value))
		key = g_strdup (prefix);

	g_hash_table_insert (schematic->priv->refdes_values,
						 key, GINT_TO_POINTER (num));
}

gboolean
schematic_is_dirty(Schematic *sm)
{
	g_return_val_if_fail (sm != NULL, FALSE);
	g_return_val_if_fail (IS_SCHEMATIC (sm), FALSE);

	return sm->priv->dirty;
}

void
schematic_set_dirty(Schematic *sm, gboolean b)
{
	g_return_if_fail (sm != NULL);
	g_return_if_fail (IS_SCHEMATIC (sm));

	sm->priv->dirty = b;
}

static void
part_moved_callback (ItemData *data, SheetPos *pos, Schematic *sm)
{
	g_return_if_fail (data != NULL);
	g_return_if_fail (IS_ITEM_DATA (data));

	schematic_set_dirty (sm, TRUE);
}

/*
static int
schematic_get_lowest_available_refdes2 (Schematic *schematic, char *prefix)
{
	RefData *data;

	g_return_val_if_fail (schematic != NULL, -1);
	g_return_val_if_fail (IS_SCHEMATIC (schematic), -1);
	g_return_val_if_fail (prefix != NULL, -1);

	data = g_hash_table_lookup(schematic->priv->refdes_values, prefix);
	if (data == NULL) {
		return 1;
	} else {
		int result;

		switch (data->type) {
		REF_DATA_SINGLE:
			break;
		REF_DATA_RANGE:
			break;
		REF_DATA_MIN:
			break;
		REF_DATA_MAX:
			break;
		default:
			result = 1;
		}
		return result;
	}

}
*/

