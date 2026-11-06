#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <wininet.h>

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
#define ID_SEND 112

/* Posted by the send thread once the webhook request has finished. */
#define WM_APP_SEND_DONE (WM_APP + 1)

/* Danish strings are encoded explicitly as Windows-1252 for Windows 98. */
static HINSTANCE instance;
static HFONT font;
static HWND fields[8], status_text;
static HWND main_window, send_button, message_field;

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

static int check_form(void)
{
    char value[257];
    int i, has_at, has_domain;
    for (i = 0; i < 8; ++i) {
        GetWindowTextA(fields[i], value, sizeof(value));
        if (!value[0]) {
            SetWindowTextA(status_text, "Udfyld alle obligatoriske felter (*).");
            SetFocus(fields[i]);
            return 0;
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
        return 0;
    }
    GetWindowTextA(fields[4], value, sizeof(value));
    if (lstrlenA(value) != 4 || !digits(value, 4)) {
        SetWindowTextA(status_text, "Postnummer skal best\345 af fire cifre.");
        SetFocus(fields[4]);
        return 0;
    }
    GetWindowTextA(fields[6], value, sizeof(value));
    if (lstrlenA(value) != 10 || value[2] != '-' || value[5] != '-' ||
        !digits(value, 2) || !digits(value + 3, 2) || !digits(value + 6, 4)) {
        SetWindowTextA(status_text, "Brug formatet dd-mm-yyyy til f\370dselsdato.");
        SetFocus(fields[6]);
        return 0;
    }
    SetWindowTextA(status_text, "Felter kontrolleret. Klar til afsendelse.");
    return 1;
}

/* --- Slack webhook -------------------------------------------------------
   The webhook URL is a secret, so it is never compiled in: it is read from
   HAL9K.INI next to the executable. The request runs on a worker thread so
   a slow dial-up link cannot freeze the form. */

#define CONFIG_FILE "HAL9K.INI"
#define URL_MAX 512
#define PAYLOAD_MAX 16384
#define HOST_MAX 128
#define URL_PATH_MAX 512

static char webhook_url[URL_MAX];
static char payload[PAYLOAD_MAX];
static int payload_length;
static char result_text[160];

/* Windows-1252 bytes 0x80-0x9F are not Latin-1, so they need a table to
   become the right Unicode code points in the JSON payload. */
static const unsigned short cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static int append_bytes(const char *text, int count)
{
    int i;
    if (count <= 0) return 1;
    if (payload_length + count >= PAYLOAD_MAX) return 0;
    for (i = 0; i < count; ++i) payload[payload_length + i] = text[i];
    payload_length += count;
    return 1;
}

static int append_text(const char *text)
{
    return append_bytes(text, lstrlenA(text));
}

static char hex_digit(int value)
{
    return (char)(value < 10 ? '0' + value : 'A' + value - 10);
}

static int append_unicode_escape(unsigned int codepoint)
{
    char out[6];
    out[0] = '\\';
    out[1] = 'u';
    out[2] = hex_digit((int)((codepoint >> 12) & 0xF));
    out[3] = hex_digit((int)((codepoint >> 8) & 0xF));
    out[4] = hex_digit((int)((codepoint >> 4) & 0xF));
    out[5] = hex_digit((int)(codepoint & 0xF));
    return append_bytes(out, 6);
}

/* Escape for both JSON and Slack's mrkdwn, since &, < and > would otherwise
   be read as markup. */
static int append_json(const char *text)
{
    unsigned char byte;
    char one[1];
    int i;
    for (i = 0; text[i]; ++i) {
        byte = (unsigned char)text[i];
        if (byte == '&') {
            if (!append_text("&amp;")) return 0;
        } else if (byte == '<') {
            if (!append_text("&lt;")) return 0;
        } else if (byte == '>') {
            if (!append_text("&gt;")) return 0;
        } else if (byte == '"') {
            if (!append_text("\\\"")) return 0;
        } else if (byte == '\\') {
            if (!append_text("\\\\")) return 0;
        } else if (byte == '\n') {
            if (!append_text("\\n")) return 0;
        } else if (byte == '\t') {
            if (!append_text("\\t")) return 0;
        } else if (byte == '\r') {
            continue;
        } else if (byte < 0x20) {
            if (!append_unicode_escape(byte)) return 0;
        } else if (byte < 0x80) {
            one[0] = (char)byte;
            if (!append_bytes(one, 1)) return 0;
        } else if (byte < 0xA0) {
            if (!append_unicode_escape(cp1252_high[byte - 0x80])) return 0;
        } else {
            if (!append_unicode_escape(byte)) return 0;
        }
    }
    return 1;
}

static int build_payload(void)
{
    static const char *labels[8] = {
        "Fornavn(e)", "Efternavn(e)", "Email", "Vejnavn og nr.",
        "Postnummer", "By", "F\370" "dselsdato", "Telefon nr."
    };
    char value[1025];
    int i;
    payload_length = 0;
    if (!append_text("{\"text\":\"")) return 0;
    if (!append_text("*Ny indmeldelse via hal9k*\\n")) return 0;
    for (i = 0; i < 8; ++i) {
        GetWindowTextA(fields[i], value, sizeof(value));
        if (!append_text("*")) return 0;
        if (!append_json(labels[i])) return 0;
        if (!append_text(":* ")) return 0;
        if (!append_json(value)) return 0;
        if (!append_text("\\n")) return 0;
    }
    GetWindowTextA(message_field, value, sizeof(value));
    if (!append_text("*Besked:* ")) return 0;
    if (!append_json(value)) return 0;
    if (!append_text("\\n*Medlemsskab:* ")) return 0;
    if (!append_json(SendMessageA(GetDlgItem(main_window, ID_STUDENT),
            BM_GETCHECK, 0, 0) == BST_CHECKED ? "Studiemedlem" : "Almindeligt medlem"))
        return 0;
    if (!append_text("\"}")) return 0;
    payload[payload_length] = '\0';
    return 1;
}

static void set_result_number(const char *prefix, DWORD value)
{
    char digits[12];
    int count = 0, i, length;
    lstrcpynA(result_text, prefix, sizeof(result_text));
    length = lstrlenA(result_text);
    do {
        digits[count++] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value);
    for (i = count - 1; i >= 0 && length < (int)sizeof(result_text) - 1; --i)
        result_text[length++] = digits[i];
    result_text[length] = '\0';
}

static int starts_with_ci(const char *text, const char *prefix)
{
    int i;
    for (i = 0; prefix[i]; ++i) {
        char a = text[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Split the webhook URL by hand: InternetCrackUrlA is documented for
   Windows 2000 and later, and this target is Windows 98. */
static int split_url(const char *url, char *host, int host_size,
                     unsigned short *port, int *secure,
                     char *path, int path_size)
{
    const char *rest, *end, *colon, *p;
    int length, i;
    if (starts_with_ci(url, "https://")) {
        *secure = 1;
        *port = 443;
        rest = url + 8;
    } else if (starts_with_ci(url, "http://")) {
        *secure = 0;
        *port = 80;
        rest = url + 7;
    } else {
        return 0;
    }
    end = rest;
    while (*end && *end != '/' && *end != '?' && *end != '#') ++end;
    colon = NULL;
    for (p = rest; p < end; ++p)
        if (*p == ':') colon = p;
    length = (int)((colon ? colon : end) - rest);
    if (length <= 0 || length >= host_size) return 0;
    for (i = 0; i < length; ++i) host[i] = rest[i];
    host[length] = '\0';
    if (colon) {
        unsigned short value = 0;
        if (colon + 1 >= end) return 0;
        for (p = colon + 1; p < end; ++p) {
            if (*p < '0' || *p > '9') return 0;
            value = (unsigned short)(value * 10 + (*p - '0'));
        }
        if (!value) return 0;
        *port = value;
    }
    if (!*end) {
        if (path_size < 2) return 0;
        path[0] = '/';
        path[1] = '\0';
        return 1;
    }
    length = 0;
    if (*end == '?') {
        if (path_size < 2) return 0;
        path[length++] = '/';
    }
    for (p = end; *p && *p != '#'; ++p) {
        if (length >= path_size - 1) return 0;
        path[length++] = *p;
    }
    path[length] = '\0';
    return 1;
}

static int post_to_slack(void)
{
    char host[HOST_MAX], path[URL_PATH_MAX];
    unsigned short port = 0;
    int secure = 0, ok = 0;
    DWORD flags, status = 0, size = sizeof(status);
    HINTERNET session = NULL, connection = NULL, request = NULL;

    if (!split_url(webhook_url, host, sizeof(host), &port, &secure,
            path, sizeof(path))) {
        lstrcpyA(result_text, "Webhook-adressen i HAL9K.INI er ugyldig.");
        return 0;
    }
    session = InternetOpenA("hal9k-signup/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
    if (!session) {
        set_result_number("Kunne ikke starte netv\346rk. Fejl ", GetLastError());
        return 0;
    }
    connection = InternetConnectA(session, host, port, NULL, NULL,
        INTERNET_SERVICE_HTTP, 0, 0);
    if (!connection) {
        set_result_number("Kunne ikke kontakte v\346rten. Fejl ", GetLastError());
        goto done;
    }
    flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (secure) flags |= INTERNET_FLAG_SECURE;
    request = HttpOpenRequestA(connection, "POST", path, NULL, NULL, NULL,
        flags, 0);
    if (!request) {
        set_result_number("Kunne ikke oprette anmodning. Fejl ", GetLastError());
        goto done;
    }
    if (!HttpSendRequestA(request, "Content-Type: application/json\r\n", -1,
            payload, (DWORD)payload_length)) {
        set_result_number("Kunne ikke sende til Slack. Fejl ", GetLastError());
        goto done;
    }
    if (!HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &status, &size, NULL)) {
        set_result_number("Ukendt svar fra Slack. Fejl ", GetLastError());
        goto done;
    }
    if (status == 200) {
        lstrcpyA(result_text, "Indmeldelse sendt til Slack.");
        ok = 1;
    } else {
        set_result_number("Slack svarede HTTP ", status);
    }
done:
    if (request) InternetCloseHandle(request);
    if (connection) InternetCloseHandle(connection);
    InternetCloseHandle(session);
    return ok;
}

static DWORD WINAPI send_thread(void *parameter)
{
    int ok;
    (void)parameter;
    ok = post_to_slack();
    if (main_window) PostMessageA(main_window, WM_APP_SEND_DONE, (WPARAM)ok, 0);
    return 0;
}

static int load_webhook_url(void)
{
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
    int i, cut = -1, end;
    webhook_url[0] = '\0';
    if (length == 0 || length >= sizeof(path)) return 0;
    for (i = 0; path[i]; ++i)
        if (path[i] == '\\' || path[i] == '/') cut = i;
    if (cut < 0) return 0;
    path[cut + 1] = '\0';
    if (lstrlenA(path) + lstrlenA(CONFIG_FILE) >= (int)sizeof(path)) return 0;
    lstrcatA(path, CONFIG_FILE);
    GetPrivateProfileStringA("slack", "url", "", webhook_url,
        sizeof(webhook_url), path);
    for (i = 0; webhook_url[i] == ' ' || webhook_url[i] == '\t'; ++i)
        ;
    if (i) lstrcpyA(webhook_url, webhook_url + i);
    end = lstrlenA(webhook_url);
    while (end > 0 && (webhook_url[end - 1] == ' ' || webhook_url[end - 1] == '\t' ||
            webhook_url[end - 1] == '\r' || webhook_url[end - 1] == '\n'))
        webhook_url[--end] = '\0';
    return webhook_url[0] != '\0';
}

static void start_send(void)
{
    HANDLE thread;
    DWORD thread_id;
    if (!load_webhook_url()) {
        SetWindowTextA(status_text, "Ingen webhook fundet i HAL9K.INI.");
        return;
    }
    if (MessageBoxA(main_window,
            "Oplysningerne sendes til Slack. Forts\346t?",
            "Send indmeldelse", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!build_payload()) {
        SetWindowTextA(status_text, "Oplysningerne er for store til at sende.");
        return;
    }
    lstrcpyA(result_text, "Ingen afsendelse gennemf\370rt.");
    SetWindowTextA(status_text, "Sender indmeldelse ...");
    EnableWindow(send_button, FALSE);
    thread = CreateThread(NULL, 0, send_thread, NULL, 0, &thread_id);
    if (!thread) {
        EnableWindow(send_button, TRUE);
        SetWindowTextA(status_text, "Kunne ikke starte afsendelsen.");
        return;
    }
    CloseHandle(thread);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        HWND control;
        char text[64];
        main_window = window;
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
        message_field = control;
        add_control(window, "BUTTON", "Kontroller felter", WS_TABSTOP | BS_PUSHBUTTON,
            0, 20, 380, 140, 27, ID_CHECK, font);
        send_button = add_control(window, "BUTTON", "Send indmeldelse",
            WS_TABSTOP | BS_PUSHBUTTON, 0, 170, 380, 140, 27, ID_SEND, font);
        add_control(window, "STATIC", "Data sendes kun efter bekr\346ftelse.", 0, 0,
            325, 386, 260, 18, 0, font);
        status_text = add_control(window, "STATIC",
            "Ingen oplysninger sendes uden bekr\346ftelse.", 0, 0, 20, 414, 560, 20, 0, font);
        SetFocus(fields[0]);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) != BN_CLICKED) break;
        if (LOWORD(wparam) == ID_CHECK) {
            check_form();
            return 0;
        }
        if (LOWORD(wparam) == ID_SEND) {
            if (check_form()) start_send();
            return 0;
        }
        break;
    case WM_APP_SEND_DONE:
        SetWindowTextA(status_text, result_text);
        EnableWindow(send_button, TRUE);
        return 0;
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
