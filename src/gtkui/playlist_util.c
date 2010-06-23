/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005-2010  Audacious development team
 *  Copyright (C) 2010 Michał Lipski <tallica@o2.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 *  The Audacious team does not consider modular code linking to
 *  Audacious or using our public API to be a derived work.
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <audacious/plugin.h>
#include <math.h>

#include "playlist_util.h"
#include "ui_playlist_model.h"
#include "ui_playlist_notebook.h"

typedef struct
{
    GtkTreeView *treeview;
    GtkTreePath *start_path;
    GtkTreePath *end_path;
} UiPlaylistSelection;

static UiPlaylistSelection *pending;

GtkTreeView *playlist_get_treeview_from_page(GtkWidget *page)
{
    if (!page)
        return NULL;

    return GTK_TREE_VIEW(g_object_get_data(G_OBJECT(page), "treeview"));
}

GtkTreeView *playlist_get_treeview(gint playlist)
{
    GtkWidget *page = gtk_notebook_get_nth_page(UI_PLAYLIST_NOTEBOOK, playlist);

    if (!page)
        return NULL;

    return GTK_TREE_VIEW(g_object_get_data(G_OBJECT(page), "treeview"));
}

GtkTreeView *playlist_get_active_treeview(void)
{
    return playlist_get_treeview(aud_playlist_get_active());
}

GtkTreeView *playlist_get_playing_treeview(void)
{
    return playlist_get_treeview(aud_playlist_get_playing());
}

gint playlist_get_playlist_from_treeview(GtkTreeView *treeview)
{
    g_return_val_if_fail(treeview != NULL, -1);

    GtkTreeModel *tree_model = gtk_tree_view_get_model(treeview);
    UiPlaylistModel *model = UI_PLAYLIST_MODEL(tree_model);

    return model->playlist;
}

static inline void _gtk_tree_selection_select_path(GtkTreePath *path, GtkTreeSelection *sel)
{
    gtk_tree_selection_select_path(sel, path);
}

void playlist_set_selected(GtkTreeView *treeview, GtkTreePath *path)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);

    gtk_tree_selection_unselect_all(sel);
    gtk_tree_selection_select_path(sel, path);
    gtk_tree_view_set_cursor(treeview, path, NULL, FALSE);
}

void playlist_set_selected_list(GtkTreeView *treeview, GList *list, gint distance)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);
    GtkTreePath *path;
    gint pos;

    gtk_tree_selection_unselect_all(sel);

    if (distance == 0)
    {
        gtk_tree_view_set_cursor(treeview, g_list_first(list)->data, NULL, FALSE);
        g_list_foreach(list, (GFunc) _gtk_tree_selection_select_path, sel);
        return;
    }

    for (GList *target = g_list_first(list); target; target = target->next)
    {
        if (!target->data)
            continue;

        pos = playlist_get_index_from_path(target->data) + distance;
        path = gtk_tree_path_new_from_indices(pos, -1);

        if (path)
        {
            gtk_tree_selection_select_path(sel, path);

            if (target->prev == NULL)
                gtk_tree_view_set_cursor(treeview, path, NULL, FALSE);

            gtk_tree_path_free(path);
        }
    }
}

void playlist_scroll_to_row(GtkTreeView *treeview, gint position)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(position, -1);

    g_return_if_fail(treeview != NULL);
    g_return_if_fail(path != NULL);

    gtk_tree_view_scroll_to_cell(treeview, path, NULL, FALSE, 0, 0);
    gtk_tree_view_set_cursor(treeview, path, NULL, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(treeview));
    gtk_tree_path_free(path);
}

GList *playlist_get_selected_list(GtkTreeView *treeview)
{
    GtkTreeModel *treemodel;
    GtkTreeSelection *sel;

    g_return_val_if_fail(treeview != NULL, NULL);

    treemodel = gtk_tree_view_get_model(treeview);

    sel = gtk_tree_view_get_selection(treeview);
    g_return_val_if_fail(sel != NULL, NULL);

    return gtk_tree_selection_get_selected_rows(sel, &treemodel);
}

gint playlist_get_selected_length(GtkTreeView *treeview)
{
    GList *list = playlist_get_selected_list(treeview);
    gint selected_length;

    selected_length = g_list_length(list);

    g_list_foreach(list, (GFunc) gtk_tree_path_free, NULL);
    g_list_free(list);

    return selected_length;
}

GtkTreePath *playlist_get_first_selected_path(GtkTreeView *treeview)
{
    GList *list;
    GtkTreePath *path;

    if (! (list = playlist_get_selected_list (treeview)))
        return NULL;

    path = gtk_tree_path_copy(list->data);

    g_list_foreach(list, (GFunc) gtk_tree_path_free, NULL);
    g_list_free(list);

    return path;
}

gint playlist_get_first_selected_index(GtkTreeView *treeview)
{
    GtkTreePath *path = playlist_get_first_selected_path(treeview);

    if (! path)
        return -1;

    gint selected = playlist_get_index_from_path(path);
    gtk_tree_path_free(path);

    return selected;
}

gint playlist_get_index_from_path(GtkTreePath * path)
{
    gint *pos;

    g_return_val_if_fail(path != NULL, -1);

    if (!(pos = gtk_tree_path_get_indices(path)))
        return -1;

    return pos[0];
}

gint calculate_column_width(GtkWidget *widget, gint num)
{
    PangoFontDescription *font_desc;
    PangoContext *context;
    PangoFontMetrics *font_metrics;
    gint digits = 1 + log10(num);

    font_desc = widget->style->font_desc;
    context = gtk_widget_get_pango_context(widget);
    font_metrics = pango_context_get_metrics(context, font_desc,
                                             pango_context_get_language(context));

    return (PANGO_PIXELS(pango_font_metrics_get_approximate_digit_width(font_metrics)) * digits) + 20;
}

gboolean playlist_is_pending_selection(void)
{
    if (pending != NULL)
        return TRUE;
    else
        return FALSE;
}

void playlist_select_range(GtkTreeView *treeview, GtkTreePath *start_path, GtkTreePath *end_path)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);

    gtk_tree_selection_unselect_all(sel);
    gtk_tree_view_set_cursor(treeview, start_path, NULL, FALSE);
    gtk_tree_selection_select_range(sel, start_path, end_path);
}

void playlist_pending_selection_set(GtkTreeView *treeview, GtkTreePath *start_path, GtkTreePath *end_path)
{
    g_return_if_fail(treeview != NULL);
    g_return_if_fail(start_path != NULL);
    g_return_if_fail(end_path != NULL);

    pending = g_slice_new0(UiPlaylistSelection);
    pending->treeview = treeview;
    pending->start_path = start_path;
    pending->end_path = end_path;
}

void playlist_pending_selection_free(void)
{
    gtk_tree_path_free(pending->start_path);
    gtk_tree_path_free(pending->end_path);
    g_slice_free(UiPlaylistSelection, pending);
    pending = NULL;
}

void playlist_pending_selection_apply(void)
{
    g_return_if_fail(pending != NULL);
    g_return_if_fail(pending->start_path != NULL);
    g_return_if_fail(pending->end_path != NULL);

    playlist_select_range(pending->treeview, pending->start_path, pending->end_path);

    playlist_pending_selection_free();
}

void playlist_block_selection(GtkTreeView *treeview)
{
    g_return_if_fail(treeview != NULL);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);
    gulong handler_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(treeview), "selection_changed_handler_id"));
    g_signal_handler_block(G_OBJECT(sel), handler_id);
}

void playlist_unblock_selection(GtkTreeView *treeview)
{
    g_return_if_fail(treeview != NULL);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(treeview);
    gulong handler_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(treeview), "selection_changed_handler_id"));
    g_signal_handler_unblock(G_OBJECT(sel), handler_id);
}

gint playlist_count_selected_in_range (gint list, gint top, gint length)
{
    gint selected = 0;
    gint count;

    for (count = 0; count < length; count ++)
    {
        if (aud_playlist_entry_get_selected (list, top + count))
            selected ++;
    }

    return selected;
}

void treeview_set_focus (GtkTreeView * tree, gint focus)
{
    GtkTreePath * path = gtk_tree_path_new_from_indices (focus, -1);
    gtk_tree_view_set_cursor ((GtkTreeView *) tree, path, NULL, FALSE);
    gtk_tree_path_free (path);
}

gint treeview_get_focus (GtkTreeView * tree)
{
    GtkTreePath * path;
    gint focus;

    gtk_tree_view_get_cursor (tree, & path, NULL);
    if (! path)
        return -1;

    focus = gtk_tree_path_get_indices (path)[0];
    gtk_tree_path_free (path);
    return focus;
}

void treeview_clear_selection (GtkTreeView * tree)
{
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree));
}

void treeview_set_selection_from_playlist (GtkTreeView * tree, gint list)
{
    GtkTreeModel * model = gtk_tree_view_get_model (tree);
    GtkTreeSelection * sel = gtk_tree_view_get_selection (tree);
    gint entries = aud_playlist_entry_count (list);
    GtkTreeIter iter;
    gint count;

    if (! entries)
        return;

    gtk_tree_model_get_iter_first (model, & iter);

    for (count = 0; count < entries; count ++)
    {
        if (aud_playlist_entry_get_selected (list, count))
            gtk_tree_selection_select_iter (sel, & iter);
        else
            gtk_tree_selection_unselect_iter (sel, & iter);

        gtk_tree_model_iter_next (model, & iter);
    }
}
