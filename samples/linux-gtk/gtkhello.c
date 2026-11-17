/* gtkhello: a GTK 4 window on THE WRLD OS, through the wrldcomp Wayland
 * compositor. A header bar (GTK's own decorations), a text entry, buttons, a
 * check box, a slider and a menu button whose popover is an xdg_popup.
 *   C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/gtkhello */
#include <gtk/gtk.h>

static GtkWidget *label, *entry, *level;
static int clicks;

static void on_greet(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    char msg[200];
    clicks++;
    g_snprintf(msg, sizeof msg, "Hello, %s! (%d click%s)", name && *name ? name : "world", clicks, clicks == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(label), msg);
}
static void on_scale(GtkRange *r, gpointer d) {
    (void)d;
    char msg[64];
    g_snprintf(msg, sizeof msg, "Level: %.0f", gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(level), msg);
}
static void on_toggle(GtkCheckButton *c, gpointer d) {
    (void)d;
    gtk_label_set_text(GTK_LABEL(label), gtk_check_button_get_active(c) ? "Dark style on" : "Dark style off");
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", gtk_check_button_get_active(c), NULL);
}
static void on_menu_item(GtkButton *b, gpointer pop) {
    gtk_label_set_text(GTK_LABEL(label), gtk_button_get_label(b));
    gtk_popover_popdown(GTK_POPOVER(pop));
}
static void activate(GtkApplication *app, gpointer d) {
    (void)d;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "GTK 4 on THE WRLD OS");
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 320);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *menu = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), "Menu");
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    const char *items[] = { "First item", "Second item", "Third item" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *it = gtk_button_new_with_label(items[i]);
        gtk_button_set_has_frame(GTK_BUTTON(it), FALSE);
        g_signal_connect(it, "clicked", G_CALLBACK(on_menu_item), pop);
        gtk_box_append(GTK_BOX(pbox), it);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), pbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), pop);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='x-large' weight='bold'>GTK 4 is running</span>");
    gtk_box_append(GTK_BOX(box), title);
    label = gtk_label_new("Type your name and press the button.");
    gtk_box_append(GTK_BOX(box), label);
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Your name");
    gtk_box_append(GTK_BOX(box), entry);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *greet = gtk_button_new_with_label("Say hello");
    gtk_widget_add_css_class(greet, "suggested-action");
    g_signal_connect(greet, "clicked", G_CALLBACK(on_greet), NULL);
    g_signal_connect(entry, "activate", G_CALLBACK(on_greet), NULL);
    gtk_box_append(GTK_BOX(row), greet);
    GtkWidget *check = gtk_check_button_new_with_label("Dark style");
    g_signal_connect(check, "toggled", G_CALLBACK(on_toggle), NULL);
    gtk_box_append(GTK_BOX(row), check);
    gtk_box_append(GTK_BOX(box), row);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(scale), 40);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale), NULL);
    gtk_box_append(GTK_BOX(box), scale);
    level = gtk_label_new("Level: 40");
    gtk_box_append(GTK_BOX(box), level);
    gtk_window_set_child(GTK_WINDOW(win), box);
    gtk_window_present(GTK_WINDOW(win));
}
int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("os.wrld.GtkHello", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
