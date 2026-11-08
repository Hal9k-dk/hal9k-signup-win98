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

/* Posted by the send thread: the step it is working on, and the result once
   the request has finished. The result carries the generation of the attempt
   it belongs to, so a late answer to an abandoned attempt is ignored. */
#define WM_APP_SEND_DONE (WM_APP + 1)
#define WM_APP_SEND_STEP (WM_APP + 2)

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

/* Receipt text returned by the signup service, shown with the send result. */
static char server_message[256];

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

/* Send bookkeeping, all on the UI thread: the attempt's generation, which of
   the five steps the worker last reported, when the attempt started, and
   whether a worker is still running. A blocked worker cannot be cancelled, so
   the watchdog may give up on its generation while it lingers. */
static int send_generation;
static int send_step;
static DWORD send_started;
static volatile LONG send_busy;

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
       the user: closing the window ends the process and with it the worker.
       After success it becomes the way to close the window. */
    EnableWindow(cancel_button, TRUE);
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
    /* The signup service answers with a receipt; the Slack webhook does not. */
    if (server_message[0]) {
        lstrcatA(text, "\n\n");
        lstrcatA(text, server_message);
    }
    lstrcatA(text, "\n\nOplysningerne kan ikke sendes igen. Luk vinduet.");
    MessageBoxA(main_window, text, "Indmeldelse sendt", MB_OK | MB_ICONINFORMATION);
}

/* --- Sending -------------------------------------------------------------
   Signups go to the signup service (POST /api/signup, the endpoint deployed
   from drillpress) when HAL9K.INI has a [signup] url. Without it the older
   direct Slack webhook is used, so an existing configuration keeps working.
   Neither URL is compiled in: it is read from HAL9K.INI next to the
   executable, and without that file nothing is transmitted. The request runs
   on a worker thread so a slow dial-up link cannot freeze the form. */

#define CONFIG_FILE "HAL9K.INI"
#define URL_MAX 512
#define PAYLOAD_MAX 16384
#define BODY_MAX 1024
#define HOST_MAX 128
#define URL_PATH_MAX 512

/* A send must always end, also on a Windows 98 machine where a broken relay,
   a dial-up line or a proxy prompt can leave a WinINet call blocked: those
   calls have no default deadline worth waiting for, and a blocked call cannot
   be cancelled from the outside. Each attempt therefore sets short timeouts
   and is watched by a timer that gives up after SEND_DEADLINE_MS. */
#define CONNECT_TIMEOUT_MS 30000
#define SEND_TIMEOUT_MS 30000
#define RECEIVE_TIMEOUT_MS 30000
#define SEND_DEADLINE_MS 120000
#define ID_TIMER_SEND 1

/* The send thread reports which call it is inside, so the status line and the
   log name the step a stuck attempt stopped at. */
#define STEP_OPENING 1
#define STEP_CONNECTING 2
#define STEP_REQUESTING 3
#define STEP_SENDING 4
#define STEP_RECEIVING 5

/* The signup service speaks the documented JSON contract; a Slack incoming
   webhook takes one mrkdwn message instead. */
#define SEND_SIGNUP_API 1
#define SEND_SLACK 2

static char endpoint_url[URL_MAX];
static char webhook_url[URL_MAX];
static int send_mode = SEND_SIGNUP_API;
static char payload[PAYLOAD_MAX];
static int payload_length;
static char result_text[640];

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

/* Escape as JSON. Slack's mrkdwn additionally reads &, < and > as markup, so
   only the Slack payload escapes those; the signup API must receive the text
   the user typed and would otherwise store the entities literally. */
static int append_json(const char *text, int slack_markup)
{
    unsigned char byte;
    char one[1];
    int i;
    for (i = 0; text[i]; ++i) {
        byte = (unsigned char)text[i];
        if (slack_markup && byte == '&') {
            if (!append_text("&amp;")) return 0;
        } else if (slack_markup && byte == '<') {
            if (!append_text("&lt;")) return 0;
        } else if (slack_markup && byte == '>') {
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

/* The v1 contract: one JSON object of strings, every property except message
   required, unknown properties rejected. Keys are part of that contract, not
   free-form labels, so they stay camelCase even though the form is Danish. */
static int build_api_payload(void)
{
    static const char *keys[8] = {
        "firstName", "lastName", "email", "street",
        "postalCode", "city", "birthDate", "phone"
    };
    char value[1025];
    int i;
    payload_length = 0;
    if (!append_text("{")) return 0;
    for (i = 0; i < 8; ++i) {
        GetWindowTextA(fields[i], value, sizeof(value));
        if (i && !append_text(",")) return 0;
        if (!append_text("\"")) return 0;
        if (!append_text(keys[i])) return 0;
        if (!append_text("\":\"")) return 0;
        if (!append_json(value, 0)) return 0;
        if (!append_text("\"")) return 0;
    }
    GetWindowTextA(message_field, value, sizeof(value));
    if (!append_text(",\"membership\":\"")) return 0;
    if (!append_text(SendMessageA(GetDlgItem(main_window, ID_STUDENT),
            BM_GETCHECK, 0, 0) == BST_CHECKED ? "student" : "ordinary")) return 0;
    if (!append_text("\",\"message\":\"")) return 0;
    if (!append_json(value, 0)) return 0;
    if (!append_text("\"}")) return 0;
    payload[payload_length] = '\0';
    return 1;
}

static int build_slack_payload(void)
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
        if (!append_json(labels[i], 1)) return 0;
        if (!append_text(":* ")) return 0;
        if (!append_json(value, 1)) return 0;
        if (!append_text("\\n")) return 0;
    }
    GetWindowTextA(message_field, value, sizeof(value));
    if (!append_text("*Besked:* ")) return 0;
    if (!append_json(value, 1)) return 0;
    if (!append_text("\\n*Medlemsskab:* ")) return 0;
    if (!append_json(SendMessageA(GetDlgItem(main_window, ID_STUDENT),
            BM_GETCHECK, 0, 0) == BST_CHECKED ? "Studiemedlem" : "Almindeligt medlem", 1))
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

/* Split the configured address by hand: InternetCrackUrlA is documented for
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

/* The signup API answers UTF-8 JSON, but the ANSI controls display
   Windows-1252, so replies are decoded into that code page for display. */
static const char *find_substring(const char *text, const char *pattern)
{
    const char *p;
    int i;
    for (p = text; *p; ++p) {
        for (i = 0; pattern[i] && p[i] == pattern[i]; ++i)
            ;
        if (!pattern[i]) return p;
    }
    return NULL;
}

static int read_hex4(const char *text, unsigned int *value)
{
    unsigned int result = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char c = text[i];
        result <<= 4;
        if (c >= '0' && c <= '9') result |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') result |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') result |= (unsigned int)(c - 'A' + 10);
        else return 0;
    }
    *value = result;
    return 1;
}

static char cp1252_from_unicode(unsigned int codepoint)
{
    int i;
    if (codepoint < 0x80) return (char)codepoint;
    if (codepoint >= 0xA0 && codepoint <= 0xFF) return (char)codepoint;
    for (i = 0; i < 32; ++i)
        if (cp1252_high[i] == codepoint) return (char)(0x80 + i);
    return '?';
}

/* Copy the body of a JSON string (the bytes after the opening quote) up to
   the closing quote, decoding \u escapes and UTF-8 along the way. */
static void decode_json_text(const char *text, char *out, int size)
{
    const char *p = text;
    unsigned int codepoint;
    int length = 0;
    while (*p && *p != '"') {
        unsigned char byte = (unsigned char)*p;
        if (byte == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': codepoint = '\n'; ++p; break;
            case 't': codepoint = '\t'; ++p; break;
            case 'r': codepoint = '\r'; ++p; break;
            case 'b': codepoint = '\b'; ++p; break;
            case 'f': codepoint = '\f'; ++p; break;
            case 'u':
                ++p;
                if (read_hex4(p, &codepoint)) p += 4;
                else codepoint = '?';
                break;
            default: codepoint = (unsigned char)*p; ++p; break;
            }
        } else if (byte < 0x80) {
            codepoint = byte;
            ++p;
        } else if ((byte & 0xE0) == 0xC0 && p[1] &&
                ((unsigned char)p[1] & 0xC0) == 0x80) {
            codepoint = ((unsigned int)(byte & 0x1F) << 6) |
                ((unsigned char)p[1] & 0x3F);
            p += 2;
        } else if ((byte & 0xF0) == 0xE0 && p[1] && p[2] &&
                ((unsigned char)p[1] & 0xC0) == 0x80 &&
                ((unsigned char)p[2] & 0xC0) == 0x80) {
            codepoint = ((unsigned int)(byte & 0x0F) << 12) |
                ((unsigned int)((unsigned char)p[1] & 0x3F) << 6) |
                ((unsigned char)p[2] & 0x3F);
            p += 3;
        } else {
            codepoint = '?';
            ++p;
        }
        if (length < size - 1) out[length++] = cp1252_from_unicode(codepoint);
    }
    out[length] = '\0';
}

/* Find "key":"value" and return the decoded value. The bodies involved are
   small and the keys are unique, so a scan is enough; no JSON parser is
   linked in this CRT-free build. */
static int find_json_text(const char *json, const char *key, char *out, int size)
{
    char pattern[64];
    const char *hit;
    out[0] = '\0';
    if (lstrlenA(key) + 3 > (int)sizeof(pattern)) return 0;
    pattern[0] = '"';
    lstrcpyA(pattern + 1, key);
    lstrcatA(pattern, "\"");
    hit = find_substring(json, pattern);
    if (!hit) return 0;
    hit += lstrlenA(pattern);
    while (*hit == ' ' || *hit == '\t' || *hit == '\r' || *hit == '\n') ++hit;
    if (*hit != ':') return 0;
    ++hit;
    while (*hit == ' ' || *hit == '\t' || *hit == '\r' || *hit == '\n') ++hit;
    if (*hit != '"') return 0;
    decode_json_text(hit + 1, out, size);
    return 1;
}

/* A 422 names the rejected fields; show them with the Danish labels the form
   uses, one per line, so the user knows what to correct. */
static void collect_field_errors(const char *body, char *out, int size)
{
    static const char *keys[10] = {
        "firstName", "lastName", "email", "street", "postalCode",
        "city", "birthDate", "phone", "membership", "message"
    };
    static const char *labels[10] = {
        "Fornavn", "Efternavn", "Email", "Vejnavn og nr.", "Postnummer",
        "By", "F\370" "dselsdato", "Telefon nr.", "Medlemsskab", "Besked"
    };
    char value[256], line[320];
    int i, length = 0;
    out[0] = '\0';
    for (i = 0; i < 10; ++i) {
        if (!find_json_text(body, keys[i], value, sizeof(value)) || !value[0])
            continue;
        lstrcpynA(line, labels[i], sizeof(line));
        lstrcatA(line, ": ");
        lstrcatA(line, value);
        if (length > 0) {
            if (length >= size - 1) break;
            out[length++] = '\n';
        }
        lstrcpynA(out + length, line, size - length);
        length += lstrlenA(out + length);
    }
    out[length < size ? length : size - 1] = '\0';
}

/* --- Diagnostics ---------------------------------------------------------
   A send runs on a worker thread and shows nothing but the status line, and
   a Windows 98 machine is awkward to debug from, so every attempt writes a
   timestamped line to HAL9K.LOG *before* each WinINet call and again when it
   returns. The last line of an interrupted log therefore names the call that
   blocked. The log records hosts, ports, timings and error codes only: never
   the signup fields, and never the configured address, whose path can hold a
   Slack webhook token. */

#define LOG_FILE "HAL9K.LOG"
#define LOG_LINE_MAX 512

static char log_path[MAX_PATH];
static HANDLE log_file = INVALID_HANDLE_VALUE;

static void close_log(void)
{
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
}

/* Prefer the folder of the executable so the log travels with the program,
   and fall back to the temp folder because a CD is read-only. */
static void open_log_file(void)
{
    char path[MAX_PATH];
    DWORD length;
    int i, cut = -1;
    log_path[0] = '\0';
    length = GetModuleFileNameA(NULL, path, sizeof(path));
    if (length > 0 && length < sizeof(path)) {
        for (i = 0; path[i]; ++i)
            if (path[i] == '\\' || path[i] == '/') cut = i;
        if (cut >= 0 && cut + 1 + (int)sizeof(LOG_FILE) < (int)sizeof(path)) {
            path[cut + 1] = '\0';
            lstrcatA(path, LOG_FILE);
            log_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (log_file != INVALID_HANDLE_VALUE) lstrcpyA(log_path, path);
        }
    }
    if (log_file == INVALID_HANDLE_VALUE) {
        char folder[MAX_PATH];
        if (!GetTempPathA(sizeof(folder), folder)) return;
        if (lstrlenA(folder) + lstrlenA(LOG_FILE) >= (int)sizeof(folder))
            return;
        lstrcatA(folder, LOG_FILE);
        log_file = CreateFileA(folder, GENERIC_WRITE, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log_file != INVALID_HANDLE_VALUE) lstrcpyA(log_path, folder);
    }
}

/* Written line by line and flushed at once, so a line survives a killed
   process: that is what makes the last line useful on a hung send. */
static void log_event(const char *format, ...)
{
    char line[LOG_LINE_MAX];
    char clock[12];
    SYSTEMTIME now;
    va_list args;
    DWORD written;
    int length;
    if (log_file == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&now);
    clock[0] = (char)('0' + (now.wHour / 10) % 10);
    clock[1] = (char)('0' + now.wHour % 10);
    clock[2] = ':';
    clock[3] = (char)('0' + (now.wMinute / 10) % 10);
    clock[4] = (char)('0' + now.wMinute % 10);
    clock[5] = ':';
    clock[6] = (char)('0' + (now.wSecond / 10) % 10);
    clock[7] = (char)('0' + now.wSecond % 10);
    clock[8] = '\0';
    line[0] = '[';
    lstrcpynA(line + 1, clock, LOG_LINE_MAX - 1);
    length = 1 + lstrlenA(clock);
    line[length++] = ']';
    line[length++] = ' ';
    line[length] = '\0';
    va_start(args, format);
    wvsprintfA(line + length, format, args);
    va_end(args);
    length = lstrlenA(line);
    if (length > LOG_LINE_MAX - 3) length = LOG_LINE_MAX - 3;
    line[length++] = '\r';
    line[length++] = '\n';
    line[length] = '\0';
    SetFilePointer(log_file, 0, NULL, FILE_END);
    WriteFile(log_file, line, (DWORD)length, &written, NULL);
    FlushFileBuffers(log_file);
}

/* WinINet reports a failure as a bare number; the name is what makes the log
   readable for whoever is looking at it later. */
#ifndef ERROR_INTERNET_SECURE_FAILURE
#define ERROR_INTERNET_SECURE_FAILURE 12175   /* schannel could not agree TLS */
#endif

static const char *wininet_error_name(DWORD error)
{
    switch (error) {
    case ERROR_INTERNET_TIMEOUT: return "timeout";
    case ERROR_INTERNET_NAME_NOT_RESOLVED: return "name not resolved";
    case ERROR_INTERNET_CANNOT_CONNECT: return "cannot connect";
    case ERROR_INTERNET_CONNECTION_ABORTED: return "connection aborted";
    case ERROR_INTERNET_CONNECTION_RESET: return "connection reset";
    case ERROR_INTERNET_OPERATION_CANCELLED: return "operation cancelled";
    case ERROR_INTERNET_SECURE_FAILURE: return "secure channel failure";
    case ERROR_INTERNET_INVALID_CA: return "unknown certificate authority";
    case ERROR_INTERNET_UNRECOGNIZED_SCHEME: return "unrecognized URL scheme";
    case ERROR_INTERNET_PROXY_SERVER_UNREACHABLE: return "proxy unreachable";
    case ERROR_INTERNET_SERVER_UNREACHABLE: return "server unreachable";
    case ERROR_INTERNET_INVALID_URL: return "invalid URL";
    case ERROR_INTERNET_INVALID_OPERATION: return "invalid operation";
    case ERROR_INTERNET_ITEM_NOT_FOUND: return "item not found";
    default: return "see the WinINet error list";
    }
}

/* Note the failing call with its error code, the time it took and, when the
   server or a proxy said anything, that text as well. */
static void log_call_failure(const char *call, DWORD error, DWORD elapsed)
{
    char server[256];
    DWORD ignored = 0, size = sizeof(server);
    server[0] = '\0';
    if (InternetGetLastResponseInfoA(&ignored, server, &size))
        keep_printable(server);
    log_event("%s failed: error %u (%s) after %u ms%s%s", call, (unsigned)error,
        wininet_error_name(error), (unsigned)elapsed,
        server[0] ? "; server replied: " : "", server);
}

/* Named so the status line and the log say where a send is, and where it
   stopped when it does not return. */
static const char *step_name(int step)
{
    switch (step) {
    case STEP_CONNECTING: return "connecting to the host";
    case STEP_REQUESTING: return "opening the HTTP request";
    case STEP_SENDING: return "sending the request and waiting for the response";
    case STEP_RECEIVING: return "reading the response";
    default: return "opening the WinINet session";
    }
}

static void report_step(int step)
{
    log_event("step: %s", step_name(step));
    if (main_window) PostMessageA(main_window, WM_APP_SEND_STEP, (WPARAM)step, 0);
}

/* Short timeouts on the session handle apply to every request made from it.
   Without them WinINet waits for its defaults, which on Windows 98 can be
   minutes per attempt and are multiplied by connection retries. */
static void set_timeouts(HINTERNET session)
{
    DWORD value;
    value = CONNECT_TIMEOUT_MS;
    if (!InternetSetOptionA(session, INTERNET_OPTION_CONNECT_TIMEOUT,
            &value, sizeof(value)))
        log_event("could not set connect timeout: error %u", (unsigned)GetLastError());
    value = SEND_TIMEOUT_MS;
    if (!InternetSetOptionA(session, INTERNET_OPTION_SEND_TIMEOUT,
            &value, sizeof(value)))
        log_event("could not set send timeout: error %u", (unsigned)GetLastError());
    value = RECEIVE_TIMEOUT_MS;
    if (!InternetSetOptionA(session, INTERNET_OPTION_RECEIVE_TIMEOUT,
            &value, sizeof(value)))
        log_event("could not set receive timeout: error %u", (unsigned)GetLastError());
    value = 0;
    if (!InternetSetOptionA(session, INTERNET_OPTION_CONNECT_RETRIES,
            &value, sizeof(value)))
        log_event("could not turn off connect retries: error %u", (unsigned)GetLastError());
    log_event("timeouts: connect %u ms, send %u ms, receive %u ms, no retries",
        (unsigned)CONNECT_TIMEOUT_MS, (unsigned)SEND_TIMEOUT_MS,
        (unsigned)RECEIVE_TIMEOUT_MS);
}

static int post_signup(void)
{
    char host[HOST_MAX], path[URL_PATH_MAX], body[BODY_MAX];
    const char *url = send_mode == SEND_SLACK ? webhook_url : endpoint_url;
    unsigned short port = 0;
    int secure = 0, ok = 0;
    DWORD flags, status = 0, size = sizeof(status);
    DWORD started = GetTickCount(), call_started;
    HINTERNET session = NULL, connection = NULL, request = NULL;

    if (!split_url(url, host, sizeof(host), &port, &secure,
            path, sizeof(path))) {
        lstrcpyA(result_text, "Adressen i HAL9K.INI er ugyldig.");
        log_event("the configured address could not be read; nothing sent");
        return 0;
    }
    /* The path is left out of the log for the webhook, where it is the
       credential; the signup service path is not a secret and is useful. */
    log_event("attempt: mode=%s host=%s port=%u secure=%d path=%s payload=%u bytes",
        send_mode == SEND_SLACK ? "slack-webhook" : "signup-api",
        host, (unsigned)port, secure,
        send_mode == SEND_SLACK ? "(redacted)" : path,
        (unsigned)payload_length);
    report_step(STEP_OPENING);
    call_started = GetTickCount();
    session = InternetOpenA("hal9k-signup/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
    if (!session) {
        log_call_failure("InternetOpenA", GetLastError(),
            GetTickCount() - call_started);
        set_result_number("Kunne ikke starte netv\346rk. Fejl ", GetLastError());
        return 0;
    }
    log_event("InternetOpenA ok after %u ms", (unsigned)(GetTickCount() - call_started));
    set_timeouts(session);
    report_step(STEP_CONNECTING);
    call_started = GetTickCount();
    connection = InternetConnectA(session, host, port, NULL, NULL,
        INTERNET_SERVICE_HTTP, 0, 0);
    if (!connection) {
        log_call_failure("InternetConnectA", GetLastError(),
            GetTickCount() - call_started);
        set_result_number("Kunne ikke kontakte v\346rten. Fejl ", GetLastError());
        goto done;
    }
    log_event("InternetConnectA ok after %u ms", (unsigned)(GetTickCount() - call_started));
    /* INTERNET_FLAG_NO_UI keeps a proxy or dial-up prompt out of the worker
       thread, where it could wait behind the form for an answer nobody sees. */
    flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_NO_UI;
    if (secure) flags |= INTERNET_FLAG_SECURE;
    report_step(STEP_REQUESTING);
    call_started = GetTickCount();
    request = HttpOpenRequestA(connection, "POST", path, NULL, NULL, NULL,
        flags, 0);
    if (!request) {
        log_call_failure("HttpOpenRequestA", GetLastError(),
            GetTickCount() - call_started);
        set_result_number("Kunne ikke oprette anmodning. Fejl ", GetLastError());
        goto done;
    }
    report_step(STEP_SENDING);
    call_started = GetTickCount();
    if (!HttpSendRequestA(request, "Content-Type: application/json\r\n", -1,
            payload, (DWORD)payload_length)) {
        log_call_failure("HttpSendRequestA", GetLastError(),
            GetTickCount() - call_started);
        set_result_number("Kunne ikke sende indmeldelsen. Fejl ", GetLastError());
        goto done;
    }
    log_event("HttpSendRequestA ok after %u ms", (unsigned)(GetTickCount() - call_started));
    report_step(STEP_RECEIVING);
    if (!HttpQueryInfoA(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &status, &size, NULL)) {
        log_call_failure("HttpQueryInfoA", GetLastError(),
            GetTickCount() - call_started);
        set_result_number("Ukendt svar fra serveren. Fejl ", GetLastError());
        goto done;
    }
    log_event("server answered HTTP %u after %u ms", (unsigned)status,
        (unsigned)(GetTickCount() - started));
    if (status == 200) {
        lstrcpyA(result_text, "Indmeldelse sendt.");
        server_message[0] = '\0';
        if (send_mode == SEND_SIGNUP_API) {
            read_response_body(request, body, sizeof(body));
            log_event("response body: %u bytes", (unsigned)lstrlenA(body));
            find_json_text(body, "message", server_message,
                sizeof(server_message));
        }
        ok = 1;
    } else if (status == 0) {
        lstrcpyA(result_text, "Ukendt svar fra serveren. Intet blev sendt.");
    } else {
        char error[192], details[384];
        set_result_number("Serveren svarede HTTP ", status);
        read_response_body(request, body, sizeof(body));
        error[0] = '\0';
        details[0] = '\0';
        if (send_mode == SEND_SIGNUP_API) {
            find_json_text(body, "error", error, sizeof(error));
            collect_field_errors(body, details, sizeof(details));
            /* A proxy in front of the service may answer with HTML instead. */
            if (!error[0]) {
                keep_printable(body);
                lstrcpynA(error, body, sizeof(error));
            }
        } else {
            keep_printable(body);
            lstrcpynA(error, body, sizeof(error));
        }
        if (error[0]) {
            lstrcatA(result_text, ": ");
            lstrcatA(result_text, error);
        }
        if (details[0]) {
            lstrcatA(result_text, "\n");
            lstrcatA(result_text, details);
        }
    }
done:
    if (request) InternetCloseHandle(request);
    if (connection) InternetCloseHandle(connection);
    InternetCloseHandle(session);
    log_event("attempt finished: ok=%d total %u ms", ok,
        (unsigned)(GetTickCount() - started));
    return ok;
}

static DWORD WINAPI send_thread(void *parameter)
{
    int ok, generation = (int)(INT_PTR)parameter;
    ok = post_signup();
    /* Post first, then stop being busy: the UI may discard this completion,
       but it must never start a new send while this thread still runs. */
    if (main_window)
        PostMessageA(main_window, WM_APP_SEND_DONE, (WPARAM)ok, (LPARAM)generation);
    InterlockedExchange(&send_busy, 0);
    return 0;
}

static void trim_url(char *text)
{
    int i, end;
    for (i = 0; text[i] == ' ' || text[i] == '\t'; ++i)
        ;
    if (i) lstrcpyA(text, text + i);
    end = lstrlenA(text);
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r' || text[end - 1] == '\n'))
        text[--end] = '\0';
}

static int load_urls(void)
{
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
    int i, cut = -1;
    endpoint_url[0] = '\0';
    webhook_url[0] = '\0';
    if (length == 0 || length >= sizeof(path)) return 0;
    for (i = 0; path[i]; ++i)
        if (path[i] == '\\' || path[i] == '/') cut = i;
    if (cut < 0) return 0;
    path[cut + 1] = '\0';
    if (lstrlenA(path) + lstrlenA(CONFIG_FILE) >= (int)sizeof(path)) return 0;
    lstrcatA(path, CONFIG_FILE);
    log_event("configuration file: %s", path);
    GetPrivateProfileStringA("signup", "url", "", endpoint_url,
        sizeof(endpoint_url), path);
    GetPrivateProfileStringA("slack", "url", "", webhook_url,
        sizeof(webhook_url), path);
    trim_url(endpoint_url);
    trim_url(webhook_url);
    if (GetFileAttributesA(path) == 0xFFFFFFFF)   /* INVALID_FILE_ATTRIBUTES */
        log_event("configuration file not found; nothing to send to");
    return endpoint_url[0] != '\0' || webhook_url[0] != '\0';
}

/* The status line is the only sign of life during a send, so it names the
   step and counts the seconds. A send that never returns is visible instead
   of looking like an idle window. */
static const char *step_label(int step)
{
    switch (step) {
    case STEP_CONNECTING: return "kontakter v\346rten";
    case STEP_REQUESTING: return "opretter anmodning";
    case STEP_SENDING: return "sender data og venter p\345 svar";
    case STEP_RECEIVING: return "l\346ser svaret";
    default: return "starter netv\346rk";
    }
}

static void update_send_status(void)
{
    char text[160];
    DWORD elapsed = GetTickCount() - send_started;
    wsprintfA(text, "Sender indmeldelse ... (%s, %u s)", step_label(send_step),
        (unsigned)(elapsed / 1000));
    SetWindowTextA(status_text, text);
}

/* The watchdog. A WinINet call that blocks cannot be cancelled from outside,
   so the form stops waiting on it: the attempt is logged with the step it
   stopped at, the fields become editable again, and the late completion is
   discarded because its generation no longer matches. */
static void abandon_send(void)
{
    char text[512];
    DWORD elapsed = GetTickCount() - send_started;
    KillTimer(main_window, ID_TIMER_SEND);
    log_event("watchdog: attempt %d got no answer after %u ms while %s; giving up",
        send_generation, (unsigned)elapsed, step_name(send_step));
    ++send_generation;
    wsprintfA(result_text, "Afsendelsen svarede ikke inden for %u sekunder.",
        (unsigned)(SEND_DEADLINE_MS / 1000));
    apply_state(STATE_EDITING, result_text);
    wsprintfA(text, "Der kom intet svar inden for %u sekunder.\n\n"
        "Sidste trin: %s.\n\nOplysningerne st\345r stadig i formularen. "
        "Kontroller HAL9K.INI og forbindelsen, og pr\370v igen.",
        (unsigned)(SEND_DEADLINE_MS / 1000), step_label(send_step));
    if (log_path[0]) {
        lstrcatA(text, "\n\nDer er skrevet en logfil:\n");
        lstrcatA(text, log_path);
    }
    MessageBoxA(main_window, text, "Afsendelsen svarede ikke",
        MB_OK | MB_ICONWARNING);
}

static void start_send(void)
{
    HANDLE thread;
    DWORD thread_id;
    int built, generation;
    if (!load_urls()) {
        SetWindowTextA(status_text,
            "Ingen adresse fundet i HAL9K.INI ([signup] url).");
        log_event("no [signup] or [slack] url next to the executable; nothing sent");
        return;
    }
    /* The signup service is the documented path; the direct Slack webhook is
       kept for existing installations and for relays that only speak Slack. */
    send_mode = endpoint_url[0] ? SEND_SIGNUP_API : SEND_SLACK;
    log_event("configuration read; mode=%s",
        send_mode == SEND_SIGNUP_API ? "signup-api" : "slack-webhook");
    /* A worker that the watchdog gave up on may still be inside a WinINet
       call. It shares the payload buffer, so a second one has to wait. */
    if (send_busy) {
        SetWindowTextA(status_text,
            "Den forrige afsendelse er stadig i gang. Vent eller luk vinduet.");
        log_event("send refused: the previous attempt has not returned yet");
        return;
    }
    if (MessageBoxA(main_window,
            "Oplysningerne sendes til foreningen. Forts\346t?",
            "Send indmeldelse", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        log_event("confirmation declined; nothing sent");
        return;
    }
    built = send_mode == SEND_SIGNUP_API ? build_api_payload() : build_slack_payload();
    if (!built) {
        SetWindowTextA(status_text, "Oplysningerne er for store til at sende.");
        log_event("payload too large for the buffer; nothing sent");
        return;
    }
    lstrcpyA(result_text, "Ingen afsendelse gennemf\370rt.");
    ++send_generation;
    generation = send_generation;
    send_started = GetTickCount();
    send_step = STEP_OPENING;
    InterlockedExchange(&send_busy, 1);
    apply_state(STATE_SENDING, "Sender indmeldelse ...");
    update_send_status();
    if (!SetTimer(main_window, ID_TIMER_SEND, 1000, NULL))
        log_event("SetTimer failed: error %u; this attempt has no watchdog",
            (unsigned)GetLastError());
    log_event("attempt %d started", generation);
    thread = CreateThread(NULL, 0, send_thread, (void *)(INT_PTR)generation, 0,
        &thread_id);
    if (!thread) {
        InterlockedExchange(&send_busy, 0);
        KillTimer(main_window, ID_TIMER_SEND);
        log_event("CreateThread failed: error %u", (unsigned)GetLastError());
        apply_state(STATE_EDITING, "Kunne ikke starte afsendelsen.");
        return;
    }
    CloseHandle(thread);
}

/* Wizard keys: Enter advances or finishes, Escape asks to close. Enter inside
   the optional message still inserts a newline. Escape also works while a
   send is running, where closing asks for confirmation first. */
static BOOL handle_wizard_keys(HWND window, const MSG *message)
{
    if (message->message != WM_KEYDOWN) return FALSE;
    if (message->hwnd != window && !IsChild(window, message->hwnd)) return FALSE;
    if (message->wParam == VK_ESCAPE) {
        PostMessageA(window, WM_CLOSE, 0, 0);
        return TRUE;
    }
    if (form_state != STATE_EDITING) return FALSE;
    if (message->wParam == VK_RETURN) {
        if (GetFocus() == message_field) return FALSE;
        go_next();
        return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        char text[64];
        OSVERSIONINFOA version;
        main_window = window;
        open_log_file();
        log_event("hal9k-signup started, built %s %s", __DATE__, __TIME__);
        if (log_path[0]) log_event("log file: %s", log_path);
        version.dwOSVersionInfoSize = sizeof(version);
        if (GetVersionExA(&version))
            log_event("Windows %u.%u.%u", (unsigned)version.dwMajorVersion,
                (unsigned)version.dwMinorVersion,
                (unsigned)version.dwBuildNumber);
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
    case WM_TIMER:
        if (wparam == ID_TIMER_SEND && form_state == STATE_SENDING) {
            if (GetTickCount() - send_started >= SEND_DEADLINE_MS)
                abandon_send();
            else
                update_send_status();
            return 0;
        }
        break;
    case WM_APP_SEND_STEP:
        if (form_state == STATE_SENDING) {
            send_step = (int)wparam;
            update_send_status();
        }
        return 0;
    case WM_APP_SEND_DONE:
        /* A completion that belongs to an attempt the watchdog abandoned is
           stale: its result is no longer what the form is waiting for. */
        if ((int)lparam != send_generation) {
            log_event("late completion for attempt %d ignored; watching attempt %d",
                (int)lparam, send_generation);
            return 0;
        }
        KillTimer(window, ID_TIMER_SEND);
        if (wparam) {
            char time_text[8], line[96];
            format_time(time_text, sizeof(time_text));
            lstrcpyA(line, "Indmeldelse sendt kl. ");
            lstrcatA(line, time_text);
            lstrcatA(line, ". Luk vinduet.");
            apply_state(STATE_SENT, line);
            show_sent_dialog();
        } else {
            char text[900];
            lstrcpyA(text, result_text);
            lstrcatA(text, "\n\nOplysningerne er stadig i formularen."
                " Kontroller HAL9K.INI og pr\370v igen.");
            apply_state(STATE_EDITING, result_text);
            MessageBoxA(main_window, text, "Afsendelsen mislykkedes",
                MB_OK | MB_ICONWARNING);
        }
        return 0;
    case WM_CLOSE:
        /* Closing ends the process, and with it a worker that is still
           blocked, so ask first while a send is running. */
        if (form_state == STATE_SENDING &&
            MessageBoxA(window, "Indmeldelsen er ikke sendt f\346rdig. Luk alligevel?",
                "Luk hal9k", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return 0;
        log_event("window closed in state %d", form_state);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        close_log();
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
