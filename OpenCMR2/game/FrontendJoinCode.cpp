#include "FrontendJoinCode.h"
#include "port/sys.h"
#include "port/net.h"
#include "net/join_code.h"
#include "FrontendMenus.h"
#include "FrontendDraw.h"
#include "Frontend.h"
#include "GameInfo.h"
#include "Graphics.h"
#include "Font.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

extern int g_selectedNetworkSession;
extern BYTE g_unk0x00818ef4;
extern BYTE g_unk0x00819014;
extern BYTE g_unk0x00819024;
extern NetGuid g_unk0x00818cf0;
extern char g_sessionPassword[20];
extern char g_strTextCursor[];
extern char g_strLabelSpacedText[];
extern Menu *g_menuNextAction;

int Network_CopyEnumeratedSessionGUID(BYTE index, NetGuid *pOut);
unsigned int Network_GetEnumeratedSessionCount(void);
BOOL Network_GetSessionStateFlag(void);
void Input_ClearCharacterQueue(void);
bool Input_PopQueuedCharacter(int *pOut);

enum JoinCodeText {
    JOINCODE_TEXT_TITLE,
    JOINCODE_TEXT_PROMPT,
    JOINCODE_TEXT_FIELD,
    JOINCODE_TEXT_SEARCHING,
    JOINCODE_TEXT_DIRECT,
    JOINCODE_TEXT_RELAY,
    JOINCODE_TEXT_NOT_FOUND,
    JOINCODE_TEXT_RATE_LIMITED,
    JOINCODE_TEXT_LOBBY_UNREACHABLE,
    JOINCODE_TEXT_HOST_UNREACHABLE,
    JOINCODE_TEXT_NO_LOBBY,
    JOINCODE_TEXT_INVALID,
    JOINCODE_TEXT_UNAVAILABLE,
    JOINCODE_TEXT_COPIED,
    JOINCODE_TEXT_REGISTERING,
    JOINCODE_TEXT_SERVER_UNREACHABLE,
    JOINCODE_TEXT_COUNT,
    JOINCODE_TEXT_NONE = JOINCODE_TEXT_COUNT
};

static const char *const g_joinCodeText[6][JOINCODE_TEXT_COUNT] = {
    { "Join with code", "Enter the game code", "Game code", "Looking for the game...", "Connecting directly...",
      "Connecting via relay server...", "No game with this code", "Too many attempts, wait a moment",
      "Cannot reach the code server", "Cannot connect to the host",
      "No code server configured:\nset [net] lobby in opencmr2.ini", "Invalid code",
      "The game is full or unavailable", "(copied to clipboard)", "registering...", "code server unreachable" },
    { "Rejoindre par code", "Entrez le code de la partie", "Code de partie", "Recherche de la partie...",
      "Connexion directe...", "Connexion via le serveur relais...", "Aucune partie avec ce code",
      "Trop de tentatives, patientez un instant", "Serveur de codes injoignable",
      "Impossible de se connecter \xe0 l'h\xf4te",
      "Aucun serveur de codes :\nr\xe9glez [net] lobby dans opencmr2.ini", "Code invalide",
      "La partie est pleine ou indisponible", "(copi\xe9 dans le presse-papiers)", "enregistrement...",
      "serveur de codes injoignable" },
    { "Unirse con c\xf3" "digo", "Introduce el c\xf3" "digo de la partida", "C\xf3" "digo de partida",
      "Buscando la partida...", "Conectando directamente...", "Conectando mediante servidor de retransmisi\xf3n...",
      "No hay ninguna partida con este c\xf3" "digo", "Demasiados intentos, espera un momento",
      "No se puede contactar con el servidor de c\xf3" "digos", "No se puede conectar con el anfitri\xf3n",
      "Sin servidor de c\xf3" "digos:\nconfigura [net] lobby en opencmr2.ini", "C\xf3" "digo no v\xe1lido",
      "La partida est\xe1 llena o no disponible", "(copiado al portapapeles)", "registrando...",
      "servidor de c\xf3" "digos inaccesible" },
    { "Partecipa con codice", "Inserisci il codice della partita", "Codice partita", "Ricerca della partita...",
      "Connessione diretta...", "Connessione tramite server relay...", "Nessuna partita con questo codice",
      "Troppi tentativi, attendi un momento", "Impossibile raggiungere il server dei codici",
      "Impossibile connettersi all'host",
      "Nessun server dei codici:\nimposta [net] lobby in opencmr2.ini", "Codice non valido",
      "La partita \xe8 piena o non disponibile", "(copiato negli appunti)", "registrazione...",
      "server dei codici irraggiungibile" },
    { "Mit Code beitreten", "Spielcode eingeben", "Spielcode", "Suche nach dem Spiel...", "Direkte Verbindung...",
      "Verbindung \xfc" "ber Relay-Server...", "Kein Spiel mit diesem Code", "Zu viele Versuche, bitte kurz warten",
      "Code-Server nicht erreichbar", "Keine Verbindung zum Host",
      "Kein Code-Server:\n[net] lobby in opencmr2.ini setzen", "Ung\xfcltiger Code",
      "Das Spiel ist voll oder nicht verf\xfcgbar", "(in die Zwischenablage kopiert)", "Registrierung...",
      "Code-Server nicht erreichbar" },
    { "Do\xb1\xa3" "cz kodem", "Podaj kod gry", "Kod gry", "Szukanie gry...",
      "|\xa3" "czenie bezpo\xbdrednie...", "|\xa3" "czenie przez serwer po\xbdrednicz\xa3" "cy...",
      "Nie znaleziono gry o tym kodzie", "Za du\xd7o pr\xf3" "b, odczekaj chwil\xae",
      "Brak po\xb1\xa3" "czenia z serwerem kod\xf3w", "Nie mo\xd7na po\xb1\xa3" "czy\xa9 si\xae z gospodarzem",
      "Brak serwera kod\xf3w:\nustaw [net] lobby w opencmr2.ini", "Nieprawid\xb1owy kod",
      "Gra jest pe\xb1na lub niedost\xaepna", "(skopiowano do schowka)", "rejestrowanie...",
      "serwer kod\xf3w nieosi\xa3galny" },
};

static const DWORD kJoinCodeUnavailableMs = 2000;
static const int kJoinCodeLength = NET_CODE_TEXT_SIZE - 1;

Menu g_joinCodeMenu;
static char s_joinCode[NET_CODE_TEXT_SIZE];
static int s_joinCodeMessage = JOINCODE_TEXT_NONE;
static BOOL s_joinCodeSearching;
static BOOL s_joinCodeWaiting;
static DWORD s_joinCodeFoundAt;
static char s_hostCode[NET_CODE_TEXT_SIZE];
static BOOL s_hostCodeCopied;
static int s_pasteSeen;
static int s_pasteInjected;
static int s_pasteDiscard;

static char *JoinCode_Text(int text)
{
    int language = CGameInfo::GetGameRegion() == 3 ? 5 : CFrontend::m_textLanguage;
    if (language > 5)
        language = 0;
    return (char *)g_joinCodeText[language][text];
}

static int JoinCode_MessageFor(int status)
{
    switch (status) {
    case NET_CODE_IDLE:
        return JOINCODE_TEXT_UNAVAILABLE;
    case NET_CODE_NO_LOBBY:
        return JOINCODE_TEXT_NO_LOBBY;
    case NET_CODE_INVALID:
        return JOINCODE_TEXT_INVALID;
    case NET_CODE_CONTACTING:
        return JOINCODE_TEXT_SEARCHING;
    case NET_CODE_CONNECTING:
        return JOINCODE_TEXT_DIRECT;
    case NET_CODE_RELAYING:
        return JOINCODE_TEXT_RELAY;
    case NET_CODE_NOT_FOUND:
        return JOINCODE_TEXT_NOT_FOUND;
    case NET_CODE_LOBBY_UNREACHABLE:
        return JOINCODE_TEXT_LOBBY_UNREACHABLE;
    case NET_CODE_HOST_UNREACHABLE:
        return JOINCODE_TEXT_HOST_UNREACHABLE;
    case NET_CODE_RATE_LIMITED:
        return JOINCODE_TEXT_RATE_LIMITED;
    }
    return JOINCODE_TEXT_NONE;
}

static void JoinCode_StopSearch(int message)
{
    if (s_joinCodeSearching)
        Net_CancelFind();
    s_joinCodeSearching = FALSE;
    s_joinCodeWaiting = FALSE;
    s_joinCodeMessage = message;
}

static void JoinCode_Edited(void)
{
    JoinCode_StopSearch(JOINCODE_TEXT_NONE);
}

static void JoinCode_Type(int key)
{
    size_t len = strlen(s_joinCode);
    int value;

    if (key == 8) {
        if (len == 0)
            return;
        s_joinCode[len - 1] = 0;
        JoinCode_Edited();
        Menu_PlaySoundId(2);
        return;
    }
    if (key == '-') {
        if (len == 0 || len >= (size_t)kJoinCodeLength || s_joinCode[len - 1] == '-')
            return;
        s_joinCode[len] = '-';
        s_joinCode[len + 1] = 0;
        JoinCode_Edited();
        Menu_PlaySoundId(1);
        return;
    }
    if (key <= ' ' || key > 0x7e)
        return;
    value = net::joincode::SymbolValue((char)key);
    if (value < 0)
        return;
    if (len == net::lobby::kCodeLength / 2 && strchr(s_joinCode, '-') == NULL && len + 1 < (size_t)kJoinCodeLength)
        s_joinCode[len++] = '-';
    if (len >= (size_t)kJoinCodeLength) {
        s_joinCode[len] = 0;
        return;
    }
    s_joinCode[len] = net::joincode::kAlphabet[value];
    s_joinCode[len + 1] = 0;
    JoinCode_Edited();
    Menu_PlaySoundId(1);
}

static BOOL JoinCode_Normalize(const char *text, char *out)
{
    net::lobby::Code code;
    char trimmed[net::joincode::kMaxInput + 1];
    size_t start = 0;
    size_t len;

    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')
        start++;
    len = strlen(text + start);
    if (len > net::joincode::kMaxInput)
        return FALSE;
    memcpy(trimmed, text + start, len + 1);
    while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t' || trimmed[len - 1] == '\r' ||
                       trimmed[len - 1] == '\n'))
        trimmed[--len] = 0;
    if (!net::joincode::Parse(trimmed, code))
        return FALSE;
    std::string formatted = net::joincode::Format(code);
    snprintf(out, NET_CODE_TEXT_SIZE, "%s", formatted.c_str());
    return TRUE;
}

static BOOL JoinCode_IsDelimiter(char c)
{
    return c != 0 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || strchr(":;,.!?()[]\"'", c) != NULL);
}

static BOOL JoinCode_Store(const std::string &text, char *out)
{
    net::lobby::Code code;

    if (!net::joincode::Parse(text.c_str(), code))
        return FALSE;
    snprintf(out, NET_CODE_TEXT_SIZE, "%s", net::joincode::Format(code).c_str());
    return TRUE;
}

static BOOL JoinCode_Extract(const char *text, char *out)
{
    std::vector<std::string> tokens;
    std::string token;
    std::string stripped;
    size_t i;

    for (i = 0; text[i] != 0; i++) {
        if (JoinCode_IsDelimiter(text[i])) {
            if (!token.empty())
                tokens.push_back(token);
            token.clear();
        } else {
            token += text[i];
            stripped += text[i];
        }
    }
    if (!token.empty())
        tokens.push_back(token);
    for (i = tokens.size(); i > 0; i--) {
        if (JoinCode_Store(tokens[i - 1], out))
            return TRUE;
    }
    for (i = tokens.size(); i > 1; i--) {
        if (JoinCode_Store(tokens[i - 2] + " " + tokens[i - 1], out))
            return TRUE;
    }
    return JoinCode_Store(stripped, out);
}

static void JoinCode_Paste(void)
{
    char clipboard[256];
    char code[NET_CODE_TEXT_SIZE];

    if (Sys_GetClipboardText(clipboard, sizeof(clipboard)) > 0 && JoinCode_Extract(clipboard, code)) {
        strcpy(s_joinCode, code);
        JoinCode_Edited();
        Menu_PlaySoundId(1);
        return;
    }
    JoinCode_StopSearch(JOINCODE_TEXT_INVALID);
    Menu_PlaySoundId(3);
}

static void JoinCode_Join(Menu *pMenu, int index, const NetGuid *pGuid)
{
    s_joinCodeSearching = FALSE;
    s_joinCodeWaiting = FALSE;
    g_selectedNetworkSession = index;
    g_unk0x00818cf0 = *pGuid;
    g_unk0x00818ef4 = 1;
    g_unk0x00819014 = 1;
    g_unk0x00819024 = 1;
    FrontendNetwork_JoinSelectedSession(FrontendMenu_GetNetworkSessionBrowser(), 0);
}

static void JoinCode_Found(Menu *pMenu, const NetGuid *pGuid, BOOL relayed)
{
    int count = (int)Network_GetEnumeratedSessionCount();
    NetGuid listed;
    DWORD now;
    int i;

    for (i = 0; i < count; i++) {
        if (Network_CopyEnumeratedSessionGUID((BYTE)i, &listed) && memcmp(&listed, pGuid, sizeof(listed)) == 0) {
            JoinCode_Join(pMenu, i, pGuid);
            return;
        }
    }
    now = Sys_GetTicks();
    if (!s_joinCodeWaiting) {
        s_joinCodeWaiting = TRUE;
        s_joinCodeFoundAt = now;
    }
    s_joinCodeMessage = relayed ? JOINCODE_TEXT_RELAY : JOINCODE_TEXT_DIRECT;
    if (now - s_joinCodeFoundAt >= kJoinCodeUnavailableMs)
        JoinCode_StopSearch(JOINCODE_TEXT_UNAVAILABLE);
}

static void JoinCode_Submit(Menu *pMenu, MenuItem *pItem)
{
    char code[NET_CODE_TEXT_SIZE];
    int status;

    if (s_joinCodeSearching)
        return;
    if (!JoinCode_Normalize(s_joinCode, code)) {
        s_joinCodeMessage = JOINCODE_TEXT_INVALID;
        Menu_PlaySoundId(3);
        return;
    }
    strcpy(s_joinCode, code);
    s_joinCodeWaiting = FALSE;
    status = Net_FindByCode(s_joinCode);
    s_joinCodeMessage = JoinCode_MessageFor(status);
    if (status == NET_CODE_CONTACTING || status == NET_CODE_CONNECTING || status == NET_CODE_RELAYING ||
        status == NET_CODE_FOUND) {
        s_joinCodeSearching = TRUE;
        if (status == NET_CODE_FOUND)
            s_joinCodeMessage = JOINCODE_TEXT_SEARCHING;
        Menu_PlaySoundId(1);
    } else {
        Menu_PlaySoundId(3);
    }
}

static void JoinCode_Enter(Menu *pMenu, int param)
{
    Net_CancelFind();
    Menu_SetFlags(pMenu, 0, 0, 1, 1);
    Input_ClearCharacterQueue();
    s_joinCode[0] = 0;
    s_joinCodeSearching = FALSE;
    s_joinCodeWaiting = FALSE;
    s_joinCodeMessage = JOINCODE_TEXT_NONE;
    s_pasteSeen = Sys_GetPasteCount(&s_pasteInjected);
    s_pasteDiscard = 0;
}

static void JoinCode_Update(Menu *pMenu)
{
    NetGuid guid;
    BOOL relayed;
    int status;
    int key;
    int paste;
    int injected;

    if (g_menuNextAction != NULL)
        return;
    Sys_RequestTextInput(JoinCode_Text(JOINCODE_TEXT_FIELD), s_joinCode, kJoinCodeLength);
    paste = Sys_GetPasteCount(&injected);
    if (paste != s_pasteSeen) {
        s_pasteSeen = paste;
        s_pasteDiscard += injected - s_pasteInjected;
        s_pasteInjected = injected;
        JoinCode_Paste();
    }
    while (Input_PopQueuedCharacter(&key)) {
        if (s_pasteDiscard > 0) {
            s_pasteDiscard--;
            continue;
        }
        JoinCode_Type(key);
    }
    CGameInfo::EnumerateNetworkSessionsWithUserData(g_sessionPassword);
    if (!s_joinCodeSearching)
        return;
    memset(&guid, 0, sizeof(guid));
    relayed = FALSE;
    status = Net_GetCodeSession(&guid, &relayed);
    switch (status) {
    case NET_CODE_IDLE:
    case NET_CODE_READY:
        break;
    case NET_CODE_CONTACTING:
    case NET_CODE_CONNECTING:
    case NET_CODE_RELAYING:
        s_joinCodeMessage = JoinCode_MessageFor(status);
        break;
    case NET_CODE_FOUND:
        JoinCode_Found(pMenu, &guid, relayed);
        break;
    default:
        JoinCode_StopSearch(JoinCode_MessageFor(status));
        break;
    }
}

static void JoinCode_Leave(Menu *pMenu, int param)
{
    if (param == 0)
        return;
    Net_CancelFind();
    s_joinCodeSearching = FALSE;
    s_joinCodeWaiting = FALSE;
    s_joinCodeMessage = JOINCODE_TEXT_NONE;
}

static void JoinCode_Draw(Menu *pMenu)
{
    char *path[4];
    char text[128];
    char sample[128];
    int x;

    path[0] = CFrontend::GetTextString(0);
    path[1] = CFrontend::GetTextString(0x12);
    path[2] = CFrontend::GetTextString(0x35);
    path[3] = JoinCode_Text(JOINCODE_TEXT_TITLE);
    FrontendDraw_PlayTime();
    FrontendDraw_Breadcrumb((int)(g_pGraphics->resX * 24) / 640, (int)(g_pGraphics->resY * 38) / 480, path, 4);
    Font_DrawText(1, JoinCode_Text(JOINCODE_TEXT_PROMPT), (int)g_pGraphics->resX / 2, (int)g_pGraphics->resY / 4,
                  (int *)g_colourWhite0x00524968, 0x12);
    snprintf(sample, sizeof(sample), g_strLabelSpacedText, JoinCode_Text(JOINCODE_TEXT_FIELD), "MMMM-MMMM");
    x = (int)g_pGraphics->resX / 2 - Font_GetTextWidth(1, (BYTE *)sample) / 2;
    snprintf(text, sizeof(text), g_strLabelSpacedText, JoinCode_Text(JOINCODE_TEXT_FIELD), s_joinCode);
    if ((int)CMain::GetFrameDelta() % 20 > 9)
        strncat(text, g_strTextCursor, sizeof(text) - strlen(text) - 1);
    Font_DrawText(1, text, x, (int)g_pGraphics->resY / 2, (int *)g_colourWhite0x00524968, 0x11);
    if (s_joinCodeMessage != JOINCODE_TEXT_NONE)
        Font_DrawText(1, JoinCode_Text(s_joinCodeMessage), (int)g_pGraphics->resX / 2,
                      (int)g_pGraphics->resY / 2 + (int)(g_pGraphics->resY * 48) / 480,
                      (int *)g_colourText0x0052496c, 0x12);
    FrontendDraw_Carousel(FrontendMenu_GetMain(), 0, NULL);
}

void FrontendJoinCode_BuildMenu(void)
{
    Menu_Init(&g_joinCodeMenu, 0, 0x35, 0, FrontendMenu_GetNetworkSessionBrowser(), NULL, 1, 0, 1);
    Menu_AddItemType4(&g_joinCodeMenu, 0, -1, (MenuItemAction)JoinCode_Submit, -1);
    Menu_SetCallbacks(&g_joinCodeMenu, JoinCode_Enter, (MenuCallback)JoinCode_Update, (MenuCallback)JoinCode_Draw,
                      JoinCode_Leave);
    Menu_ValidateCursor(&g_joinCodeMenu, 0);
}

void FrontendJoinCode_Open(Menu *pMenu, MenuItem *pItem)
{
    Menu_SetNextAction(&g_joinCodeMenu);
}

char *FrontendJoinCode_BrowserLabel(void)
{
    return JoinCode_Text(JOINCODE_TEXT_TITLE);
}

void FrontendJoinCode_LabelBrowserItem(Menu *pMenu)
{
    int i;

    for (i = 0; i < pMenu->itemCount; i++) {
        if (pMenu->items[i].action == (MenuItemAction)FrontendJoinCode_Open)
            pMenu->items[i].text = FrontendJoinCode_BrowserLabel();
    }
}

void FrontendJoinCode_DrawHostCode(void)
{
    char code[NET_CODE_TEXT_SIZE];
    char text[160];
    int status;

    if (Network_GetSessionStateFlag() == 0)
        return;
    code[0] = 0;
    status = Net_GetHostCode(code, sizeof(code));
    switch (status) {
    case NET_CODE_READY:
        code[sizeof(code) - 1] = 0;
        if (code[0] == 0)
            return;
        if (strcmp(code, s_hostCode) != 0) {
            strcpy(s_hostCode, code);
            s_hostCodeCopied = Sys_SetClipboardText(code);
            Sys_Log("net: join code %s", code);
        }
        if (s_hostCodeCopied)
            snprintf(text, sizeof(text), "%s: %s %s", JoinCode_Text(JOINCODE_TEXT_FIELD), code,
                     JoinCode_Text(JOINCODE_TEXT_COPIED));
        else
            snprintf(text, sizeof(text), "%s: %s", JoinCode_Text(JOINCODE_TEXT_FIELD), code);
        break;
    case NET_CODE_CONTACTING:
        snprintf(text, sizeof(text), "%s: %s", JoinCode_Text(JOINCODE_TEXT_FIELD),
                 JoinCode_Text(JOINCODE_TEXT_REGISTERING));
        break;
    case NET_CODE_LOBBY_UNREACHABLE:
        snprintf(text, sizeof(text), "%s: %s", JoinCode_Text(JOINCODE_TEXT_FIELD),
                 JoinCode_Text(JOINCODE_TEXT_SERVER_UNREACHABLE));
        break;
    default:
        return;
    }
    Font_DrawText(1, text, (int)(g_pGraphics->resX * 99) / 640, (int)(g_pGraphics->resY * 377) / 480,
                  (int *)g_colourText0x0052496c, 0x10);
}
