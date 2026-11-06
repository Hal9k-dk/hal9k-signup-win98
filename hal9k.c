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
#define ID_BACK 111
#define ID_NEXT 112
#define ID_CANCEL 113

/* Posted by the send thread once the webhook request has finished. */
#define WM_APP_SEND_DONE (WM_APP + 1)

/* Danish strings are encoded explicitly as Windows-1252 for Windows 98. */
static HINSTANCE instance;
static HFONT font;
static HWND fields[8], field_labels[8], status_text;
static HWND main_window, message_field;
static HWND ordinary_button, student_button;
static HWND page_title, page_subtitle, intro_text;
static HWND member_label, member_help, message_label;
static HWND back_button, next_button, cancel_button;
static HFONT header_font;

static void show_page(int page);
static void sync_controls(const char *status);
static int check_form(void);
static void start_send(void);

/* The form is a small state machine: fields are editable only while
   editing; a send disables everything, a failure returns to editing, and a
   successful send stays disabled because there is nothing left to resend. */
#define STATE_EDITING 0
#define STATE_SENDING 1
#define STATE_SENT 2
static int form_state = STATE_EDITING;

/* Classic wizard: one page of controls is visible at a time. */
#define PAGE_INTRO 1
#define PAGE_PERSONAL 2
#define PAGE_MEMBER 3
#define PAGE_MESSAGE 4
static int current_page = PAGE_INTRO;

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
                      int width, int id, int limit, HWND *label_out)
{
    char text[64];
    HWND edit;
    label_with_mnemonic(label, mnemonic, text, sizeof(text));
    *label_out = add_control(parent, "STATIC", text, 0, 0, x, y, width, 16, 0, font);
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
    if (form_state != STATE_EDITING) return FALSE;
    if (message->hwnd != window && !IsChild(window, message->hwnd)) return FALSE;
    if (message->wParam > 0xFF) return FALSE;
    /* Ignore AltGr combinations, which are reported as Ctrl+Alt. */
    if (GetKeyState(VK_CONTROL) < 0) return FALSE;
    key = upper_ascii((char)message->wParam);
    for (i = 0; i < mnemonic_count; ++i) {
        if (mnemonics[i].key != key) continue;
        if (!IsWindowVisible(mnemonics[i].control)) continue;
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
            show_page(PAGE_PERSONAL);
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
        show_page(PAGE_PERSONAL);
        SetWindowTextA(status_text, "Kontroller din emailadresse.");
        SetFocus(fields[2]);
        return 0;
    }
    GetWindowTextA(fields[4], value, sizeof(value));
    if (lstrlenA(value) != 4 || !digits(value, 4)) {
        show_page(PAGE_PERSONAL);
        SetWindowTextA(status_text, "Postnummer skal best\345 af fire cifre.");
        SetFocus(fields[4]);
        return 0;
    }
    GetWindowTextA(fields[6], value, sizeof(value));
    if (lstrlenA(value) != 10 || value[2] != '-' || value[5] != '-' ||
        !digits(value, 2) || !digits(value + 3, 2) || !digits(value + 6, 4)) {
        show_page(PAGE_PERSONAL);
        SetWindowTextA(status_text, "Brug formatet dd-mm-yyyy til f\370dselsdato.");
        SetFocus(fields[6]);
        return 0;
    }
    return 1;
}

/* WinMainCRTStartup has no argv, so the command line is read directly.
   --test-data (also -test-data, /test-data, --testdata) prefills the form. */
static int option_matches(const char *start, int length, const char *name)
{
    int skip = 0, i, name_length = lstrlenA(name);
    if (length > 0 && (start[0] == '-' || start[0] == '/')) skip = 1;
    if (length > skip && start[skip] == '-') ++skip;
    if (length - skip != name_length) return 0;
    for (i = 0; i < name_length; ++i) {
        char c = start[skip + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != name[i]) return 0;
    }
    return 1;
}

static int has_test_data_option(void)
{
    const char *p = GetCommandLineA();
    const char *start;
    int length;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') ++p;
        if (*p == '"') ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') {
            ++p;
            start = p;
            while (*p && *p != '"') ++p;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
        }
        length = (int)(p - start);
        if (option_matches(start, length, "test-data") ||
            option_matches(start, length, "testdata")) return 1;
        if (*p == '"') ++p;
    }
    return 0;
}

static void format_time(char *out, int size)
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    out[0] = (char)('0' + (now.wHour / 10) % 10);
    out[1] = (char)('0' + now.wHour % 10);
    out[2] = ':';
    out[3] = (char)('0' + (now.wMinute / 10) % 10);
    out[4] = (char)('0' + now.wMinute % 10);
    out[5] = '\0';
    (void)size;
}

static const char *page_titles[4] = {
    "Velkommen til hal9k",
    "Personlige oplysninger",
    "Medlemskab",
    "Valgfri besked"
};

static const char *page_subtitles[4] = {
    "Indmeldelsen best\345r af fire korte trin. Intet sendes, f\370r du bekr\346fter p\345 den sidste side.",
    "Udfyld navn, adresse og kontaktoplysninger. Felter med * er obligatoriske.",
    "V\346lg den type medlemsskab, du \370nsker.",
    "Beskeden er valgfri. N\345r du trykker Send indmeldelse, sendes oplysningerne til foreningen efter din bekr\346ftelse."
};

/* Everything that depends on state and page lives here, so the two can never
   disagree: a control is usable only while editing and only on its page. */
static void sync_controls(const char *status)
{
    BOOL editing = (form_state == STATE_EDITING);
    int i;
    for (i = 0; i < 8; ++i) EnableWindow(fields[i], editing);
    EnableWindow(ordinary_button, editing);
    EnableWindow(student_button, editing);
    EnableWindow(message_field, editing);
    EnableWindow(back_button, editing && current_page > PAGE_INTRO);
    EnableWindow(next_button, editing);
    /* Cancel stays available while sending so a stuck request cannot trap
       the user; after success it becomes the way to close the window. */
    EnableWindow(cancel_button, form_state != STATE_SENDING);
    SetWindowTextA(next_button, form_state == STATE_SENDING ? "Sender ..." :
        form_state == STATE_SENT ? "Sendt" :
        current_page == PAGE_MESSAGE ? "Send indmeldelse" : "N\346ste >");
    SetWindowTextA(cancel_button, form_state == STATE_SENT ? "Luk" : "Annuller");
    SetWindowTextA(main_window, form_state == STATE_SENDING ? "hal9k - Indmeldelse (sender)" :
        form_state == STATE_SENT ? "hal9k - Indmeldelse (sendt)" : "hal9k - Indmeldelse");
    if (status) SetWindowTextA(status_text, status);
}

static void apply_state(int state, const char *status)
{
    HWND focus = GetFocus();
    form_state = state;
    if (state != STATE_EDITING && focus &&
            (focus == main_window || IsChild(main_window, focus)))
        SetFocus(main_window);
    sync_controls(status);
}

static void show_page(int page)
{
    int i;
    current_page = page;
    ShowWindow(intro_text, page == PAGE_INTRO ? SW_SHOW : SW_HIDE);
    for (i = 0; i < 8; ++i) {
        ShowWindow(field_labels[i], page == PAGE_PERSONAL ? SW_SHOW : SW_HIDE);
        ShowWindow(fields[i], page == PAGE_PERSONAL ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(member_label, page == PAGE_MEMBER ? SW_SHOW : SW_HIDE);
    ShowWindow(member_help, page == PAGE_MEMBER ? SW_SHOW : SW_HIDE);
    ShowWindow(ordinary_button, page == PAGE_MEMBER ? SW_SHOW : SW_HIDE);
    ShowWindow(student_button, page == PAGE_MEMBER ? SW_SHOW : SW_HIDE);
    ShowWindow(message_label, page == PAGE_MESSAGE ? SW_SHOW : SW_HIDE);
    ShowWindow(message_field, page == PAGE_MESSAGE ? SW_SHOW : SW_HIDE);
    SetWindowTextA(page_title, page_titles[page - 1]);
    SetWindowTextA(page_subtitle, page_subtitles[page - 1]);
    sync_controls(NULL);
}

static void focus_for_page(void)
{
    if (current_page == PAGE_PERSONAL) SetFocus(fields[0]);
    else if (current_page == PAGE_MEMBER) SetFocus(ordinary_button);
    else if (current_page == PAGE_MESSAGE) SetFocus(message_field);
    else SetFocus(next_button);
}

static void go_next(void)
{
    if (form_state != STATE_EDITING) return;
    if (current_page == PAGE_PERSONAL && !check_form()) return;
    if (current_page < PAGE_MESSAGE) {
        show_page(current_page + 1);
        SetWindowTextA(status_text, "");
        focus_for_page();
        return;
    }
    /* Last page: validate again in case an earlier page was edited, and stop
       if validation pulled the wizard back to the personal details. */
    if (!check_form() || current_page != PAGE_MESSAGE) return;
    start_send();
}

static void go_back(void)
{
    if (form_state != STATE_EDITING || current_page == PAGE_INTRO) return;
    show_page(current_page - 1);
    SetWindowTextA(status_text, "");
    focus_for_page();
}

/* Sample data for the --test-data option: valid input that can be submitted
   unchanged, including Danish characters to exercise the payload encoding. */
/* Sample data for the --test-data option: valid input that can be submitted
   unchanged, including Danish characters to exercise the payload encoding. */
static void prefill_test_data(void)
{
    static const char *values[8] = {
        "Anna", "Hansen", "anna@example.dk", "Testvej 1",
        "2100", "K\370benhavn \330", "01-02-1990", "12345678"
    };
    int i;
    for (i = 0; i < 8; ++i) SetWindowTextA(fields[i], values[i]);
    SetWindowTextA(message_field, "Testbesked med \346\370\345 fra kommandolinjen.");
    SendMessageA(ordinary_button, BM_SETCHECK, BST_CHECKED, 0);
    show_page(PAGE_PERSONAL);
    apply_state(STATE_EDITING, "Testdata indl\346st. Kontroller og send.");
    SetFocus(fields[0]);
}

/* After a successful send the form is frozen, so the dialog repeats what was
   delivered and when. */
static void show_sent_dialog(void)
{
    char first[260], last[260], email[260], time_text[8], text[1024];
    GetWindowTextA(fields[0], first, sizeof(first));
    GetWindowTextA(fields[1], last, sizeof(last));
    GetWindowTextA(fields[2], email, sizeof(email));
    format_time(time_text, sizeof(time_text));
    lstrcpyA(text, "Indmeldelsen er sendt kl. ");
    lstrcatA(text, time_text);
    lstrcatA(text, ".\n\nNavn: ");
    lstrcatA(text, first);
    lstrcatA(text, " ");
    lstrcatA(text, last);
    lstrcatA(text, "\nEmail: ");
    lstrcatA(text, email);
    lstrcatA(text, "\n\nOplysningerne kan ikke sendes igen. Luk vinduet.");
    MessageBoxA(main_window, text, "Indmeldelse sendt", MB_OK | MB_ICONINFORMATION);
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

static void read_response_body(HINTERNET request, char *out, int size)
{
    DWORD read = 0;
    int total = 0;
    out[0] = '\0';
    while (total < size - 1 &&
           InternetReadFile(request, out + total, (DWORD)(size - 1 - total), &read) &&
           read > 0)
        total += (int)read;
    out[total] = '\0';
}

/* Slack answers errors with a short token such as invalid_payload, but a
   proxy may answer with HTML, so keep only printable characters. */
static void keep_printable(char *text)
{
    int input, output = 0, spaced = 0;
    for (input = 0; text[input]; ++input) {
        unsigned char byte = (unsigned char)text[input];
        if (byte >= 0x20 && byte < 0x7F) {
            text[output++] = (char)byte;
            spaced = 0;
        } else if (!spaced && output > 0) {
            text[output++] = ' ';
            spaced = 1;
        }
    }
    while (output > 0 && text[output - 1] == ' ') --output;
    text[output] = '\0';
}

static int post_to_slack(void)
{
    char host[HOST_MAX], path[URL_PATH_MAX], body[81];
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
        set_result_number("Kunne ikke sende indmeldelsen. Fejl ", GetLastError());
        goto done;
    }
    if (!HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &status, &size, NULL)) {
        set_result_number("Ukendt svar fra serveren. Fejl ", GetLastError());
        goto done;
    }
    if (status == 200) {
        lstrcpyA(result_text, "Indmeldelse sendt.");
        ok = 1;
    } else if (status == 0) {
        lstrcpyA(result_text, "Ukendt svar fra serveren. Intet blev sendt.");
    } else {
        set_result_number("Serveren svarede HTTP ", status);
        read_response_body(request, body, sizeof(body));
        keep_printable(body);
        if (body[0]) {
            lstrcatA(result_text, ": ");
            lstrcatA(result_text, body);
        }
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
            "Oplysningerne sendes til foreningen. Forts\346t?",
            "Send indmeldelse", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!build_payload()) {
        SetWindowTextA(status_text, "Oplysningerne er for store til at sende.");
        return;
    }
    lstrcpyA(result_text, "Ingen afsendelse gennemf\370rt.");
    apply_state(STATE_SENDING, "Sender indmeldelse ...");
    thread = CreateThread(NULL, 0, send_thread, NULL, 0, &thread_id);
    if (!thread) {
        apply_state(STATE_EDITING, "Kunne ikke starte afsendelsen.");
        return;
    }
    CloseHandle(thread);
}

/* Wizard keys: Enter advances or finishes, Escape cancels. Enter inside the
   optional message still inserts a newline. */
static BOOL handle_wizard_keys(HWND window, const MSG *message)
{
    if (form_state != STATE_EDITING) return FALSE;
    if (message->message != WM_KEYDOWN) return FALSE;
    if (message->hwnd != window && !IsChild(window, message->hwnd)) return FALSE;
    if (message->wParam == VK_RETURN) {
        if (GetFocus() == message_field) return FALSE;
        go_next();
        return TRUE;
    }
    if (message->wParam == VK_ESCAPE) {
        PostMessageA(window, WM_CLOSE, 0, 0);
        return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        char text[64];
        main_window = window;
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        header_font = CreateFontA(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");
        if (!header_font) header_font = font;

        page_title = add_control(window, "STATIC", "", 0, 0, 20, 14, 560, 20, 0, header_font);
        page_subtitle = add_control(window, "STATIC", "", 0, 0, 20, 38, 560, 32, 0, font);
        add_control(window, "STATIC", "", SS_ETCHEDHORZ, 0, 7, 74, 591, 2, 0, font);

        intro_text = add_control(window, "STATIC",
            "Hal9k er Aalborgs hackerspace - et sted for nysgerrige mennesker. Her "
            "bygger, l\346rer og deler vi viden om teknik, elektronik, mekanik og "
            "software, og alle er velkomne.\n"
            "\n"
            "Byg noget. L\346r noget. G\370r det sammen.\n"
            "\n"
            "Vi m\370des hver torsdag fra kl. 19.00 p\345 Sofiendalsvej 80, "
            "9200 Aalborg SV.\n"
            "\n"
            "Denne assistent f\370rer dig gennem fire trin og tager kun et par minutter. "
            "Brug N\346ste og Tilbage for at skifte trin, og Annuller for at afslutte. "
            "Programmet kr\346ver ingen installation og kan k\370res direkte fra cd'en.\n"
            "\n"
            "N\345r du har bekr\346ftet p\345 den sidste side, sendes dine oplysninger "
            "til foreningen. L\346s overlevelsesguiden, inden du kigger forbi f\370rste "
            "gang.",
            0, 0, 20, 92, 560, 240, 0, font);

        fields[0] = add_field(window, "Fornavn(e) *", 'F', 20, 92, 270, ID_FIRST, 256, &field_labels[0]);
        fields[1] = add_field(window, "Efternavn(e) *", 'E', 310, 92, 270, ID_LAST, 256, &field_labels[1]);
        fields[2] = add_field(window, "Email *", 'M', 20, 135, 270, ID_EMAIL, 256, &field_labels[2]);
        fields[3] = add_field(window, "Vejnavn og nr. *", 'V', 310, 135, 270, ID_STREET, 256, &field_labels[3]);
        fields[4] = add_field(window, "Postnummer *", 'P', 20, 178, 270, ID_POSTCODE, 4, &field_labels[4]);
        fields[5] = add_field(window, "By *", 'B', 310, 178, 270, ID_CITY, 256, &field_labels[5]);
        fields[6] = add_field(window, "F\370dselsdato *  (dd-mm-yyyy)", 'D', 20, 221, 270, ID_BIRTH, 10, &field_labels[6]);
        fields[7] = add_field(window, "Telefon nr. *", 'T', 310, 221, 270, ID_PHONE, 256, &field_labels[7]);

        member_label = add_control(window, "STATIC", "Medlemsskab *", 0, 0, 20, 92, 200, 16, 0, font);
        label_with_mnemonic("Almindeligt medlem - 150 kr. pr. md.", 'A', text, sizeof(text));
        ordinary_button = add_control(window, "BUTTON", text,
            WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 0, 20, 114, 560, 22, ID_ORDINARY, font);
        SendMessageA(ordinary_button, BM_SETCHECK, BST_CHECKED, 0);
        add_mnemonic('A', ordinary_button, TRUE);
        label_with_mnemonic("Studiemedlem - 75 kr. pr. md.", 'S', text, sizeof(text));
        student_button = add_control(window, "BUTTON", text,
            WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 20, 140, 560, 22, ID_STUDENT, font);
        add_mnemonic('S', student_button, TRUE);
        member_help = add_control(window, "STATIC",
            "Kontingentet opkr\346ves hver m\345ned. Er du medlem af PROSA, "
            "f\345r du halv pris.\n"
            "\n"
            "Medlemsskabet giver adgang til lokalerne hele d\370gnet. Adgangskortet "
            "til d\370ren f\345r du efter introperioden, n\345r du har v\346ret med "
            "til et par klubaftener om torsdagen - se overlevelsesguiden.\n"
            "\n"
            "Du kan g\345 tilbage og \346ndre valget, indtil indmeldelsen er sendt.",
            0, 0, 20, 174, 560, 100, 0, font);

        label_with_mnemonic("Evt. besked / valgfrit", 'K', text, sizeof(text));
        message_label = add_control(window, "STATIC", text, 0, 0, 20, 92, 300, 16, 0, font);
        message_field = add_control(window, "EDIT", "", WS_TABSTOP | WS_GROUP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, WS_EX_CLIENTEDGE,
            20, 110, 560, 120, ID_MESSAGE, font);
        SendMessageA(message_field, EM_SETLIMITTEXT, 1000, 0);
        add_mnemonic('K', message_field, FALSE);

        status_text = add_control(window, "STATIC", "", 0, 0, 20, 348, 560, 30, 0, font);
        add_control(window, "STATIC", "", SS_ETCHEDHORZ, 0, 7, 384, 591, 2, 0, font);
        back_button = add_control(window, "BUTTON", "< Tilbage",
            WS_TABSTOP | BS_PUSHBUTTON, 0, 270, 392, 100, 27, ID_BACK, font);
        next_button = add_control(window, "BUTTON", "N\346ste >",
            WS_TABSTOP | BS_PUSHBUTTON, 0, 380, 392, 125, 27, ID_NEXT, font);
        cancel_button = add_control(window, "BUTTON", "Annuller",
            WS_TABSTOP | BS_PUSHBUTTON, 0, 515, 392, 75, 27, ID_CANCEL, font);

        show_page(PAGE_INTRO);
        apply_state(STATE_EDITING, "Ingen oplysninger sendes uden bekr\346ftelse.");
        if (has_test_data_option()) prefill_test_data();
        SetFocus(next_button);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) != BN_CLICKED) break;
        if (LOWORD(wparam) == ID_NEXT) {
            go_next();
            return 0;
        }
        if (LOWORD(wparam) == ID_BACK) {
            go_back();
            return 0;
        }
        if (LOWORD(wparam) == ID_CANCEL) {
            PostMessageA(window, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_APP_SEND_DONE:
        if (wparam) {
            char time_text[8], line[96];
            format_time(time_text, sizeof(time_text));
            lstrcpyA(line, "Indmeldelse sendt kl. ");
            lstrcatA(line, time_text);
            lstrcatA(line, ". Luk vinduet.");
            apply_state(STATE_SENT, line);
            show_sent_dialog();
        } else {
            char text[400];
            lstrcpyA(text, result_text);
            lstrcatA(text, "\n\nOplysningerne er stadig i formularen."
                " Kontroller HAL9K.INI og pr\370v igen.");
            apply_state(STATE_EDITING, result_text);
            MessageBoxA(main_window, text, "Afsendelsen mislykkedes",
                MB_OK | MB_ICONWARNING);
        }
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
    window = CreateWindowExA(0, wc.lpszClassName, "hal9k - Indmeldelse",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 610, 475, NULL, NULL, instance, NULL);
    if (!window) ExitProcess(1);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    while ((result = GetMessageA(&message, NULL, 0, 0)) > 0) {
        if (handle_wizard_keys(window, &message)) continue;
        if (handle_mnemonic(window, &message)) continue;
        if (!IsDialogMessageA(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    ExitProcess(result == -1 ? 1 : (UINT)message.wParam);
}
