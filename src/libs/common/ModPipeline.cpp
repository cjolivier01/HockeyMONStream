#include "hstream/src/libs/common/ModPipeline.h"

#include <glib-object.h>
#include <gst/gst.h>
#include <gst/gstbin.h>
#include <gst/gstiterator.h>
#include <gstreamer-1.0/gst/gstobject.h>
#include <gtk/gtk.h>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

namespace hm {
namespace {

// Columns for the element list store.
enum {
  COL_ELEMENT_PTR, // Pointer to GstElement (stored as gpointer)
  COL_ELEMENT_NAME, // Element name string
  NUM_ELEM_COLS
};

// Columns for the property list store.
enum {
  COL_PROP_NAME, // Property name (string)
  COL_PROP_VALUE, // Current value as string
  COL_PROP_TYPE, // Property type (stored as gpointer, actually a GType)
  COL_PROP_SPEC, // Pointer to the GParamSpec
  NUM_PROP_COLS
};

static GtkListStore* element_store = nullptr;
static GtkListStore* property_store = nullptr;
static GtkWidget* element_tree_view = nullptr;
static GtkWidget* property_tree_view = nullptr;
// static GstElement* g_pipeline = nullptr;

// Recursively traverse the element tree, accumulating a map from
// a dot-separated name (e.g. "my_pipeline.mybin.myelement") to the GstElement pointer.
std::map<std::string, GstElement*> get_element_tree(GstElement* element, const std::string& prefix = "") {
  std::map<std::string, GstElement*> result;
  if (!element)
    return result;

  // Get the element's name and build the fully qualified name.
  const gchar* elem_name = gst_element_get_name(element);
  std::string full_name = prefix.empty() ? (elem_name ? elem_name : "") : prefix + "." + (elem_name ? elem_name : "");

  // Add this element to the result map.
  result[full_name] = element;

  // If the element is a bin, iterate its children.
  if (GST_IS_BIN(element)) {
    GstIterator* it = gst_bin_iterate_elements(GST_BIN(element));
    if (it) {
      GValue item = G_VALUE_INIT;
      g_value_init(&item, G_TYPE_OBJECT);

      while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        GstElement* child = GST_ELEMENT(g_value_get_object(&item));
        if (child) {
          // Recursively get the child tree and merge it into our map.
          std::map<std::string, GstElement*> child_map = get_element_tree(child, full_name);
          result.insert(child_map.begin(), child_map.end());
        }
        g_value_reset(&item);
      }
      gst_iterator_free(it);
    }
  }
  return result;
}

// GList* my_gst_bin_get_children(GstBin* bin) {
//   GList* children = nullptr;

//   // Get an iterator for all elements in the bin.
//   GstIterator* it = gst_bin_iterate_elements(bin);
//   if (!it) {
//     return nullptr;
//   }

//   // Prepare a GValue to hold each element.
//   GValue item = G_VALUE_INIT;
//   g_value_init(&item, G_TYPE_OBJECT);

//   // Iterate through the elements.
//   while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
//     // Retrieve the element from the GValue.
//     GstElement* element = GST_ELEMENT(g_value_get_object(&item));
//     if (element) {
//       // Prepend the element to our list.
//       children = g_list_prepend(children, element);
//     }
//     g_value_reset(&item);
//   }
//   gst_iterator_free(it);

//   // Reverse the list so that the order is the same as the iterator order.
//   children = g_list_reverse(children);
//   return children;
// }

/* Helper function: Convert a GValue to a std::string using GST’s serializer */
std::string value_to_string(const GValue* value) {
  gchar* str = gst_value_serialize(value);
  std::string result = str ? str : "";
  g_free(str);
  return result;
}

/* Populate the property list for the given element.
   We only show writable properties (G_PARAM_WRITABLE) for simplicity. */
void populate_property_list(GstElement* element) {
  // Clear any previous property list.
  gtk_list_store_clear(property_store);

  GObjectClass* klass = G_OBJECT_GET_CLASS(element);
  guint n_properties = 0;
  GParamSpec** props = g_object_class_list_properties(klass, &n_properties);

  for (guint i = 0; i < n_properties; i++) {
    GParamSpec* pspec = props[i];
    // Only list writable properties.
    if (!(pspec->flags & G_PARAM_WRITABLE))
      continue;

    // Retrieve the current property value.
    GValue value = G_VALUE_INIT;
    g_value_init(&value, pspec->value_type);
    g_object_get_property(G_OBJECT(element), pspec->name, &value);
    std::string valueStr = value_to_string(&value);
    g_value_unset(&value);

    // Append a new row to the property list store.
    GtkTreeIter iter;
    gtk_list_store_append(property_store, &iter);
    gtk_list_store_set(
        property_store,
        &iter,
        COL_PROP_NAME,
        pspec->name,
        COL_PROP_VALUE,
        valueStr.c_str(),
        COL_PROP_TYPE,
        (gpointer)pspec->value_type,
        COL_PROP_SPEC,
        pspec,
        -1);
  }
  g_free(props);
}

/* Callback: When an element is selected from the left pane,
   list its properties on the right. */
static void on_element_selection_changed(GtkTreeSelection* selection, gpointer user_data) {
  GtkTreeModel* model;
  GtkTreeIter iter;
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    gpointer ptr = nullptr;
    gtk_tree_model_get(model, &iter, COL_ELEMENT_PTR, &ptr, -1);
    GstElement* element = static_cast<GstElement*>(ptr);
    if (element)
      populate_property_list(element);
  }
}

/* Callback: When a property row is double-clicked, open a dialog to edit it. */
static void on_property_row_activated(
    GtkTreeView* tree_view,
    GtkTreePath* path,
    GtkTreeViewColumn* column,
    gpointer user_data) {
  GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(model, &iter, path))
    return;

  gchar* prop_name = nullptr;
  gchar* prop_value = nullptr;
  gpointer prop_type_ptr = nullptr;
  GParamSpec* pspec = nullptr;
  gtk_tree_model_get(
      model,
      &iter,
      COL_PROP_NAME,
      &prop_name,
      COL_PROP_VALUE,
      &prop_value,
      COL_PROP_TYPE,
      &prop_type_ptr,
      COL_PROP_SPEC,
      &pspec,
      -1);
  if (!prop_name)
    return;

  // Create a dialog with a text entry prefilled with the current property value.
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Edit Property", nullptr, GTK_DIALOG_MODAL, "_OK", GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, nullptr);
  GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), prop_value ? prop_value : "");
  gtk_box_pack_start(GTK_BOX(content_area), entry, TRUE, TRUE, 5);
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
    const gchar* new_val_str = gtk_entry_get_text(GTK_ENTRY(entry));

    // Get the currently selected element from the element list.
    GtkTreeSelection* elem_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(element_tree_view));
    GtkTreeIter elem_iter;
    GstElement* element = nullptr;
    if (gtk_tree_selection_get_selected(elem_selection, nullptr, &elem_iter)) {
      gtk_tree_model_get(
          gtk_tree_view_get_model(GTK_TREE_VIEW(element_tree_view)), &elem_iter, COL_ELEMENT_PTR, &element, -1);
    }
    if (element && pspec) {
      // Create a GValue of the appropriate type.
      GValue value = G_VALUE_INIT;
      g_value_init(&value, (GType)prop_type_ptr);

      // Perform a simple conversion based on the property type.
      if (G_TYPE_CHECK_VALUE_TYPE(&value, G_TYPE_INT)) {
        int intval = std::stoi(new_val_str);
        g_value_set_int(&value, intval);
      } else if (G_TYPE_CHECK_VALUE_TYPE(&value, G_TYPE_FLOAT)) {
        float fval = std::stof(new_val_str);
        g_value_set_float(&value, fval);
      } else if (G_TYPE_CHECK_VALUE_TYPE(&value, G_TYPE_BOOLEAN)) {
        gboolean bval = (g_strcmp0(new_val_str, "true") == 0 || g_strcmp0(new_val_str, "1") == 0);
        g_value_set_boolean(&value, bval);
      } else if (G_TYPE_CHECK_VALUE_TYPE(&value, G_TYPE_STRING)) {
        g_value_set_string(&value, new_val_str);
      } else {
        // Fallback: treat it as a string.
        g_value_set_string(&value, new_val_str);
      }

      // Set the property on the element.
      g_object_set_property(G_OBJECT(element), prop_name, &value);
      g_value_unset(&value);

      // Refresh the property list to show the updated value.
      populate_property_list(element);
    }
  }

  gtk_widget_destroy(dialog);
  g_free(prop_name);
  if (prop_value)
    g_free(prop_value);
}
} // namespace

std::unique_ptr<std::thread> edit_pipeline(GstObject* pipeline) {
  auto gui_thread = std::make_unique<std::thread>([g_pipeline = pipeline]() {
    int argc = 1;
    char *argv[] = {(char *)"program", nullptr};
    gtk_init(&argc, (char ***)&argv);
    // Create the main window.
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GStreamer Pipeline Editor");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    // Create a horizontal pane.
    GtkWidget* hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(window), hpaned);

    // -------------------------
    // Left pane: Element list
    // -------------------------
    element_store = gtk_list_store_new(NUM_ELEM_COLS, G_TYPE_POINTER, G_TYPE_STRING);
    element_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(element_store));
    GtkCellRenderer* elem_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* elem_column =
        gtk_tree_view_column_new_with_attributes("Elements", elem_renderer, "text", COL_ELEMENT_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(element_tree_view), elem_column);

    // Populate the element list from the pipeline.

    std::map<std::string, GstElement*> element_tree = get_element_tree(GST_ELEMENT(g_pipeline));
    for (auto& item : element_tree) {
      GtkTreeIter iter;
      gtk_list_store_append(element_store, &iter);
      gtk_list_store_set(element_store, &iter, COL_ELEMENT_PTR, item.second, COL_ELEMENT_NAME, item.first.c_str(), -1);
    }

    // GList* children = my_gst_bin_get_children(GST_BIN(g_pipeline));
    // for (GList* l = children; l != nullptr; l = l->next) {
    //   GstElement* element = GST_ELEMENT(l->data);
    //   const gchar* name = gst_element_get_name(element);
    //   GtkTreeIter iter;
    //   gtk_list_store_append(element_store, &iter);
    //   gtk_list_store_set(element_store, &iter, COL_ELEMENT_PTR, element, COL_ELEMENT_NAME, name, -1);
    // }
    // g_list_free(children);

    // Connect selection changes to update the property list.
    GtkTreeSelection* elem_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(element_tree_view));
    g_signal_connect(elem_selection, "changed", G_CALLBACK(on_element_selection_changed), nullptr);

    // Put the element list in a scrolled window.
    GtkWidget* elem_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(elem_scrolled), element_tree_view);
    gtk_widget_set_vexpand(elem_scrolled, TRUE);
    gtk_widget_set_hexpand(elem_scrolled, TRUE);
    gtk_paned_pack1(GTK_PANED(hpaned), elem_scrolled, TRUE, FALSE);

    // -------------------------
    // Right pane: Property list
    // -------------------------
    property_store = gtk_list_store_new(NUM_PROP_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER);
    property_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(property_store));

    // Add two columns: one for property names, one for their values.
    GtkCellRenderer* prop_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* prop_column =
        gtk_tree_view_column_new_with_attributes("Property", prop_renderer, "text", COL_PROP_NAME, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(property_tree_view), prop_column);
    GtkCellRenderer* val_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* val_column =
        gtk_tree_view_column_new_with_attributes("Value", val_renderer, "text", COL_PROP_VALUE, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(property_tree_view), val_column);

    // When a property row is activated (double-clicked), open the editor dialog.
    g_signal_connect(property_tree_view, "row-activated", G_CALLBACK(on_property_row_activated), nullptr);

    // Put the property list in a scrolled window.
    GtkWidget* prop_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(prop_scrolled), property_tree_view);
    gtk_widget_set_vexpand(prop_scrolled, TRUE);
    gtk_widget_set_hexpand(prop_scrolled, TRUE);
    gtk_paned_pack2(GTK_PANED(hpaned), prop_scrolled, TRUE, FALSE);

    gtk_widget_show_all(window);
    gtk_main();
  });
  return gui_thread;
}
} // namespace hm

#if 0
/* Main program: creates a pipeline, sets it to PLAYING,
   then creates a GTK window with a horizontal pane:
   the left side shows pipeline elements,
   the right side shows the selected element’s properties. */
int main(int argc, char* argv[]) {
  gtk_init(&argc, &argv);
  gst_init(&argc, &argv);

  // Create (or parse) a pipeline.
  // If no command-line argument is provided, use a simple test pipeline.
  const char* pipeline_desc = (argc > 1) ? argv[1] : "videotestsrc ! videoconvert ! autovideosink";
  GError* error = nullptr;
  g_pipeline = gst_parse_launch(pipeline_desc, &error);
  if (!g_pipeline) {
    g_printerr("Failed to create pipeline: %s\n", error->message);
    g_error_free(error);
    return -1;
  }
  gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

  std::thread thrd = hm::edit_pipeline(GST_OBJECT(g_pipeline));
  thrd.join();

  // // Create the main window.
  // GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // gtk_window_set_title(GTK_WINDOW(window), "GStreamer Pipeline Editor");
  // gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  // g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

  // // Create a horizontal pane.
  // GtkWidget* hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  // gtk_container_add(GTK_CONTAINER(window), hpaned);

  // // -------------------------
  // // Left pane: Element list
  // // -------------------------
  // element_store = gtk_list_store_new(NUM_ELEM_COLS, G_TYPE_POINTER, G_TYPE_STRING);
  // element_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(element_store));
  // GtkCellRenderer* elem_renderer = gtk_cell_renderer_text_new();
  // GtkTreeViewColumn* elem_column =
  //     gtk_tree_view_column_new_with_attributes("Elements", elem_renderer, "text", COL_ELEMENT_NAME, nullptr);
  // gtk_tree_view_append_column(GTK_TREE_VIEW(element_tree_view), elem_column);

  // // Populate the element list from the pipeline.

  // std::map<std::string, GstElement*> element_tree = get_element_tree(GST_ELEMENT(g_pipeline));
  // for (auto& item : element_tree) {
  //   GtkTreeIter iter;
  //   gtk_list_store_append(element_store, &iter);
  //   gtk_list_store_set(element_store, &iter, COL_ELEMENT_PTR, item.second, COL_ELEMENT_NAME, item.first.c_str(), -1);
  // }

  // // GList* children = my_gst_bin_get_children(GST_BIN(g_pipeline));
  // // for (GList* l = children; l != nullptr; l = l->next) {
  // //   GstElement* element = GST_ELEMENT(l->data);
  // //   const gchar* name = gst_element_get_name(element);
  // //   GtkTreeIter iter;
  // //   gtk_list_store_append(element_store, &iter);
  // //   gtk_list_store_set(element_store, &iter, COL_ELEMENT_PTR, element, COL_ELEMENT_NAME, name, -1);
  // // }
  // // g_list_free(children);

  // // Connect selection changes to update the property list.
  // GtkTreeSelection* elem_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(element_tree_view));
  // g_signal_connect(elem_selection, "changed", G_CALLBACK(on_element_selection_changed), nullptr);

  // // Put the element list in a scrolled window.
  // GtkWidget* elem_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  // gtk_container_add(GTK_CONTAINER(elem_scrolled), element_tree_view);
  // gtk_widget_set_vexpand(elem_scrolled, TRUE);
  // gtk_widget_set_hexpand(elem_scrolled, TRUE);
  // gtk_paned_pack1(GTK_PANED(hpaned), elem_scrolled, TRUE, FALSE);

  // // -------------------------
  // // Right pane: Property list
  // // -------------------------
  // property_store = gtk_list_store_new(NUM_PROP_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER);
  // property_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(property_store));

  // // Add two columns: one for property names, one for their values.
  // GtkCellRenderer* prop_renderer = gtk_cell_renderer_text_new();
  // GtkTreeViewColumn* prop_column =
  //     gtk_tree_view_column_new_with_attributes("Property", prop_renderer, "text", COL_PROP_NAME, nullptr);
  // gtk_tree_view_append_column(GTK_TREE_VIEW(property_tree_view), prop_column);
  // GtkCellRenderer* val_renderer = gtk_cell_renderer_text_new();
  // GtkTreeViewColumn* val_column =
  //     gtk_tree_view_column_new_with_attributes("Value", val_renderer, "text", COL_PROP_VALUE, nullptr);
  // gtk_tree_view_append_column(GTK_TREE_VIEW(property_tree_view), val_column);

  // // When a property row is activated (double-clicked), open the editor dialog.
  // g_signal_connect(property_tree_view, "row-activated", G_CALLBACK(on_property_row_activated), nullptr);

  // // Put the property list in a scrolled window.
  // GtkWidget* prop_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  // gtk_container_add(GTK_CONTAINER(prop_scrolled), property_tree_view);
  // gtk_widget_set_vexpand(prop_scrolled, TRUE);
  // gtk_widget_set_hexpand(prop_scrolled, TRUE);
  // gtk_paned_pack2(GTK_PANED(hpaned), prop_scrolled, TRUE, FALSE);

  // gtk_widget_show_all(window);
  // gtk_main();

  // Cleanup: stop the pipeline.
  gst_element_set_state(g_pipeline, GST_STATE_NULL);
  gst_object_unref(g_pipeline);
  return 0;
}
#endif
