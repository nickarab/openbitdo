/*
 * 8bdgui — 8BitDo Ultimate Software nativo para Linux (GTK4, C23)
 *
 * A navegação segue o app oficial: barra de abas (Mapping / Sticks / Triggers /
 * Vibration / Macro / Perfil). O painel esquerdo explica o que foi detectado,
 * o que cada aba faz e os limites do protocolo; o painel direito edita o
 * perfil de 1652 bytes.
 *
 * Protocolo, structs e CRC recuperados por engenharia reversa de
 * 8BitDoAdvance.dll + assembly .NET do app oficial (ver ../re/01..09*.md).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "bd_hid.h"
#include "bd_usb.h"

/* ---------------------------------------------------------------- protocolo */

/* ---- limites e periodicidades da interface ---- */
constexpr int NARROW_WINDOW_MAX_WIDTH = 1100;
constexpr int DEFAULT_WINDOW_WIDTH = 1200;
constexpr int DEFAULT_WINDOW_HEIGHT = 700;
constexpr int PANED_START_PERCENT = 45;
constexpr int PANED_VERTICAL_PERCENT = 52;
constexpr int PANED_INITIAL_POSITION = DEFAULT_WINDOW_WIDTH * PANED_START_PERCENT / 100;
constexpr guint PROGRESS_REFRESH_INTERVAL_MS = 200;
constexpr guint LAYOUT_POLL_INTERVAL_MS = 250;
constexpr guint PANED_SETTLE_DELAY_MS = 50;
constexpr guint AUTO_READ_DELAY_MS = 600;
constexpr guint LOOP_PROBE_INTERVAL_MS = 50;
constexpr int LOOP_PROBE_TICKS = 30;
constexpr int VIBRATION_MAX_LEVEL = 5;
constexpr int BYTE_MAX = 255;
constexpr double BYTE_MAX_VALUE = 255.0;
constexpr double PERCENT_MAX = 100.0;
constexpr double PERCENT_STEP = 1.0;

/* ---- layout dos sub-registros por slot (ver bd_core.h) ---- */
constexpr size_t RUMBLE_SLOT_SIZE = 12;
constexpr size_t STICK_SLOT_SIZE = 8;
constexpr size_t TRIGGER_SLOT_SIZE = 8;
constexpr size_t MAPPING_SLOT_SIZE = 84;
constexpr size_t MACRO_SLOT_SIZE = 392;
constexpr size_t PROFILE_VALUE_OFFSET = 4;
constexpr size_t MACRO_STEP_DATA_OFFSET = 8;
constexpr size_t MACRO_INTERVAL_OFFSET = 4;
constexpr size_t MACRO_KEY_INDEX_OFFSET = 40;
constexpr size_t MACRO_STEP_COUNT = 4;
constexpr size_t MACRO_STEP_SIZE = 96;
constexpr size_t MACRO_KEY_COUNT = 18;

static_assert(BD_REC_LEN == 1652, "o perfil do 8BitDo Ultimate tem 1652 bytes");

/* ------------------------------------------------------------------- modelo */


static uint8_t record[BD_REC_LEN];
static struct bd_hid_link g_hid_link = {.fd = -1};
/* bd_hid_link não guarda o path; o painel de transporte precisa dele. */
static char g_hid_path[BD_HID_PATH_SIZE];
static volatile int g_transfer_progress;
static bool record_loaded;
static bool g_auto_read;

static char *profile_name_dup(const uint8_t *profile, int slot)
{
    char name[64];
    bd_name_get(profile, slot, name, sizeof name);
    return g_strdup(name);
}

/* ---------------------------------------------------------------------- UI  */

struct ui {
    GtkWidget *window;
    GtkWidget *stack;
    GtkWidget *device_combo;
    GtkWidget *status_label;
    GtkWidget *slot_indicator;
    GtkWidget *profile_dropdown;
    GtkWidget *name_entry;
    GtkWidget *mode_dropdown;
    GtkWidget *active_slot_spin;
    GtkWidget *sync_button;
    GtkWidget *read_button;
    GtkWidget *write_permission_switch;
    GtkWidget *mapping_dropdowns[BD_NKEYS];
    GtkWidget *stick_percent[2][2];    /* [stick][início/fim] */
    GtkWidget *trigger_percent[2][2];
    GtkWidget *vibration_level[2];
    GtkWidget *feature_entry;
    GtkWidget *transport_value;
    GtkWidget *macro_view;
    GtkWidget *body_paned;
    int layout_width;
    int layout_height;
    bool layout_initialized;
    int current_slot;
};

static struct ui U;
static bool g_busy;

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(U.status_label), s);
    g_free(s);
}

/* -------------------------------------------------------- painel esquerdo */

struct tab_help {
    const char *tab;
    const char *description;
};

static const struct tab_help TAB_HELP[] = {
    {"Mapping", "Escolhe qual comando cada botão envia ao controle."},
    {"Sticks", "Ajusta a zona ativa dos analógicos e os bits de recursos."},
    {"Triggers", "Ajusta a zona ativa dos gatilhos analógicos."},
    {"Vibration", "Define a intensidade da vibração (0 a 5) de cada lado."},
    {"Macro", "Mostra a leitura crua das macros gravadas no perfil."},
    {"Perfil", "Edita nome, modo e slot ativo do perfil."},
};

static const char KNOWN_LIMITS[] =
    "• O perfil de 1652 bytes só é acessível no modo HID (dongle ou cabo HID).\n"
    "• A gravação fica bloqueada até ligar \"Permitir gravar\" na barra superior.";

static_assert(G_N_ELEMENTS(TAB_HELP) == 6, "uma linha de ajuda para cada aba");

static GtkWidget *make_info_label(const char *text, bool monospace)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(label, true);
    if (monospace) gtk_widget_add_css_class(label, "monospace");
    return label;
}

static GtkWidget *make_section_heading(const char *title)
{
    GtkWidget *label = gtk_label_new(title);
    gtk_widget_add_css_class(label, "bd-section");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    return label;
}

static void append_hid_transport_lines(GString *text, const struct bd_hid_device *devices,
                                       int count)
{
    for (int i = 0; i < count; i++) {
        const bool opened = g_hid_link.fd >= 0 &&
                            g_strcmp0(g_hid_path, devices[i].path) == 0;
        g_string_append_printf(text,
            "HID · %s (PID 0x%04x)\n"
            "perfil de 1652 bytes via reports vendor 0x81/0x02 · %s",
            devices[i].path, devices[i].pid, opened ? "aberto" : "não aberto");
        if (i + 1 < count) g_string_append(text, "\n\n");
    }
}

/* Mostra o HID detectado e o estado do link. */
static void refresh_transport_panel(void)
{
    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    GString *text = g_string_new(nullptr);

    if (count > 0) append_hid_transport_lines(text, devices, count);
    if (text->len == 0) {
        g_string_append(text,
            "Nenhum controle encontrado.\n"
            "Conecte por USB e clique em Procurar.");
    }
    gtk_label_set_text(GTK_LABEL(U.transport_value), text->str);
    g_string_free(text, true);
}

static GtkWidget *build_tab_help(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    for (size_t i = 0; i < G_N_ELEMENTS(TAB_HELP); i++) {
        GtkWidget *name = gtk_label_new(TAB_HELP[i].tab);
        gtk_widget_add_css_class(name, "bd-tab-name");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_set_valign(name, GTK_ALIGN_START);
        gtk_widget_set_size_request(name, 92, -1);
        GtkWidget *description = make_info_label(TAB_HELP[i].description, false);
        gtk_grid_attach(GTK_GRID(grid), name, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), description, 1, (int)i, 1, 1);
    }
    return grid;
}

static GtkWidget *build_info_panel(void)
{
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(panel, 22);
    gtk_widget_set_margin_bottom(panel, 22);
    gtk_widget_set_margin_start(panel, 22);
    gtk_widget_set_margin_end(panel, 22);

    GtkWidget *title = gtk_label_new("8BitDo Ultimate");
    gtk_widget_add_css_class(title, "bd-panel-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(panel), title);

    GtkWidget *subtitle = make_info_label(
        "Configurador nativo para Linux. Lê, edita e grava o perfil do controle por USB.",
        false);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_box_append(GTK_BOX(panel), subtitle);

    gtk_box_append(GTK_BOX(panel), make_section_heading("Transporte"));
    U.transport_value = make_info_label("procurando controle...", true);
    gtk_box_append(GTK_BOX(panel), U.transport_value);

    gtk_box_append(GTK_BOX(panel), make_section_heading("O que cada aba faz"));
    gtk_box_append(GTK_BOX(panel), build_tab_help());

    gtk_box_append(GTK_BOX(panel), make_section_heading("Limites conhecidos"));
    GtkWidget *limits = make_info_label(KNOWN_LIMITS, false);
    gtk_widget_add_css_class(limits, "dim-label");
    gtk_box_append(GTK_BOX(panel), limits);

    return panel;
}

/* ------------------------------------------------------------- atualização */

static void refresh_ui(void)
{
    if (!record_loaded) return;

    uint16_t active_slot = bd_rd16(record, BD_OFF_SLOT);
    GString *indicator = g_string_new(nullptr);
    for (int slot = 0; slot < BD_SLOTS; slot++)
        g_string_append(indicator, slot == active_slot ? "● " : "○ ");
    gtk_label_set_text(GTK_LABEL(U.slot_indicator), indicator->str);
    g_string_free(indicator, true);

    char *profile_name = profile_name_dup(record, U.current_slot);
    gtk_editable_set_text(GTK_EDITABLE(U.name_entry), profile_name);
    g_free(profile_name);

    uint16_t mode = bd_rd16(record, BD_OFF_MODE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(U.mode_dropdown), mode <= 3 ? mode : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.active_slot_spin), active_slot);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(U.profile_dropdown), (guint)U.current_slot);

    /* sticks e gatilhos: bytes 0..255 -> percentual 0..100 */
    for (int axis = 0; axis < 2; axis++) {
        size_t stick_offset = BD_OFF_JOY + (size_t)U.current_slot * STICK_SLOT_SIZE
                              + PROFILE_VALUE_OFFSET + (size_t)axis * 2;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.stick_percent[axis][0]),
                                  record[stick_offset] * PERCENT_MAX / BYTE_MAX_VALUE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.stick_percent[axis][1]),
                                  record[stick_offset + 1] * PERCENT_MAX / BYTE_MAX_VALUE);

        size_t trigger_offset = BD_OFF_TRG + (size_t)U.current_slot * TRIGGER_SLOT_SIZE
                                + PROFILE_VALUE_OFFSET + (size_t)axis * 2;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.trigger_percent[axis][0]),
                                  record[trigger_offset] * PERCENT_MAX / BYTE_MAX_VALUE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.trigger_percent[axis][1]),
                                  record[trigger_offset + 1] * PERCENT_MAX / BYTE_MAX_VALUE);
    }

    /* vibração: float -> nível 0..5 */
    size_t rumble_offset = BD_OFF_RUM + (size_t)U.current_slot * RUMBLE_SLOT_SIZE;
    for (int side = 0; side < 2; side++) {
        float stored_level = 0.0f;
        memcpy(&stored_level, record + rumble_offset + PROFILE_VALUE_OFFSET
               + (size_t)side * sizeof stored_level, sizeof stored_level);
        int level = (int)lroundf(stored_level * (float)VIBRATION_MAX_LEVEL);
        level = CLAMP(level, 0, VIBRATION_MAX_LEVEL);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(U.vibration_level[side]), level);
    }

    /* feature bits (não mapeados) */
    uint32_t features = bd_rd32(record, BD_OFF_FEAT + (size_t)U.current_slot * 8
                                + PROFILE_VALUE_OFFSET);
    char feature_text[16];
    g_snprintf(feature_text, sizeof feature_text, "0x%08x", features);
    gtk_editable_set_text(GTK_EDITABLE(U.feature_entry), feature_text);

    /* mapeamento */
    size_t mapping_offset = BD_OFF_MAP + (size_t)U.current_slot * MAPPING_SLOT_SIZE
                            + PROFILE_VALUE_OFFSET;
    for (int key = 0; key < BD_NKEYS; key++)
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.mapping_dropdowns[key]),
            (guint)bd_action_index(bd_rd32(record, mapping_offset + (size_t)key * 4)));

    /* macro: leitura crua (parcialmente mapeada) */
    if (U.macro_view) {
        GString *macro_text = g_string_new(nullptr);
        size_t macro_offset = BD_OFF_MAC + (size_t)U.current_slot * MACRO_SLOT_SIZE;
        g_string_append_printf(macro_text, "flag=0x%08x  total_cnt=%u\n",
                               bd_rd32(record, macro_offset), record[macro_offset + PROFILE_VALUE_OFFSET]);
        for (size_t step = 0; step < MACRO_STEP_COUNT; step++) {
            size_t step_offset = macro_offset + MACRO_STEP_DATA_OFFSET + step * MACRO_STEP_SIZE;
            g_string_append_printf(macro_text,
                                   "\npasso %zu: key_map=0x%08x\n  key_index:",
                                   step, bd_rd32(record, step_offset));
            for (size_t key = 0; key < MACRO_KEY_COUNT; key++)
                g_string_append_printf(macro_text, " %u",
                    bd_rd16(record, step_offset + MACRO_KEY_INDEX_OFFSET + key * 2));
            g_string_append(macro_text, "\n  interval_ms:");
            for (size_t key = 0; key < MACRO_KEY_COUNT; key++)
                g_string_append_printf(macro_text, " %u",
                    bd_rd16(record, step_offset + MACRO_INTERVAL_OFFSET + key * 2));
        }
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(U.macro_view)),
                                 macro_text->str, -1);
        g_string_free(macro_text, true);
    }
}

/* --------------------------------------------------------------- callbacks */

static void on_map_changed(GtkDropDown *dropdown, GParamSpec *spec [[maybe_unused]],
                           gpointer data)
{
    int key_index = GPOINTER_TO_INT(data);
    if (!record_loaded) return;

    guint selected = gtk_drop_down_get_selected(dropdown);
    if (selected >= bd_actions_count) return;

    size_t mapping_offset = BD_OFF_MAP + (size_t)U.current_slot * MAPPING_SLOT_SIZE
                            + PROFILE_VALUE_OFFSET;
    bd_wr32(record, mapping_offset + (size_t)key_index * 4, bd_actions[selected].value);
}

struct range_binding {
    size_t base_offset;
    size_t slot_size;
    int axis;
    int endpoint;
};

enum { AXIS_LEFT, AXIS_RIGHT };
enum { RANGE_START, RANGE_END };

static void on_range_changed(GtkSpinButton *spin, gpointer data)
{
    struct range_binding *binding = data;
    if (!record_loaded) return;

    size_t offset = binding->base_offset + (size_t)U.current_slot * binding->slot_size
                    + PROFILE_VALUE_OFFSET + (size_t)binding->axis * 2
                    + (size_t)binding->endpoint;
    double percent = gtk_spin_button_get_value(spin);
    int byte = (int)lround(percent * BYTE_MAX_VALUE / PERCENT_MAX);
    record[offset] = (uint8_t)CLAMP(byte, 0, BYTE_MAX);
}

static void on_vib_changed(GtkSpinButton *spin, gpointer data)
{
    int side = GPOINTER_TO_INT(data);
    if (!record_loaded) return;

    float level = (float)(gtk_spin_button_get_value(spin) / VIBRATION_MAX_LEVEL);
    size_t offset = BD_OFF_RUM + (size_t)U.current_slot * RUMBLE_SLOT_SIZE
                    + PROFILE_VALUE_OFFSET + (size_t)side * sizeof level;
    memcpy(record + offset, &level, sizeof level);
}

static void on_feat_changed(GtkEntry *entry, gpointer data [[maybe_unused]])
{
    if (!record_loaded) return;

    uint32_t features = (uint32_t)strtoul(gtk_editable_get_text(GTK_EDITABLE(entry)),
                                          nullptr, 16);
    bd_wr32(record, BD_OFF_FEAT + (size_t)U.current_slot * 8 + PROFILE_VALUE_OFFSET,
            features);
}

static void on_profile_changed(GtkDropDown *dropdown, GParamSpec *spec [[maybe_unused]],
                               gpointer data [[maybe_unused]])
{
    U.current_slot = (int)gtk_drop_down_get_selected(dropdown);
    refresh_ui();
}

static void on_name_activate(GtkEntry *entry, gpointer data [[maybe_unused]])
{
    if (!record_loaded) return;
    bd_name_set(record, U.current_slot, gtk_editable_get_text(GTK_EDITABLE(entry)));
    refresh_ui();
}

static void on_mode_changed(GtkDropDown *dropdown, GParamSpec *spec [[maybe_unused]],
                            gpointer data [[maybe_unused]])
{
    if (!record_loaded) return;
    bd_wr16(record, BD_OFF_MODE, (uint16_t)gtk_drop_down_get_selected(dropdown));
}

static void on_active_changed(GtkSpinButton *spin, gpointer data [[maybe_unused]])
{
    if (!record_loaded) return;
    bd_wr16(record, BD_OFF_SLOT, (uint16_t)gtk_spin_button_get_value(spin));
    refresh_ui();
}

/* ------------------------------------------------------------ I/O assíncrono */

static gboolean on_io_done(gpointer data)
{
    int result = GPOINTER_TO_INT(data);
    g_busy = false;
    gtk_widget_set_sensitive(U.read_button, true);
    gtk_widget_set_sensitive(U.sync_button, true);

    if (result < 0) {
        set_status("sem resposta do dispositivo — confirme que o controle está ligado e "
                   "conectado por USB num modo HID. Nada foi alterado.");
        return G_SOURCE_REMOVE;
    }

    record_loaded = true;
    refresh_ui();
    set_status("sincronizado: slot %u · modo %s · contador %u · %d bytes",
               bd_rd16(record, BD_OFF_SLOT),
               bd_rd16(record, BD_OFF_MODE) <= 3
                   ? bd_mode_names[bd_rd16(record, BD_OFF_MODE)] : "?",
               bd_rd32(record, BD_OFF_CRC), BD_REC_LEN);
    return G_SOURCE_REMOVE;
}

static gboolean on_progress_tick(gpointer data [[maybe_unused]])
{
    if (!g_busy) return G_SOURCE_REMOVE;
    int progress = g_transfer_progress;
    if (progress > 0)
        set_status("transferindo... %d/%d bytes", progress, BD_REC_LEN);
    return G_SOURCE_CONTINUE;
}

struct io_job {
    bool is_write;
    uint8_t *profile;
};

static gpointer io_thread(gpointer data)
{
    struct io_job *job = data;
    const bool done = job->is_write
        ? bd_hid_write_record(&g_hid_link, job->profile)
        : bd_hid_read_record(&g_hid_link, record, &g_transfer_progress);
    g_free(job->profile);
    g_free(job);
    g_idle_add(on_io_done, GINT_TO_POINTER(done ? 0 : -1));
    return nullptr;
}

static void start_io(bool is_write)
{
    if (g_hid_link.fd < 0) {
        set_status("nenhum dispositivo conectado");
        return;
    }
    if (g_busy) {
        set_status("já existe uma operação em andamento");
        return;
    }
    if (is_write) {
        if (!record_loaded) {
            set_status("leia o perfil antes de sincronizar");
            return;
        }
        if (!gtk_switch_get_active(GTK_SWITCH(U.write_permission_switch))) {
            set_status("sincronização bloqueada: ligue 'Permitir gravar'");
            return;
        }
    }

    struct io_job *job = g_new0(struct io_job, 1);
    job->is_write = is_write;
    job->profile = is_write ? g_memdup2(record, BD_REC_LEN) : nullptr;

    g_busy = true;
    g_transfer_progress = 0;
    gtk_widget_set_sensitive(U.read_button, false);
    gtk_widget_set_sensitive(U.sync_button, false);
    set_status(is_write ? "sincronizando com o controle..." : "lendo perfil...");
    g_thread_unref(g_thread_new("io", io_thread, job));
    g_timeout_add(PROGRESS_REFRESH_INTERVAL_MS, on_progress_tick, nullptr);
}

static void on_read(void) { start_io(false); }
static void on_write(void) { start_io(true); }

static void open_hid_device(const struct bd_hid_device *device)
{
    bd_hid_close(&g_hid_link);
    g_hid_path[0] = '\0';
    if (!bd_hid_open(&g_hid_link, device->path, device->pid)) {
        set_status("falha ao abrir %s: %s", device->path, g_strerror(errno));
        refresh_transport_panel();
        return;
    }
    g_strlcpy(g_hid_path, device->path, sizeof g_hid_path);
    set_status("conectado em %s (PID %04x)", device->path, device->pid);
    refresh_transport_panel();
}

static void on_device_changed(GtkDropDown *dropdown, GParamSpec *spec [[maybe_unused]],
                              gpointer data [[maybe_unused]])
{
    guint selected = gtk_drop_down_get_selected(dropdown);
    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count <= 0 || selected >= (guint)count) return;
    open_hid_device(&devices[selected]);
}


/* O controle em XInput (2dc8:3106) nao tem hidraw: manda o changeDinput_CMD pelo
 * endpoint vendor e espera o re-enumero. Sem isso o app pediria "aperte HOME". */
static bool g_ensuring_hid;
static void on_reload_devices(void);

static gboolean on_hid_ready(gpointer data [[maybe_unused]])
{
    on_reload_devices();
    return G_SOURCE_REMOVE;
}

static gpointer ensure_hid_thread(gpointer data [[maybe_unused]])
{
    if (bd_usb_switch_mode(1)) {
        for (int attempt = 0; attempt < 60; attempt++) {
            struct bd_hid_device probe[BD_HID_MAX_DEVICES];
            if (bd_hid_find(probe, BD_HID_MAX_DEVICES) > 0) break;
            g_usleep(100000);
        }
    }
    g_idle_add(on_hid_ready, nullptr);
    return nullptr;
}

static void on_reload_devices(void)
{

    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count < 0) count = 0;

    char **items = g_new0(char *, (gsize)count + 1);
    for (int i = 0; i < count; i++)
        items[i] = g_strdup_printf("%s  (%04x:%04x)", devices[i].path, BD_VID,
                                   devices[i].pid);

    const char *placeholder = "(nenhum 8BitDo)";
    GtkStringList *model = count > 0
        ? gtk_string_list_new((const char *const *)items)
        : gtk_string_list_new((const char *const[]){placeholder, nullptr});
    gtk_drop_down_set_model(GTK_DROP_DOWN(U.device_combo), G_LIST_MODEL(model));
    g_object_unref(model);
    for (int i = 0; i < count; i++) g_free(items[i]);
    g_free(items);

    if (count > 0) {
        g_ensuring_hid = false;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.device_combo), 0);
        open_hid_device(&devices[0]);
    } else if (bd_usb_has_xinput() && !g_ensuring_hid) {
        g_ensuring_hid = true;
        set_status("controle em modo XInput: ativando o canal de configuração...");
        g_thread_unref(g_thread_new("ensure-hid", ensure_hid_thread, nullptr));
    } else {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(U.device_combo), 0);
        bd_hid_close(&g_hid_link);
        g_hid_path[0] = '\0';
        set_status("nenhum 8BitDo encontrado (VID 2dc8)");
        refresh_transport_panel();
    }
}

/* --------------------------------------------------------------- montagem  */

static GtkWidget *page_mapping(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_widget_set_margin_top(grid, 14);
    gtk_widget_set_margin_bottom(grid, 14);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);

    GtkWidget *button_header = gtk_label_new("Botão");
    GtkWidget *arrow_header = gtk_label_new("");
    GtkWidget *command_header = gtk_label_new("Comando");
    gtk_widget_add_css_class(button_header, "dim-label");
    gtk_widget_add_css_class(command_header, "dim-label");
    gtk_widget_set_halign(button_header, GTK_ALIGN_START);
    gtk_widget_set_halign(command_header, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), button_header, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), arrow_header, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), command_header, 2, 0, 1, 1);

    const char **action_names = g_new0(const char *, bd_actions_count + 1);
    for (size_t i = 0; i < bd_actions_count; i++) action_names[i] = bd_actions[i].name;

    for (int key = 0; key < BD_NKEYS; key++) {
        GtkWidget *label = gtk_label_new(bd_key_long[key]);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_size_request(label, 210, -1);
        gtk_grid_attach(GTK_GRID(grid), label, 0, key + 1, 1, 1);

        GtkWidget *arrow = gtk_label_new("›");
        gtk_widget_add_css_class(arrow, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), arrow, 1, key + 1, 1, 1);

        GtkWidget *dropdown = gtk_drop_down_new_from_strings(action_names);
        gtk_widget_set_hexpand(dropdown, true);
        g_signal_connect(dropdown, "notify::selected",
                         G_CALLBACK(on_map_changed), GINT_TO_POINTER(key));
        gtk_grid_attach(GTK_GRID(grid), dropdown, 2, key + 1, 1, 1);
        U.mapping_dropdowns[key] = dropdown;
    }
    g_free(action_names);
    return grid;
}

static void free_range_binding(gpointer data, GClosure *closure [[maybe_unused]])
{
    g_free(data);
}

static void connect_range_spin(GtkWidget *spin, size_t base_offset, size_t slot_size,
                               int axis, int endpoint)
{
    struct range_binding *binding = g_new(struct range_binding, 1);
    *binding = (struct range_binding){
        .base_offset = base_offset,
        .slot_size = slot_size,
        .axis = axis,
        .endpoint = endpoint,
    };
    g_signal_connect_data(spin, "value-changed", G_CALLBACK(on_range_changed),
                          binding, free_range_binding, 0);
}

static GtkWidget *range_row(const char *title, GtkWidget **start, GtkWidget **end,
                            size_t base_offset, size_t slot_size, int axis)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *title_label = gtk_label_new(title);
    gtk_widget_add_css_class(title_label, "title-4");
    gtk_widget_set_halign(title_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), title_label);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    *start = gtk_spin_button_new_with_range(0, PERCENT_MAX, PERCENT_STEP);
    *end = gtk_spin_button_new_with_range(0, PERCENT_MAX, PERCENT_STEP);
    gtk_widget_set_hexpand(*start, true);
    gtk_widget_set_hexpand(*end, true);
    connect_range_spin(*start, base_offset, slot_size, axis, RANGE_START);
    connect_range_spin(*end, base_offset, slot_size, axis, RANGE_END);

    GtkWidget *start_hint = gtk_label_new("0");
    GtkWidget *end_hint = gtk_label_new("100");
    gtk_widget_add_css_class(start_hint, "dim-label");
    gtk_widget_add_css_class(end_hint, "dim-label");
    gtk_box_append(GTK_BOX(bar), start_hint);
    gtk_box_append(GTK_BOX(bar), *start);
    gtk_box_append(GTK_BOX(bar), *end);
    gtk_box_append(GTK_BOX(bar), end_hint);
    gtk_box_append(GTK_BOX(box), bar);

    GtkWidget *legend = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(legend),
        "<span size='small' color='#7fb2ff'>● área ativa</span>   "
        "<span size='small' color='#8a8a8a'>● área inativa</span>");
    gtk_widget_set_halign(legend, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), legend);
    return box;
}

static GtkWidget *page_sticks(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    gtk_box_append(GTK_BOX(box), range_row("Left Stick", &U.stick_percent[AXIS_LEFT][0],
                                           &U.stick_percent[AXIS_LEFT][1],
                                           BD_OFF_JOY, STICK_SLOT_SIZE, AXIS_LEFT));
    gtk_box_append(GTK_BOX(box), range_row("Right Stick", &U.stick_percent[AXIS_RIGHT][0],
                                           &U.stick_percent[AXIS_RIGHT][1],
                                           BD_OFF_JOY, STICK_SLOT_SIZE, AXIS_RIGHT));

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *note = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(note),
        "<span size='small'>Invert X/Y, Swap Sticks, Disable Deadzone e Dpad Four-way\n"
        "ficam no bitmask <b>featrue</b> (4 bytes). Os bits individuais não foram\n"
        "identificados na engenharia reversa; por isso o valor é editado cru:</span>");
    gtk_label_set_wrap(GTK_LABEL(note), true);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), note);

    U.feature_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(U.feature_entry), "0x00000000");
    gtk_entry_set_max_length(GTK_ENTRY(U.feature_entry), 10);
    g_signal_connect(U.feature_entry, "changed", G_CALLBACK(on_feat_changed), nullptr);

    GtkWidget *feature_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *feature_label = gtk_label_new("featrue");
    gtk_widget_set_size_request(feature_label, 90, -1);
    gtk_widget_set_halign(feature_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(feature_row), feature_label);
    gtk_box_append(GTK_BOX(feature_row), U.feature_entry);
    gtk_box_append(GTK_BOX(box), feature_row);
    return box;
}

static GtkWidget *page_triggers(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    gtk_box_append(GTK_BOX(box), range_row("Left Trigger", &U.trigger_percent[AXIS_LEFT][0],
                                           &U.trigger_percent[AXIS_LEFT][1],
                                           BD_OFF_TRG, TRIGGER_SLOT_SIZE, AXIS_LEFT));
    gtk_box_append(GTK_BOX(box), range_row("Right Trigger", &U.trigger_percent[AXIS_RIGHT][0],
                                           &U.trigger_percent[AXIS_RIGHT][1],
                                           BD_OFF_TRG, TRIGGER_SLOT_SIZE, AXIS_RIGHT));
    return box;
}

static GtkWidget *page_vibration(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 40);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    static const char *const SIDE_NAMES[] = {"Left", "Right"};
    for (size_t side = 0; side < G_N_ELEMENTS(SIDE_NAMES); side++) {
        GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *title = gtk_label_new(SIDE_NAMES[side]);
        gtk_widget_add_css_class(title, "title-4");
        gtk_box_append(GTK_BOX(column), title);

        GtkWidget *spin = gtk_spin_button_new_with_range(0, VIBRATION_MAX_LEVEL, 1);
        gtk_widget_set_size_request(spin, 120, -1);
        g_signal_connect(spin, "value-changed", G_CALLBACK(on_vib_changed),
                         GINT_TO_POINTER((int)side));
        gtk_box_append(GTK_BOX(column), spin);
        U.vibration_level[side] = spin;

        GtkWidget *hint = gtk_label_new("0 — 5");
        gtk_widget_add_css_class(hint, "dim-label");
        gtk_box_append(GTK_BOX(column), hint);
        gtk_box_append(GTK_BOX(row), column);
    }
    gtk_box_append(GTK_BOX(box), row);

    GtkWidget *note = gtk_label_new(
        "Nível 0–5 gravado como float (nível / 5) em zoom_out[2].");
    gtk_widget_add_css_class(note, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(note), true);
    gtk_box_append(GTK_BOX(box), note);
    return box;
}

static GtkWidget *page_macro(void)
{
    GtkWidget *scroller = gtk_scrolled_window_new();
    U.macro_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(U.macro_view), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(U.macro_view), true);
    gtk_widget_set_margin_top(U.macro_view, 12);
    gtk_widget_set_margin_start(U.macro_view, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), U.macro_view);
    return scroller;
}

static GtkWidget *page_profile(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);

    U.name_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(U.name_entry), BD_NAME_UNITS - 1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(U.name_entry), "nome do perfil");
    g_signal_connect(U.name_entry, "activate", G_CALLBACK(on_name_activate), nullptr);

    static const char *const MODE_NAMES[] = {
        "Switch", "DInput", "(desconhecido)", "XInput", nullptr,
    };
    U.mode_dropdown = gtk_drop_down_new_from_strings(MODE_NAMES);
    g_signal_connect(U.mode_dropdown, "notify::selected",
                     G_CALLBACK(on_mode_changed), nullptr);

    U.active_slot_spin = gtk_spin_button_new_with_range(0, BD_SLOTS - 1, 1);
    g_signal_connect(U.active_slot_spin, "value-changed",
                     G_CALLBACK(on_active_changed), nullptr);

    const char *labels[] = {"Nome do perfil", "Modo", "Slot ativo"};
    GtkWidget *values[] = {U.name_entry, U.mode_dropdown, U.active_slot_spin};
    for (size_t row_index = 0; row_index < G_N_ELEMENTS(labels); row_index++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *label = gtk_label_new(labels[row_index]);
        gtk_widget_set_size_request(label, 130, -1);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), label);
        gtk_widget_set_hexpand(values[row_index], row_index == 0);
        gtk_box_append(GTK_BOX(row), values[row_index]);
        gtk_box_append(GTK_BOX(box), row);
    }
    return box;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller [[maybe_unused]],
                               guint keyval, guint keycode [[maybe_unused]],
                               GdkModifierType state, gpointer data [[maybe_unused]])
{
    if (!(state & GDK_CONTROL_MASK)) return false;
    switch (gdk_keyval_to_lower(keyval)) {
    case GDK_KEY_r: on_read(); return true;
    case GDK_KEY_g: on_write(); return true;
    case GDK_KEY_p: on_reload_devices(); return true;
    default: return false;
    }
}

static gboolean auto_read_cb(gpointer data [[maybe_unused]])
{
    on_read();
    return G_SOURCE_REMOVE;
}

/* ---- sonda de responsividade do main loop (teste de regressão do freeze) ----
 * Mede o intervalo real entre ticks de 50 ms. Se algum I/O (USB/HID) rodar na
 * thread da UI, o gap máximo dispara. */
static gint64 g_probe_last, g_probe_max;
static int g_probe_ticks;

static gboolean probe_tick(gpointer data)
{
    gint64 now = g_get_monotonic_time();
    if (g_probe_last) {
        gint64 gap = (now - g_probe_last) / 1000;
        if (gap > g_probe_max) g_probe_max = gap;
    }
    g_probe_last = now;
    if (++g_probe_ticks >= LOOP_PROBE_TICKS) {
        fprintf(stderr, "LOOP max_gap_ms=%" G_GINT64_FORMAT "\n", g_probe_max);
        fflush(stderr);
        g_application_quit(G_APPLICATION(data));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static int desired_paned_position(GtkWidget *paned)
{
    int width = gtk_widget_get_width(GTK_WIDGET(U.window));
    int height = gtk_widget_get_height(GTK_WIDGET(U.window));
    if (gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) == GTK_ORIENTATION_HORIZONTAL)
        return width * PANED_START_PERCENT / 100;
    return height * PANED_VERTICAL_PERCENT / 100;
}

/* A troca de orientação reajusta o GtkPaned no próximo layout; reaplicar a
 * posição depois desse ajuste evita que o valor antigo (largura × altura) vença. */
static gboolean apply_paned_position_cb(gpointer data)
{
    GtkWidget *paned = GTK_WIDGET(data);
    gtk_paned_set_position(GTK_PANED(paned), desired_paned_position(paned));
    return G_SOURCE_REMOVE;
}

/* adaptação a tiling (Hyprland): horizontal se largo, vertical se estreito */
static gboolean adapt_layout_cb(gpointer data)
{
    GtkWidget *paned = GTK_WIDGET(data);
    int width = gtk_widget_get_width(GTK_WIDGET(U.window));
    int height = gtk_widget_get_height(GTK_WIDGET(U.window));
    if (width <= 1 || height <= 1) return G_SOURCE_CONTINUE;

    GtkOrientation wanted = (width < NARROW_WINDOW_MAX_WIDTH)
        ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    const char *forced = g_getenv("BD_LAYOUT");   /* uso manual: vertical|horizontal */
    if (forced && !g_strcmp0(forced, "vertical")) wanted = GTK_ORIENTATION_VERTICAL;
    if (forced && !g_strcmp0(forced, "horizontal")) wanted = GTK_ORIENTATION_HORIZONTAL;

    bool orientation_changed =
        gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) != wanted;
    bool size_changed = !U.layout_initialized || width != U.layout_width ||
                        height != U.layout_height;
    if (!orientation_changed && !size_changed) return G_SOURCE_CONTINUE;

    if (orientation_changed)
        gtk_orientable_set_orientation(GTK_ORIENTABLE(paned), wanted);
    int position = wanted == GTK_ORIENTATION_HORIZONTAL
        ? width * PANED_START_PERCENT / 100
        : height * PANED_VERTICAL_PERCENT / 100;
    gtk_paned_set_position(GTK_PANED(paned), position);
    g_timeout_add_full(G_PRIORITY_DEFAULT, PANED_SETTLE_DELAY_MS,
                       apply_paned_position_cb, g_object_ref(paned),
                       g_object_unref);
    U.layout_width = width;
    U.layout_height = height;
    U.layout_initialized = true;
    return G_SOURCE_CONTINUE;
}

static GtkWidget *build_top_bar(void)
{
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(top, "toolbar");
    gtk_widget_set_margin_top(top, 8);
    gtk_widget_set_margin_bottom(top, 8);
    gtk_widget_set_margin_start(top, 8);
    gtk_widget_set_margin_end(top, 8);

    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(U.stack));
    gtk_widget_add_css_class(switcher, "bd-tabs");
    gtk_box_append(GTK_BOX(top), switcher);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, true);
    gtk_box_append(GTK_BOX(top), spacer);

    U.slot_indicator = gtk_label_new("○ ○ ○");
    gtk_widget_add_css_class(U.slot_indicator, "dim-label");
    gtk_box_append(GTK_BOX(top), U.slot_indicator);

    static const char *const PROFILE_NAMES[] = {
        "Profile 1", "Profile 2", "Profile 3", nullptr,
    };
    U.profile_dropdown = gtk_drop_down_new_from_strings(PROFILE_NAMES);
    g_signal_connect(U.profile_dropdown, "notify::selected",
                     G_CALLBACK(on_profile_changed), nullptr);
    gtk_box_append(GTK_BOX(top), U.profile_dropdown);

    GtkWidget *write_label = gtk_label_new("Permitir gravar");
    gtk_widget_add_css_class(write_label, "dim-label");
    gtk_box_append(GTK_BOX(top), write_label);

    U.write_permission_switch = gtk_switch_new();
    gtk_widget_set_tooltip_text(U.write_permission_switch,
                                "Permitir gravar no controle (Sync to device)");
    gtk_box_append(GTK_BOX(top), U.write_permission_switch);

    U.sync_button = gtk_button_new_with_label("⟳  Sync to device");
    gtk_widget_add_css_class(U.sync_button, "suggested-action");
    g_signal_connect_swapped(U.sync_button, "clicked", G_CALLBACK(on_write), nullptr);
    gtk_box_append(GTK_BOX(top), U.sync_button);

    U.read_button = gtk_button_new_with_label("↺  Reset");
    g_signal_connect_swapped(U.read_button, "clicked", G_CALLBACK(on_read), nullptr);
    gtk_box_append(GTK_BOX(top), U.read_button);
    return top;
}

static GtkWidget *build_body(void)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, true);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), true);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), true);
    gtk_paned_set_position(GTK_PANED(paned), PANED_INITIAL_POSITION);

    GtkWidget *info_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(info_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(info_scroller),
                                  build_info_panel());
    gtk_widget_set_size_request(info_scroller, 300, 200);
    gtk_widget_set_hexpand(info_scroller, true);
    gtk_widget_set_vexpand(info_scroller, true);
    gtk_paned_set_start_child(GTK_PANED(paned), info_scroller);

    GtkWidget *pages_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pages_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pages_scroller), U.stack);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(pages_scroller), false);
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(pages_scroller), false);
    gtk_widget_set_vexpand(pages_scroller, true);
    gtk_paned_set_end_child(GTK_PANED(paned), pages_scroller);
    U.body_paned = paned;
    return paned;
}

static GtkWidget *build_footer(void)
{
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(footer, 6);
    gtk_widget_set_margin_bottom(footer, 6);
    gtk_widget_set_margin_start(footer, 12);
    gtk_widget_set_margin_end(footer, 12);

    U.device_combo = gtk_drop_down_new_from_strings((const char *const[]){
        "(procurando...)", nullptr});
    gtk_widget_set_size_request(U.device_combo, 230, -1);
    g_signal_connect(U.device_combo, "notify::selected",
                     G_CALLBACK(on_device_changed), nullptr);
    gtk_box_append(GTK_BOX(footer), U.device_combo);

    GtkWidget *scan_button = gtk_button_new_with_label("Procurar");
    g_signal_connect_swapped(scan_button, "clicked",
                             G_CALLBACK(on_reload_devices), nullptr);
    gtk_box_append(GTK_BOX(footer), scan_button);

    U.status_label = gtk_label_new(
        "pronto — conecte o controle por USB e clique em Procurar");
    gtk_widget_set_halign(U.status_label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(U.status_label, true);
    gtk_widget_set_margin_start(U.status_label, 8);
    gtk_box_append(GTK_BOX(footer), U.status_label);
    return footer;
}

static void on_activate(GtkApplication *app, gpointer data [[maybe_unused]])
{
    auto window = gtk_application_window_new(app);
    U.window = window;
    gtk_window_set_title(GTK_WINDOW(window), "8BitDo Ultimate Software — nativo");
    gtk_window_set_default_size(GTK_WINDOW(window), DEFAULT_WINDOW_WIDTH,
                                DEFAULT_WINDOW_HEIGHT);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".bd-tabs button { padding: 4px 10px; min-height: 0; }\n"
        ".bd-tabs button label { font-size: 12px; }\n"
        ".bd-tabs { border: none; }\n"
        ".bd-panel-title { font-size: 22px; font-weight: bold; }\n"
        ".bd-section { font-weight: bold; }\n"
        ".bd-tab-name { font-weight: bold; }\n");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), root);

    U.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(U.stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_titled(GTK_STACK(U.stack), page_mapping(), "mapping", "Mapping");
    gtk_stack_add_titled(GTK_STACK(U.stack), page_sticks(), "sticks", "Sticks");
    gtk_stack_add_titled(GTK_STACK(U.stack), page_triggers(), "triggers", "Triggers");
    gtk_stack_add_titled(GTK_STACK(U.stack), page_vibration(), "vibration", "Vibration");
    gtk_stack_add_titled(GTK_STACK(U.stack), page_macro(), "macro", "Macro");
    gtk_stack_add_titled(GTK_STACK(U.stack), page_profile(), "profile", "Perfil");

    gtk_box_append(GTK_BOX(root), build_top_bar());
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), build_body());
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), build_footer());

    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), nullptr);
    gtk_widget_add_controller(window, key_controller);

    gtk_window_present(GTK_WINDOW(window));
    g_timeout_add(LAYOUT_POLL_INTERVAL_MS, adapt_layout_cb, U.body_paned);
    if (g_getenv("BD_LOOP_TEST"))
        g_timeout_add(LOOP_PROBE_INTERVAL_MS, probe_tick, app);

    on_reload_devices();
    if (g_auto_read) g_timeout_add(AUTO_READ_DELAY_MS, auto_read_cb, nullptr);
}

/* ------------------------------------------------------------- self-test */

/* Prova o caminho do APP: lê, grava o arquivo, relê e compara.
 * Campos gerenciados pelo device são ignorados na comparação:
 *   0x00C..0x00D (crc/contador) e 0x012 (sentinela de slot). */
static int write_selftest(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        fprintf(stderr, "não abri %s: %s\n", path, strerror(errno));
        return 1;
    }
    uint8_t wanted[BD_REC_LEN];
    size_t read_size = fread(wanted, 1, sizeof wanted, file);
    fclose(file);
    if (read_size != BD_REC_LEN) {
        fprintf(stderr, "arquivo com %zu bytes; esperado %d\n", read_size, BD_REC_LEN);
        return 1;
    }

    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count <= 0) {
        fprintf(stderr, "nenhum 8BitDo HID encontrado\n");
        return 1;
    }

    struct bd_hid_link link = {.fd = -1};
    if (!bd_hid_open(&link, devices[0].path, devices[0].pid)) {
        fprintf(stderr, "não abri %s\n", devices[0].path);
        return 1;
    }
    printf("device: %s (pid 0x%04x)\n", devices[0].path, devices[0].pid);

    printf("gravando %d bytes…\n", BD_REC_LEN);
    const bool written = bd_hid_write_record(&link, wanted);
    printf("escrita: %s\n", written ? "ACK (chunks + commit)" : "FALHOU");

    int status = 1;
    if (written) {
        uint8_t back[BD_REC_LEN];
        memset(back, 0, sizeof back);
        if (bd_hid_read_record(&link, back, nullptr)) {
            const size_t differences = bd_record_diff(wanted, back, nullptr);
            printf("releitura: %zu byte(s) divergente(s) fora dos campos do device\n", differences);
            status = differences == 0 ? 0 : 1;
        } else {
            printf("releitura: falhou\n");
        }
    }

    bd_hid_close(&link);
    return status;
}

static int selftest(void)
{
    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count <= 0) {
        fprintf(stderr, "nenhum 8BitDo encontrado\n");
        return 1;
    }

    printf("device: %s (pid %04x)\n", devices[0].path, devices[0].pid);
    if (!bd_hid_open(&g_hid_link, devices[0].path, devices[0].pid)) {
        fprintf(stderr, "open: %s\n", strerror(errno));
        return 1;
    }

    memset(record, 0, sizeof record);
    if (!bd_hid_read_record(&g_hid_link, record, nullptr)) {
        fprintf(stderr, "falha na leitura\n");
        bd_hid_close(&g_hid_link);
        return 1;
    }

    printf("record: %d bytes\n", BD_REC_LEN);
    printf("  mode=%u slot=%u contador=0x%08x\n", bd_rd16(record, BD_OFF_MODE),
           bd_rd16(record, BD_OFF_SLOT), bd_rd32(record, BD_OFF_CRC));
    for (int slot = 0; slot < BD_SLOTS; slot++) {
        char *name = profile_name_dup(record, slot);
        printf("  slot %d: nome='%s'\n", slot, name);
        g_free(name);
    }

    size_t mapping_offset = BD_OFF_MAP + PROFILE_VALUE_OFFSET;
    printf("  mapeamento slot 0:");
    for (int key = 0; key < BD_NKEYS; key++)
        printf(" %s=0x%x", bd_key_short[key],
               bd_rd32(record, mapping_offset + (size_t)key * 4));
    printf("\n");
    bd_hid_close(&g_hid_link);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc > 2 && strcmp(argv[1], "--write-selftest") == 0) return write_selftest(argv[2]);

    /* filtra opções próprias: GApplication rejeita opções desconhecidas */
    char **arguments = g_new0(char *, (gsize)argc + 1);
    int argument_count = 0;
    arguments[argument_count++] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--auto-read") == 0) {
            g_auto_read = true;
            continue;
        }
        arguments[argument_count++] = argv[i];
    }

    /* BD_NON_UNIQUE permite rodar duas instâncias (usado pelo smoke test da suíte) */
    GApplicationFlags flags = g_getenv("BD_NON_UNIQUE")
                                  ? G_APPLICATION_NON_UNIQUE
                                  : G_APPLICATION_DEFAULT_FLAGS;
    auto app = gtk_application_new("org.rea.eightbdoultimate", flags);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argument_count, arguments);

    g_object_unref(app);
    bd_hid_close(&g_hid_link);
    g_free(arguments);
    return status;
}
