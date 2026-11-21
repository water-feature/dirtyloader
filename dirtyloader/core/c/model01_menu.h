/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Text MOD LOADER for Model:01. Shares core's SD functions and state. */
#define ACTIVE_PATH "/dirtyloader/active.txt"
/* Keep menu text inside the rows visible on the Model:01 panel. */
#define MENU_TITLE_ROW 5
#define MENU_FIRST_ROW 8
#define MENU_VISIBLE_MODS 10
#define MENU_STATUS_ROW (MENU_FIRST_ROW + MENU_VISIBLE_MODS)

static int read_active(char *list)
{
    struct fw_file f;
    if (!f_open(&f, ACTIVE_PATH, FW_O_READ)) return -1;
    int n = f_read(&f, list, LIST_SIZE);
    fw_file_close(&f);
    if (n < 0 || n >= (int)LIST_SIZE) return -1;
    list[n] = 0;
    return n;
}

static int active_line(const char *line, const char *end, const char *name)
{
    while (line < end && *line == ' ') line++;
    if (line < end && *line == '#') return 0;
    while (line < end && *name && *line == *name) { line++; name++; }
    return line == end && !*name;
}

enum { MENU_OK, MENU_NO_CARD, MENU_NO_FOLDER, MENU_NO_NAMES, MENU_ACTIVE_ERROR, MENU_SAVE_ERROR };

static int mod_extension(const char *ext)
{
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') &&
           ext[2] == '8' && (ext[3] == 'x' || ext[3] == 'X');
}

static int read_menu(void)
{
    struct fw_file dir, f;
    ram->page_n = 0;
    if (!FW_SD_MOUNTED()) return MENU_NO_CARD;
    if (!f_open(&dir, "/dirtyloader/mods", FW_O_READ)) return MENU_NO_FOLDER;
    unsigned entries = 0;
    while (ram->page_n < MAX_MODS) {
        zero(&f, sizeof f);
        if (!fw_file_open_next(&f, &dir, FW_O_READ)) break;
        entries++;
        char *name = ram->page[ram->page_n].name;
        int len = f.fat ? fw_fat_name(f.fat, name, NAME_MAX) :
                  f.exfat ? fw_exfat_name(f.exfat, name, NAME_MAX) : 0;
        fw_file_close(&f);
        if (len <= 4 || len >= NAME_MAX || !mod_extension(name + len - 4)) continue;
        name[len] = 0;
        ram->page[ram->page_n++].on = 0;
    }
    fw_file_close(&dir);
    if (entries && !ram->page_n) return MENU_NO_NAMES;

    char list[LIST_SIZE];
    if (read_active(list) < 0) return MENU_ACTIVE_ERROR;
    const char *line = list;
    while (*line) {
        const char *end = line;
        while (*end && *end != '\n' && *end != '\r') end++;
        for (unsigned i = 0; i < ram->page_n; i++)
            if (active_line(line, end, ram->page[i].name)) ram->page[i].on = 1;
        line = *end ? end + 1 : end;
    }
    return MENU_OK;
}

static int set_active(const char *name, int enable)
{
    char list[LIST_SIZE], updated[LIST_SIZE];
    if (read_active(list) < 0) return 0;
    const char *line = list;
    char *out = updated;
    while (*line) {
        const char *end = line;
        while (*end && *end != '\n' && *end != '\r') end++;
        int remove = active_line(line, end, name);
        if (*end == '\r') end++;
        if (*end == '\n') end++;
        if (!remove)
            for (const char *p = line; p < end; p++) *out++ = *p;
        line = end;
    }
    if (enable) {
        const char *end = name;
        while (*end) end++;
        if ((uint32_t)((out - updated) + (end - name)) + 2 >= LIST_SIZE) return 0;
        if (out != updated && out[-1] != '\n') *out++ = '\n';
        out = put(out, name);
        *out++ = '\n';
    }
    return write_file(ACTIVE_PATH, updated, (uint32_t)(out - updated));
}

static void menu_row(int row, const char *text, uint8_t attr)
{
    char line[FW_GRID_COLS + 1];
    unsigned i = 0;
    while (i < FW_GRID_COLS && text[i]) { line[i] = text[i]; i++; }
    while (i < FW_GRID_COLS) line[i++] = ' ';
    line[i] = 0;
    uint8_t *attrs = fw_grid + FW_GRID_ATTR + row * FW_GRID_COLS;
    for (unsigned col = 0; col < FW_GRID_COLS; col++) attrs[col] = attr;
    fw_text(fw_grid, 0, row, line);
}

static void model01_draw(void *view)
{
    for (int row = 0; row < FW_GRID_ROWS; row++) menu_row(row, "", '1');
    menu_row(MENU_TITLE_ROW, " MOD LOADER", '3');
    if (ram->skipped) {
        menu_row(MENU_TITLE_ROW + 1, " SAFE MODE - MODS NOT LOADED", '3');
        menu_row(MENU_TITLE_ROW + 2, " UP/DN MOVE EDIT TOGGLE OPT BACK", '1');
    } else {
        menu_row(MENU_TITLE_ROW + 1, ram->page_n ? " UP/DN MOVE  EDIT TOGGLE" : " OPT BACK", '1');
        if (ram->page_n) menu_row(MENU_TITLE_ROW + 2, " OPT BACK", '1');
    }
    if (!ram->page_n) {
        const char *message = ram->last_action == MENU_NO_CARD ? " SD CARD NOT MOUNTED" :
                              ram->last_action == MENU_NO_FOLDER ? " CANNOT OPEN /DIRTYLOADER/MODS" :
                              ram->last_action == MENU_NO_NAMES ? " NO SUPPORTED MOD FILENAMES" :
                              " NO DIRECTORY ENTRIES FOUND";
        menu_row(MENU_FIRST_ROW, message, '1');
    }
    uint32_t first = ram->page_sel >= MENU_VISIBLE_MODS ? ram->page_sel - MENU_VISIBLE_MODS + 1 : 0;
    for (uint32_t i = first; i < ram->page_n && i - first < MENU_VISIBLE_MODS; i++) {
        char line[FW_GRID_COLS + 1], *out = line;
        *out++ = i == ram->page_sel ? '>' : ' ';
        out = put(out, ram->page[i].on ? " ON  " : " OFF ");
        out = put(out, ram->page[i].name);
        *out = 0;
        menu_row(MENU_FIRST_ROW + (int)(i - first), line, '1');
    }
    if (ram->skipped) {
        const char *help = ram->guard_recovery == GUARD_CLEARED ? " RIGHT RESTART TO APPLY"
                         : ram->guard_recovery == GUARD_WRITE_ERROR ? " SD: SET DIRTYLOADER/GUARD TO GOOD"
                         : " RIGHT CLEAR SKIP";
        menu_row(MENU_STATUS_ROW, help, '3');
    }
    else if (ram->last_action == MENU_ACTIVE_ERROR) menu_row(MENU_STATUS_ROW, " CANNOT READ ACTIVE LIST", '1');
    else if (ram->last_action == MENU_SAVE_ERROR) menu_row(MENU_STATUS_ROW, " COULD NOT SAVE ACTIVE LIST", '1');
    else if (ram->page_n) menu_row(MENU_STATUS_ROW, " TAKES EFFECT ON NEXT REBOOT", '1');
}

static void model01_enter(void *view, int arg)
{
    ram->page_sel = 0;
    ram->last_action = (uint32_t)read_menu();
    fw_clear_display(fw_grid, fw_background(fw_grid));
}

static void model01_input(void *view, int action) { }
static int model01_view_flag(void *view) { return 0; }

static const void *const model01_view[] = {
    (const void *)model01_enter, (const void *)FW_VIEW_SHARED_04,
    (const void *)model01_input, (const void *)FW_VIEW_NOP,
    (const void *)model01_draw, (const void *)FW_VIEW_NOP,
    (const void *)model01_view_flag, (const void *)model01_view_flag,
};

static void model01_install_menu(void)
{
    if (fw_views[FW_VIEW_FREE]) return;
    zero(&ram->page_view, sizeof ram->page_view);
    ram->page_view.vt = model01_view;
    fw_views[FW_VIEW_FREE] = &ram->page_view;
}

void model01_menu_selection(void *selection)
{
    if (fw_ui[FW_UI_VIEW] != FW_VIEW_FREE)
        fw_draw_selection(selection);
}

void model01_menu_cursor(void *overlays)
{
    if (fw_ui[FW_UI_VIEW] == FW_VIEW_FREE)
        fw_clear_cursors(overlays);
    else
        fw_draw_cursors(overlays);
}

void model01_menu_toast(void *toast)
{
    /* This stock overlay runs after the view draws and clears its old rows
       on expiry. Keep it out of the menu; delegate on every stock view. */
    if (fw_ui[FW_UI_VIEW] != FW_VIEW_FREE)
        ((void (*)(void *))0x6004B6C1u)(toast);
}

int core_key(uint8_t *ui, int action)
{
    enum { EDIT = 0x01, OPT = 0x02, RIGHT = 0x04, DOWN = 0x20, UP = 0x40, OPEN = 0xE4 };
    if (fw_views[FW_VIEW_FREE] != &ram->page_view) return 0;
    int in_menu = ui[FW_UI_VIEW] == FW_VIEW_FREE;
    if (!in_menu) {
        if (action != OPEN || ui[0]) return 0;
        ram->prev_view = ui[FW_UI_VIEW];
        fw_set_view(ui, FW_VIEW_FREE);
    } else if (action == OPT || action == OPEN) {
        fw_set_view(ui, (int)ram->prev_view);
    } else if (action == RIGHT && ram->skipped) {
        guard_recover();
    } else if (action == UP && ram->page_sel) {
        ram->page_sel--;
    } else if (action == DOWN && ram->page_sel + 1 < ram->page_n) {
        ram->page_sel++;
    } else if (action == EDIT && ram->page_sel < ram->page_n && ram->last_action != MENU_ACTIVE_ERROR) {
        unsigned i = ram->page_sel;
        int on = !ram->page[i].on;
        ram->last_action = set_active(ram->page[i].name, on) ? MENU_OK : MENU_SAVE_ERROR;
        if (!ram->last_action) ram->page[i].on = (uint8_t)on;
    }
    *(uint32_t *)(ui + 8) = fw_millis;  /* the stock router updates the same idle timer */
    return 1;
}
