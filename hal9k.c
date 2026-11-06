#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#include <windows.h>

#define ID_FIRST 100
#define ID_LAST 101
#define ID_EMAIL 102
#define ID_STREET 103
#define ID_POSTCODE 104
#define ID_CITY 105
#define ID_BIRTH 106
#define ID_PHONE 107
#define ID_ORDINARY 108
#define ID_STUDENT 109
#define ID_MESSAGE 110
#define ID_CHECK 111

/* Danish strings are encoded explicitly as Windows-1252 for Windows 98. */
static HINSTANCE instance;
static HFONT font;
static HWND fields[8], status_text;

/* Classic mnemonic shortcuts: Alt+<letter> moves focus to the matching field.
   The ampersand inserted into each label produces the underlined letter. */
#define MAX_MNEMONICS 16
static struct {
    char key;
    HWND control;
    BOOL activate;   /* buttons are also clicked, as in a classic dialog */
} mnemonics[MAX_MNEMONICS];
static int mnemonic_count;

static char upper_ascii(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static void label_with_mnemonic(const char *label, char key, char *buffer, int size)
{
    int i, out = 0, placed = 0;
    for (i = 0; label[i] && out < size - 4; ++i) {
        if (!placed && upper_ascii(label[i]) == upper_ascii(key)) {
            buffer[out++] = '&';
            placed = 1;
        }
        buffer[out++] = label[i];
    }
    buffer[out] = '\0';
}

static void add_mnemonic(char key, HWND control, BOOL activate)
{
    if (mnemonic_count < MAX_MNEMONICS) {
        mnemonics[mnemonic_count].key = upper_ascii(key);
        mnemonics[mnemonic_count].control = control;
        mnemonics[mnemonic_count].activate = activate;
        ++mnemonic_count;
    }
}

static HWND add_control(HWND parent, const char *class_name, const char *text,
                        DWORD style, DWORD exstyle, int x, int y, int w, int h,
                        int id, HFONT control_font)
{
    HWND control = CreateWindowExA(exstyle, class_name, text,
        WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
        (HMENU)(INT_PTR)id, instance, NULL);
    if (control) SendMessageA(control, WM_SETFONT, (WPARAM)control_font, TRUE);
    return control;
}

static HWND add_field(HWND parent, const char *label, char mnemonic, int x, int y,
                      int width, int id, int limit)
{
    char text[64];
    HWND edit;
    label_with_mnemonic(label, mnemonic, text, sizeof(text));
    add_control(parent, "STATIC", text, 0, 0, x, y, width, 16, 0, font);
    edit = add_control(parent, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL,
        WS_EX_CLIENTEDGE, x, y + 17, width, 23, id, font);
    SendMessageA(edit, EM_SETLIMITTEXT, limit, 0);
    add_mnemonic(mnemonic, edit, FALSE);
    return edit;
}

/* The window is not a dialog, so IsDialogMessageA does not dispatch
   mnemonics; Alt+<key> arrives as WM_SYSKEYDOWN for the focused child. */
static BOOL handle_mnemonic(HWND window, const MSG *message)
{
    char key;
    int i;
    if (message->message != WM_SYSKEYDOWN) return FALSE;
    if (message->hwnd != window && !IsChild(window, message->hwnd)) return FALSE;
    if (message->wParam > 0xFF) return FALSE;
    /* Ignore AltGr combinations, which are reported as Ctrl+Alt. */
    if (GetKeyState(VK_CONTROL) < 0) return FALSE;
    key = upper_ascii((char)message->wParam);
    for (i = 0; i < mnemonic_count; ++i) {
        if (mnemonics[i].key != key) continue;
        if (mnemonics[i].activate)
            SendMessageA(mnemonics[i].control, BM_CLICK, 0, 0);
        SetFocus(mnemonics[i].control);
        return TRUE;
    }
    return FALSE;
}

static int digits(const char *s, int count)
{
    int i;
    for (i = 0; i < count; ++i)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static void check_form(void)
{
    char value[257];
    int i, has_at, has_domain;
    for (i = 0; i < 8; ++i) {
        GetWindowTextA(fields[i], value, sizeof(value));
        if (!value[0]) {
            SetWindowTextA(status_text, "Udfyld alle obligatoriske felter (*).");
            SetFocus(fields[i]);
            return;
        }
    }
    GetWindowTextA(fields[2], value, sizeof(value));
    has_at = 0;
    has_domain = 0;
    for (i = 0; value[i]; ++i) {
        if (value[i] == '@' && i > 0) has_at = 1;
        else if (has_at && value[i] != ' ') has_domain = 1;
    }
    if (!has_at || !has_domain) {
        SetWindowTextA(status_text, "Kontroller din emailadresse.");
        SetFocus(fields[2]);
        return;
    }
    GetWindowTextA(fields[4], value, sizeof(value));
    if (lstrlenA(value) != 4 || !digits(value, 4)) {
        SetWindowTextA(status_text, "Postnummer skal best\345 af fire cifre.");
        SetFocus(fields[4]);
        return;
    }
    GetWindowTextA(fields[6], value, sizeof(value));
    if (lstrlenA(value) != 10 || value[2] != '-' || value[5] != '-' ||
        !digits(value, 2) || !digits(value + 3, 2) || !digits(value + 6, 4)) {
        SetWindowTextA(status_text, "Brug formatet dd-mm-yyyy til f\370dselsdato.");
        SetFocus(fields[6]);
        return;
    }
    SetWindowTextA(status_text, "Felter kontrolleret. Offline demo: intet er sendt eller gemt.");
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        HWND control;
        char text[64];
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        add_control(window, "STATIC", "Indmeldelse - offline demo", 0, 0,
            20, 20, 550, 18, 0, font);
        add_control(window, "STATIC", "Udfyld dine oplysninger. Felter med * er obligatoriske.", 0, 0,
            20, 55, 550, 18, 0, font);

        fields[0] = add_field(window, "Fornavn(e) *", 'F', 20, 87, 270, ID_FIRST, 256);
        fields[1] = add_field(window, "Efternavn(e) *", 'E', 310, 87, 270, ID_LAST, 256);
        fields[2] = add_field(window, "Email *", 'M', 20, 130, 270, ID_EMAIL, 256);
        fields[3] = add_field(window, "Vejnavn og nr. *", 'V', 310, 130, 270, ID_STREET, 256);
        fields[4] = add_field(window, "Postnummer *", 'P', 20, 173, 270, ID_POSTCODE, 4);
        fields[5] = add_field(window, "By *", 'B', 310, 173, 270, ID_CITY, 256);
        fields[6] = add_field(window, "F\370dselsdato *  (dd-mm-yyyy)", 'D', 20, 216, 270, ID_BIRTH, 10);
        fields[7] = add_field(window, "Telefon nr. *", 'T', 310, 216, 270, ID_PHONE, 256);

        add_control(window, "STATIC", "Medlemsskab *", 0, 0, 20, 261, 200, 16, 0, font);
        label_with_mnemonic("Almindeligt medlem", 'A', text, sizeof(text));
        control = add_control(window, "BUTTON", text,
            WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 20, 279, 200, 22, ID_ORDINARY, font);
        SendMessageA(control, BM_SETCHECK, BST_CHECKED, 0);
        add_mnemonic('A', control, TRUE);
        label_with_mnemonic("Studiemedlem", 'S', text, sizeof(text));
        control = add_control(window, "BUTTON", text, WS_TABSTOP | BS_AUTORADIOBUTTON,
            0, 230, 279, 160, 22, ID_STUDENT, font);
        add_mnemonic('S', control, TRUE);
        label_with_mnemonic("Evt. besked / valgfrit", 'K', text, sizeof(text));
        add_control(window, "STATIC", text, 0, 0, 20, 305, 300, 16, 0, font);
        control = add_control(window, "EDIT", "", WS_TABSTOP | WS_GROUP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, WS_EX_CLIENTEDGE,
            20, 322, 560, 50, ID_MESSAGE, font);
        SendMessageA(control, EM_SETLIMITTEXT, 1000, 0);
        add_mnemonic('K', control, FALSE);
        add_control(window, "BUTTON", "Kontroller felter", WS_TABSTOP | BS_PUSHBUTTON,
            0, 20, 380, 140, 27, ID_CHECK, font);
        add_control(window, "STATIC", "Offline demo: sender ikke indmeldelser.", 0, 0,
            178, 386, 390, 18, 0, font);
        status_text = add_control(window, "STATIC", "Ingen oplysninger sendes eller gemmes.", 0, 0,
            20, 414, 560, 20, 0, font);
        SetFocus(fields[0]);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_CHECK && HIWORD(wparam) == BN_CLICKED) {
            check_form();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

void WINAPI WinMainCRTStartup(void)
{
    WNDCLASSA wc = {0};
    HWND window;
    MSG message;
    BOOL result;
    instance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconA(instance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HAL9KWindow";
    if (!RegisterClassA(&wc)) ExitProcess(1);
    window = CreateWindowExA(0, wc.lpszClassName, "hal9k - Indmeldelse (offline demo)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 610, 475, NULL, NULL, instance, NULL);
    if (!window) ExitProcess(1);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    while ((result = GetMessageA(&message, NULL, 0, 0)) > 0) {
        if (handle_mnemonic(window, &message)) continue;
        if (!IsDialogMessageA(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    ExitProcess(result == -1 ? 1 : (UINT)message.wParam);
}
