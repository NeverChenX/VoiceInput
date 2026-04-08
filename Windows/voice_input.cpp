// voice_input.cpp — Windows push-to-talk voice transcription (C++ rewrite)
// Zero external dependencies: Win32 + WinMM + WinHTTP only.
//
// HOW TO BUILD
// ─────────────────────────────────────────────────────────────────────────────
// MSVC (recommended):
//   run build.bat
//
// MinGW/MSYS2:
//   g++ -std=c++17 -O2 -o voice_input.exe voice_input.cpp \
//       -lwinmm -lwinhttp -luser32 -lgdi32 -lole32 -mwindows
//
// HOW TO EXIT
//   Press ESC at any time, or close from the taskbar.
// ─────────────────────────────────────────────────────────────────────────────

// DPI-aware manifest embedding (Per-Monitor V2, Windows 10 1703+)
#pragma comment(linker, \
    "/manifestdependency:\"type='win32' "\
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>

// Config dialog control IDs
#define IDC_EDIT_APPID    2001
#define IDC_EDIT_TOKEN    2002
#define IDC_EDIT_SECRET   2003
#define IDC_EDIT_RESOURCE 2004
#define IDC_BTN_SAVE      2005
#define IDC_BTN_CANCEL    2006
#define IDC_EDIT_HOTKEY   2007
#define IDC_EDIT_HKSTOP   2008
#define IDC_CHK_LONGPRESS 2009

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <ctime>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// ════════════════════════════════════════════════════════════════════════════
// Constants
// ════════════════════════════════════════════════════════════════════════════
// Hotkey: Alt+Space to start recording, Space to stop
static const int       SAMPLE_RATE     = 16000;
static const double    MIN_RECORD_SEC  = 0.25;

static const wchar_t*  API_HOST        = L"openspeech.bytedance.com";
static const wchar_t*  SUBMIT_PATH     = L"/api/v3/auc/bigmodel/submit";
static const wchar_t*  QUERY_PATH      = L"/api/v3/auc/bigmodel/query";
static const wchar_t*  OVERLAY_CLASS   = L"VoiceInputOverlay";
static const wchar_t*  STATUS_CLASS    = L"VoiceInputStatus";
static const wchar_t*  CONFIG_CLASS    = L"VoiceInputConfig";
static const int       STATUS_W        = 200;
static const int       STATUS_H        = 200;

// Custom window messages
enum {
    WM_APP_START  = WM_APP + 1,   // hook → main: begin recording
    WM_APP_STOP   = WM_APP + 2,   // hook → main: stop recording
    WM_APP_PASTE  = WM_APP + 3,   // worker → main: lParam = new std::wstring*
    WM_APP_ERROR  = WM_APP + 4,   // any   → main: lParam = new std::wstring*
    WM_APP_HIDE   = WM_APP + 5,   // timer → main: hide overlay
    WM_APP_TRAY   = WM_APP + 6,   // tray icon messages
};

// ════════════════════════════════════════════════════════════════════════════
// Logging — voice_input.log (info) + error.log (errors only)
// ════════════════════════════════════════════════════════════════════════════
static std::mutex g_log_mu;

static std::string timestamp() {
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return ts;
}

static void log_write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::ofstream f("voice_input.log", std::ios::app);
    f << "[" << timestamp() << "] [" << level << "] " << msg << "\n";
    f.flush();
}

static void log_info(const std::string& msg)  { log_write("INFO ", msg); }
static void log_warn(const std::string& msg)  { log_write("WARN ", msg); }
static void log_error(const std::string& msg) {
    log_write("ERROR", msg);
    // Also append to error.log for backward compat
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::ofstream f("error.log", std::ios::app);
    f << "[" << timestamp() << "] " << msg << "\n";
}

// ════════════════════════════════════════════════════════════════════════════
// Config
// ════════════════════════════════════════════════════════════════════════════
struct Config {
    std::string app_id;
    std::string access_token;
    std::string secret_key;
    std::string resource_id    = "volc.seedasr.auc";
    std::string trigger_mode   = "hotkey";   // "hotkey" | "longpress"
    std::string hotkey         = "Alt+Space";
    std::string hotkey_stop    = "Space";
    int request_timeout_ms     = 120000;
    int poll_interval_ms       = 1200;
    int poll_timeout_ms        = 45000;
};

// ════════════════════════════════════════════════════════════════════════════
// Hotkey parsing
// ════════════════════════════════════════════════════════════════════════════
struct HotkeyDef {
    DWORD vk    = VK_SPACE;
    bool  alt   = true;
    bool  ctrl  = false;
    bool  shift = false;
    bool  win   = false;
};

static std::string str_upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static std::vector<std::string> str_split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur = ""; }
        else            { cur += c; }
    }
    out.push_back(cur);
    return out;
}

static HotkeyDef parse_hotkey(const std::string& raw) {
    HotkeyDef def;
    def.alt = def.ctrl = def.shift = def.win = false;
    def.vk  = 0;

    auto parts = str_split(str_upper(raw), '+');
    for (auto& p : parts) {
        // Trim spaces
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        while (!p.empty() && p.back()  == ' ') p.pop_back();

        if (p == "ALT"   || p == "OPT")           { def.alt   = true; continue; }
        if (p == "CTRL"  || p == "CONTROL")        { def.ctrl  = true; continue; }
        if (p == "SHIFT")                           { def.shift = true; continue; }
        if (p == "WIN"   || p == "CMD")             { def.win   = true; continue; }

        // Named keys
        if (p == "SPACE")                           { def.vk = VK_SPACE;   continue; }
        if (p == "ENTER" || p == "RETURN")          { def.vk = VK_RETURN;  continue; }
        if (p == "TAB")                             { def.vk = VK_TAB;     continue; }
        if (p == "ESC"   || p == "ESCAPE")          { def.vk = VK_ESCAPE;  continue; }
        if (p == "BACK"  || p == "BACKSPACE")       { def.vk = VK_BACK;    continue; }
        if (p == "DELETE" || p == "DEL")            { def.vk = VK_DELETE;  continue; }
        if (p == "INSERT" || p == "INS")            { def.vk = VK_INSERT;  continue; }
        if (p == "HOME")                            { def.vk = VK_HOME;    continue; }
        if (p == "END")                             { def.vk = VK_END;     continue; }
        if (p == "PGUP" || p == "PAGEUP")           { def.vk = VK_PRIOR;   continue; }
        if (p == "PGDN" || p == "PAGEDOWN")         { def.vk = VK_NEXT;    continue; }
        if (p == "UP")                              { def.vk = VK_UP;      continue; }
        if (p == "DOWN")                            { def.vk = VK_DOWN;    continue; }
        if (p == "LEFT")                            { def.vk = VK_LEFT;    continue; }
        if (p == "RIGHT")                           { def.vk = VK_RIGHT;   continue; }
        // Hex VK fallback (e.g. "VK41" stored by capture control for unknown keys)
        if (p.size() == 4 && p[0]=='V' && p[1]=='K') {
            unsigned vkh = 0;
            if (sscanf(p.c_str()+2, "%02X", &vkh) == 1) { def.vk = vkh; continue; }
        }

        // F1–F12
        if (p.size() >= 2 && p[0] == 'F') {
            int n = 0;
            for (size_t i = 1; i < p.size(); i++) {
                if (!isdigit((unsigned char)p[i])) { n = 0; break; }
                n = n * 10 + (p[i] - '0');
            }
            if (n >= 1 && n <= 12) { def.vk = VK_F1 + n - 1; continue; }
        }

        // Single letter or digit
        if (p.size() == 1) {
            char c = p[0];
            if (c >= 'A' && c <= 'Z') { def.vk = (DWORD)c; continue; }
            if (c >= '0' && c <= '9') { def.vk = (DWORD)c; continue; }
        }
    }

    if (def.vk == 0) def.vk = VK_SPACE;  // fallback
    return def;
}

// Canonical English key name — round-trips through parse_hotkey()
static std::wstring vk_canonical_name(DWORD vk) {
    switch (vk) {
    case VK_SPACE:   return L"Space";
    case VK_RETURN:  return L"Enter";
    case VK_TAB:     return L"Tab";
    case VK_BACK:    return L"Backspace";
    case VK_ESCAPE:  return L"Escape";
    case VK_DELETE:  return L"Delete";
    case VK_INSERT:  return L"Insert";
    case VK_HOME:    return L"Home";
    case VK_END:     return L"End";
    case VK_PRIOR:   return L"PageUp";
    case VK_NEXT:    return L"PageDown";
    case VK_UP:      return L"Up";
    case VK_DOWN:    return L"Down";
    case VK_LEFT:    return L"Left";
    case VK_RIGHT:   return L"Right";
    }
    if (vk >= VK_F1 && vk <= VK_F12)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::wstring(1, (wchar_t)vk);
    wchar_t buf[16]; swprintf(buf, 16, L"VK%02X", vk);
    return buf;
}

static HotkeyDef g_hotkey_start;
static HotkeyDef g_hotkey_stop;

// Build human-readable string from HotkeyDef (uses canonical names)
static std::string hotkey_to_string(const HotkeyDef& h) {
    std::string s;
    if (h.ctrl)  s += "Ctrl+";
    if (h.alt)   s += "Alt+";
    if (h.shift) s += "Shift+";
    if (h.win)   s += "Win+";
    std::wstring wname = vk_canonical_name(h.vk);
    s += std::string(wname.begin(), wname.end());
    return s;
}

// Minimal flat-JSON string extractor (no recursive nesting needed)
static std::string jstr(const std::string& s, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return {};
    p = s.find('"', p + needle.size());
    if (p == std::string::npos) return {};
    size_t q = s.find('"', p + 1);
    if (q == std::string::npos) return {};
    return s.substr(p + 1, q - p - 1);
}

static double jnum(const std::string& s, const std::string& key, double def) {
    std::string needle = "\"" + key + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return def;
    p = s.find(':', p + needle.size());
    if (p == std::string::npos) return def;
    while (p < s.size() && !isdigit((unsigned char)s[p]) && s[p] != '-') p++;
    try { return std::stod(s.substr(p)); } catch (...) { return def; }
}

// Extract "obj_key" → "{...}" → inner "key" value
static std::string jnested(const std::string& s, const std::string& obj, const std::string& key) {
    std::string needle = "\"" + obj + "\"";
    size_t p = s.find(needle);
    if (p == std::string::npos) return {};
    p = s.find('{', p + needle.size());
    if (p == std::string::npos) return {};
    int depth = 0;
    for (size_t i = p; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        else if (s[i] == '}' && --depth == 0)
            return jstr(s.substr(p, i - p + 1), key);
    }
    return {};
}

static Config load_config(const char* path) {
    log_info(std::string("Loading config from: ") + path);
    std::ifstream f(path);
    if (!f) {
        std::string err = std::string("Cannot open ") + path + ". Copy config.example.json → config.json.";
        log_error(err);
        throw std::runtime_error(err);
    }
    std::string raw((std::istreambuf_iterator<char>(f)), {});
    // Strip UTF-8 BOM
    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF)
        raw = raw.substr(3);

    Config c;
    c.app_id        = jstr(raw, "app_id");
    c.access_token  = jstr(raw, "access_token");
    c.secret_key    = jstr(raw, "secret_key");
    std::string rid = jstr(raw, "standard_resource_id");
    if (!rid.empty()) c.resource_id = rid;
    std::string tm = jstr(raw, "trigger_mode");
    if (!tm.empty()) c.trigger_mode = tm;
    std::string hks = jstr(raw, "hotkey");
    if (!hks.empty()) c.hotkey = hks;
    std::string hkstop = jstr(raw, "hotkey_stop");
    if (!hkstop.empty()) c.hotkey_stop = hkstop;
    c.request_timeout_ms = (int)(jnum(raw, "request_timeout_seconds", 120) * 1000);
    c.poll_interval_ms   = (int)(jnum(raw, "poll_interval_seconds",   1.2) * 1000);
    c.poll_timeout_ms    = (int)(jnum(raw, "poll_timeout_seconds",   45.0) * 1000);
    if (c.app_id.empty() || c.access_token.empty()) {
        std::string err = "config.json must contain app_id and access_token.";
        log_error(err);
        throw std::runtime_error(err);
    }
    log_info("Config loaded OK. app_id=" + c.app_id +
             " resource_id=" + c.resource_id +
             " hotkey=" + c.hotkey +
             " hotkey_stop=" + c.hotkey_stop);
    return c;
}

static void save_config(const Config& c) {
    std::ofstream f("config.json");
    f << "{\n"
      << "  \"app_id\": \""               << c.app_id        << "\",\n"
      << "  \"access_token\": \""         << c.access_token  << "\",\n"
      << "  \"secret_key\": \""           << c.secret_key    << "\",\n"
      << "  \"standard_resource_id\": \"" << c.resource_id   << "\",\n"
      << "  \"trigger_mode\": \""          << c.trigger_mode  << "\",\n"
      << "  \"hotkey\": \""               << c.hotkey        << "\",\n"
      << "  \"hotkey_stop\": \""          << c.hotkey_stop   << "\",\n"
      << "  \"standard_submit_endpoint\": \"https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit\",\n"
      << "  \"standard_query_endpoint\":  \"https://openspeech.bytedance.com/api/v3/auc/bigmodel/query\",\n"
      << "  \"request_timeout_seconds\": " << (c.request_timeout_ms / 1000)  << ",\n"
      << "  \"poll_interval_seconds\": "   << (c.poll_interval_ms  / 1000.0) << ",\n"
      << "  \"poll_timeout_seconds\": "    << (c.poll_timeout_ms   / 1000.0) << "\n"
      << "}\n";
    log_info("config.json saved");
}

// ════════════════════════════════════════════════════════════════════════════
// Base64 encoder
// ════════════════════════════════════════════════════════════════════════════
static const char B64T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64(const void* data, size_t n) {
    const auto* p = (const uint8_t*)data;
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = (uint32_t)p[i] << 16;
        if (i+1 < n) b |= (uint32_t)p[i+1] << 8;
        if (i+2 < n) b |= p[i+2];
        out += B64T[(b>>18)&63];
        out += B64T[(b>>12)&63];
        out += i+1<n ? B64T[(b>>6)&63] : '=';
        out += i+2<n ? B64T[b&63]      : '=';
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
// WAV builder
// ════════════════════════════════════════════════════════════════════════════
static std::vector<uint8_t> pcm_to_wav(const std::vector<int16_t>& pcm, int sr) {
    uint32_t ds = (uint32_t)(pcm.size() * 2);
    std::vector<uint8_t> wav(44 + ds);
    uint8_t* p = wav.data();
    auto w4 = [&](uint32_t v){ memcpy(p,&v,4); p+=4; };
    auto w2 = [&](uint16_t v){ memcpy(p,&v,2); p+=2; };
    memcpy(p,"RIFF",4); p+=4; w4(36+ds);
    memcpy(p,"WAVE",4); p+=4;
    memcpy(p,"fmt ",4); p+=4; w4(16); w2(1); w2(1);
    w4(sr); w4(sr*2); w2(2); w2(16);
    memcpy(p,"data",4); p+=4; w4(ds);
    memcpy(p, pcm.data(), ds);
    return wav;
}

// ════════════════════════════════════════════════════════════════════════════
// UUID
// ════════════════════════════════════════════════════════════════════════════
static std::string gen_uuid() {
    GUID g; CoCreateGuid(&g);
    char buf[40];
    snprintf(buf, sizeof(buf),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned long)g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

// ════════════════════════════════════════════════════════════════════════════
// WinHTTP client
// ════════════════════════════════════════════════════════════════════════════
struct HttpResp {
    std::string body, api_status, api_msg;
};

class HttpClient {
    HINTERNET ses_ = NULL, con_ = NULL;
public:
    explicit HttpClient(const wchar_t* host) {
        log_info(std::string("HttpClient: opening session to ") +
                 std::string(host, host + wcslen(host)));
        ses_ = WinHttpOpen(L"VoiceInput/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        if (!ses_) {
            DWORD err = GetLastError();
            log_error("WinHttpOpen failed, error=" + std::to_string(err));
            throw std::runtime_error("WinHttpOpen failed");
        }
        con_ = WinHttpConnect(ses_, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con_) {
            DWORD err = GetLastError();
            log_error("WinHttpConnect failed, error=" + std::to_string(err));
            throw std::runtime_error("WinHttpConnect failed");
        }
        log_info("HttpClient: session ready");
    }
    ~HttpClient() {
        if (con_) WinHttpCloseHandle(con_);
        if (ses_) WinHttpCloseHandle(ses_);
    }

    HttpResp post(const wchar_t* path, const std::string& body,
                  const std::vector<std::pair<std::string,std::string>>& hdrs,
                  int timeout_ms) {
        std::string path_str(path, path + wcslen(path));
        log_info("HTTP POST " + path_str + " body_bytes=" + std::to_string(body.size()) +
                 " timeout_ms=" + std::to_string(timeout_ms));

        HINTERNET req = WinHttpOpenRequest(con_, L"POST", path, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) {
            DWORD err = GetLastError();
            log_error("WinHttpOpenRequest failed, error=" + std::to_string(err));
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        WinHttpSetTimeouts(req, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

        std::wstring hs = L"Content-Type: application/json\r\n";
        for (auto& h : hdrs)
            hs += std::wstring(h.first.begin(), h.first.end()) + L": "
                + std::wstring(h.second.begin(), h.second.end()) + L"\r\n";
        WinHttpAddRequestHeaders(req, hs.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

        BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (!ok || !WinHttpReceiveResponse(req, NULL)) {
            DWORD err = GetLastError();
            WinHttpCloseHandle(req);
            std::string errmsg = "HTTP request failed, WinHTTP error=" + std::to_string(err);
            log_error(errmsg);
            throw std::runtime_error(errmsg);
        }

        HttpResp resp;
        auto ghdr = [&](const wchar_t* name) -> std::string {
            DWORD sz = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, NULL, &sz, NULL);
            if (!sz) return {};
            std::wstring buf(sz / sizeof(wchar_t) + 2, L'\0');
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, &buf[0], &sz, NULL);
            buf.resize(sz / sizeof(wchar_t));
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            return std::string(buf.begin(), buf.end());
        };
        resp.api_status = ghdr(L"X-Api-Status-Code");
        resp.api_msg    = ghdr(L"X-Api-Message");

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD rd = 0;
            WinHttpReadData(req, &chunk[0], avail, &rd);
            chunk.resize(rd);
            resp.body += chunk;
        }
        WinHttpCloseHandle(req);

        log_info("HTTP response: api_status=" + resp.api_status +
                 " api_msg=" + resp.api_msg +
                 " body_bytes=" + std::to_string(resp.body.size()));
        return resp;
    }
};

// ════════════════════════════════════════════════════════════════════════════
// ASR client (Volcengine)
// ════════════════════════════════════════════════════════════════════════════
class AsrClient {
    Config     cfg_;
    HttpClient http_;
public:
    explicit AsrClient(const Config& c) : cfg_(c), http_(API_HOST) {
        log_info("AsrClient initialized");
    }

    std::wstring recognize(const std::vector<uint8_t>& wav) {
        std::string rid = gen_uuid();
        log_info("ASR recognize: request_id=" + rid +
                 " wav_bytes=" + std::to_string(wav.size()));

        // ── Submit ───────────────────────────────────────────────────────────
        std::string body =
            "{\"user\":{\"uid\":\"" + cfg_.app_id + "\"},"
            "\"audio\":{\"data\":\"" + base64(wav.data(), wav.size()) + "\",\"format\":\"wav\"},"
            "\"request\":{\"model_name\":\"bigmodel\",\"enable_itn\":true,\"enable_punc\":true}}";

        log_info("ASR submit: request_id=" + rid);
        auto sr = http_.post(SUBMIT_PATH, body, make_hdrs(rid), cfg_.request_timeout_ms);
        if (sr.api_status != "20000000") {
            std::string err = "ASR submit error: code=" + sr.api_status + " msg=" + sr.api_msg;
            log_error(err);
            throw std::runtime_error(err);
        }
        log_info("ASR submit OK: request_id=" + rid);

        // ── Poll ─────────────────────────────────────────────────────────────
        std::string qbody = "{\"id\":\"" + rid + "\"}";
        ULONGLONG deadline = GetTickCount64() + (ULONGLONG)cfg_.poll_timeout_ms;
        int poll_count = 0;

        while (GetTickCount64() < deadline) {
            poll_count++;
            auto qr = http_.post(QUERY_PATH, qbody, make_hdrs(rid), cfg_.request_timeout_ms);
            std::string code   = qr.api_status;
            std::string text   = jnested(qr.body, "result", "text");
            std::string status = jnested(qr.body, "result", "status");

            log_info("ASR poll #" + std::to_string(poll_count) +
                     " code=" + code + " status=" + status +
                     " text_len=" + std::to_string(text.size()));

            if (code == "20000000") {
                if (!text.empty()) {
                    log_info("ASR success: text_len=" + std::to_string(text.size()));
                    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
                    std::wstring wt(len, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wt[0], len);
                    while (!wt.empty() && wt.back() == L'\0') wt.pop_back();
                    return wt;
                }
                if (status == "fail" || status == "failed" || status == "error") {
                    std::string err = "ASR transcription failed (status=" + status + ").";
                    log_error(err);
                    throw std::runtime_error(err);
                }
            } else if (code == "20000001") {
                log_info("ASR still processing...");
            } else if (code == "20000003") {
                log_warn("ASR: no valid speech detected");
                throw std::runtime_error("No valid speech detected.");
            } else if (code == "45000292") {
                log_warn("ASR: QPS throttled, waiting 2s+");
                Sleep((DWORD)std::max(cfg_.poll_interval_ms, 2000));
                continue;
            } else {
                std::string err = "ASR query error: code=" + code + " msg=" + qr.api_msg;
                log_error(err);
                throw std::runtime_error(err);
            }
            Sleep((DWORD)cfg_.poll_interval_ms);
        }
        log_error("ASR timeout after " + std::to_string(poll_count) + " polls");
        throw std::runtime_error("ASR timeout.");
    }

private:
    std::vector<std::pair<std::string,std::string>> make_hdrs(const std::string& rid) {
        return {
            {"X-Api-App-Key",     cfg_.app_id},
            {"X-Api-Access-Key",  cfg_.access_token},
            {"X-Api-Resource-Id", cfg_.resource_id},
            {"X-Api-Request-Id",  rid},
            {"X-Api-Sequence",    "-1"},
        };
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Audio recorder — WinMM waveIn
// ════════════════════════════════════════════════════════════════════════════
class AudioRecorder {
    static const int N_BUF = 4, BUF_SZ = 4096; // samples per buffer
    HWAVEIN               hwi_  = NULL;
    std::atomic<bool>     alive_{false};
    std::mutex            mu_;
    std::vector<int16_t>  data_;
    int16_t               raw_[N_BUF][BUF_SZ];
    WAVEHDR               hdr_[N_BUF];

public:
    void start() {
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = SAMPLE_RATE;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = SAMPLE_RATE * 2;

        data_.clear();
        alive_ = true;

        log_info("AudioRecorder: opening waveIn device (WAVE_MAPPER, 16kHz mono int16)");
        MMRESULT r = waveInOpen(&hwi_, WAVE_MAPPER, &wfx,
            (DWORD_PTR)&AudioRecorder::cb, (DWORD_PTR)this, CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            alive_ = false;
            std::string err = "waveInOpen failed (code " + std::to_string(r) +
                "). Check microphone permission.";
            log_error(err);
            throw std::runtime_error(err);
        }
        log_info("AudioRecorder: waveInOpen OK, preparing " + std::to_string(N_BUF) + " buffers");

        for (int i = 0; i < N_BUF; i++) {
            hdr_[i]              = {};
            hdr_[i].lpData       = (LPSTR)raw_[i];
            hdr_[i].dwBufferLength = BUF_SZ * 2;
            waveInPrepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR));
            waveInAddBuffer(hwi_, &hdr_[i], sizeof(WAVEHDR));
        }
        waveInStart(hwi_);
        log_info("AudioRecorder: recording started");
    }

    std::vector<int16_t> stop() {
        alive_ = false;
        if (hwi_) {
            log_info("AudioRecorder: stopping");
            waveInStop(hwi_);
            waveInReset(hwi_);
            for (int i = 0; i < N_BUF; i++)
                waveInUnprepareHeader(hwi_, &hdr_[i], sizeof(WAVEHDR));
            waveInClose(hwi_);
            hwi_ = NULL;
        }
        std::lock_guard<std::mutex> lk(mu_);
        log_info("AudioRecorder: stopped, samples=" + std::to_string(data_.size()) +
                 " duration_ms=" + std::to_string(data_.size() * 1000 / SAMPLE_RATE));
        return data_;
    }

private:
    static void CALLBACK cb(HWAVEIN hwi, UINT msg, DWORD_PTR inst,
                            DWORD_PTR p1, DWORD_PTR) {
        if (msg != WIM_DATA) return;
        auto* self = (AudioRecorder*)inst;
        if (!self->alive_) return;
        auto* h = (WAVEHDR*)p1;
        int n = h->dwBytesRecorded / 2;
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            auto* d = (int16_t*)h->lpData;
            self->data_.insert(self->data_.end(), d, d + n);
        }
        h->dwBytesRecorded = 0;
        waveInAddBuffer(hwi, h, sizeof(WAVEHDR));
    }
};

// ════════════════════════════════════════════════════════════════════════════
// Global application state
// ════════════════════════════════════════════════════════════════════════════
enum AppState { IDLE, RECORDING, TRANSCRIBING };

struct AppGlobals {
    HWND         overlay   = NULL;
    HWND         target    = NULL;  // window to paste into
    AppState     state     = IDLE;
    AudioRecorder recorder;
    AsrClient*   asr       = NULL;
    Config       config;

    // Overlay text (written from main thread only, so no mutex needed here)
    std::wstring main_text, sub_text;

    // Status mini-window + config dialog
    HWND status_wnd = NULL;
    HWND config_wnd = NULL;

    // Tray icon
    NOTIFYICONDATA nid = {};
    bool tray_added    = false;
} G;

// ════════════════════════════════════════════════════════════════════════════
// Status mini-window (68×82, always on top, draggable, top-right corner)
// ════════════════════════════════════════════════════════════════════════════
static HFONT  g_font_status     = NULL;
static HFONT  g_font_status_sub = NULL;
static bool   g_auto_enter      = true;

// Drag state
static bool  g_st_dragging  = false;
static POINT g_st_drag_orig = {};
static POINT g_st_wnd_orig  = {};

// Gear button hit rect (in client coords, computed in WM_PAINT)
static RECT  g_gear_rect    = {};

static void status_style(AppState s, COLORREF& dot, const wchar_t*& label) {
    switch (s) {
    case RECORDING:    dot = RGB(255, 70,  70);  label = L"录音"; break;
    case TRANSCRIBING: dot = RGB(70,  140, 255); label = L"识别"; break;
    default:           dot = RGB(120, 120, 120); label = L"待机"; break;
    }
}

static void status_set_round_rgn(HWND hwnd) {
    HRGN rgn = CreateRoundRectRgn(0, 0, STATUS_W + 1, STATUS_H + 1, 14, 14);
    SetWindowRgn(hwnd, rgn, TRUE);
}

static LRESULT CALLBACK StatusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        // Use ANTIALIASED_QUALITY for crisp rendering on any DPI
        g_font_status = CreateFont(-26, 0,0,0, FW_BOLD, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        g_font_status_sub = CreateFont(-15, 0,0,0, FW_NORMAL, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        status_set_round_rgn(hwnd);
        return 0;

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        // Use memory DC to avoid flicker / sub-pixel artifacts
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP old_bmp = (HBITMAP)SelectObject(mdc, bmp);

        // ── Layout constants ──────────────────────────────────────────────
        // Window is STATUS_W × STATUS_H (200 × 200)
        // Zone A: state indicator  — top 56%  (0..112)
        // Divider 1                — y=112
        // Zone B: auto-enter       — 113..157 (44px)
        // Divider 2                — y=157
        // Zone C: hotkey + gear    — 158..200 (42px)

        const int W  = rc.right;          // 200
        const int cx = W / 2;
        const int DIV1 = 112, DIV2 = 157;

        // ── Background ───────────────────────────────────────────────────
        HBRUSH bg_br = CreateSolidBrush(RGB(18, 18, 24));
        FillRect(mdc, &rc, bg_br);
        DeleteObject(bg_br);

        SetBkMode(mdc, TRANSPARENT);

        // ── Zone A: colored dot + state label ────────────────────────────
        COLORREF dot_col; const wchar_t* label;
        status_style(G.state, dot_col, label);

        const int dot_r = 20, dot_cy = 46;
        HBRUSH dot_br = CreateSolidBrush(dot_col);
        HPEN   dot_pn = CreatePen(PS_SOLID, 0, dot_col);
        SelectObject(mdc, dot_br); SelectObject(mdc, dot_pn);
        Ellipse(mdc, cx-dot_r, dot_cy-dot_r, cx+dot_r, dot_cy+dot_r);
        DeleteObject(dot_br); DeleteObject(dot_pn);

        SelectObject(mdc, g_font_status);
        SetTextColor(mdc, RGB(230, 230, 235));
        RECT rA = { 0, dot_cy+dot_r+8, W, DIV1-2 };
        DrawText(mdc, label, -1, &rA, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // ── Divider 1 ─────────────────────────────────────────────────────
        auto draw_div = [&](int y) {
            HPEN p = CreatePen(PS_SOLID, 1, RGB(44, 44, 56));
            SelectObject(mdc, p);
            MoveToEx(mdc, 14, y, NULL); LineTo(mdc, W-14, y);
            DeleteObject(p);
        };
        draw_div(DIV1);

        // ── Zone B: auto-enter toggle switch ─────────────────────────────
        // Label on the left, iOS-style toggle on the right
        const int zB_cy  = (DIV1 + DIV2) / 2;   // vertical center of zone B = 134

        // Label
        SelectObject(mdc, g_font_status_sub);
        SetTextColor(mdc, RGB(210, 210, 218));
        RECT rB_lbl = { 14, DIV1+1, W/2 + 10, DIV2 };
        DrawText(mdc, L"自动发送", -1, &rB_lbl, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Toggle track (64 × 28, right-aligned, vertically centered)
        const int TW = 58, TH = 28;
        const int tx = W - TW - 14;           // track left x
        const int ty = zB_cy - TH / 2;        // track top y
        COLORREF track_col = g_auto_enter ? RGB(48, 192, 100) : RGB(52, 52, 66);
        HBRUSH trk_br = CreateSolidBrush(track_col);
        HPEN   trk_pn = CreatePen(PS_SOLID, 0, track_col);
        SelectObject(mdc, trk_br); SelectObject(mdc, trk_pn);
        RoundRect(mdc, tx, ty, tx+TW, ty+TH, TH, TH);   // fully rounded ends
        DeleteObject(trk_br); DeleteObject(trk_pn);

        // Toggle thumb (circle, slides inside the track)
        const int thumb_r = TH/2 - 3;         // radius = 11
        const int thumb_cx = g_auto_enter
            ? (tx + TW - thumb_r - 4)          // right side (ON)
            : (tx + thumb_r + 4);              // left  side (OFF)
        const int thumb_cy = zB_cy;
        HBRUSH th_br = CreateSolidBrush(RGB(255, 255, 255));
        HPEN   th_pn = CreatePen(PS_SOLID, 0, RGB(255, 255, 255));
        SelectObject(mdc, th_br); SelectObject(mdc, th_pn);
        Ellipse(mdc, thumb_cx-thumb_r, thumb_cy-thumb_r,
                     thumb_cx+thumb_r, thumb_cy+thumb_r);
        DeleteObject(th_br); DeleteObject(th_pn);

        // ── Divider 2 ─────────────────────────────────────────────────────
        draw_div(DIV2);

        // ── Zone C: hotkey hint (left) + gear button (right) ─────────────
        SelectObject(mdc, g_font_status_sub);

        // Gear button — right side
        g_gear_rect = { W-40, DIV2+8, W-8, rc.bottom-8 };
        HBRUSH gear_bg = CreateSolidBrush(RGB(36, 38, 50));
        HPEN   gear_pn = CreatePen(PS_SOLID, 1, RGB(62, 64, 80));
        SelectObject(mdc, gear_bg); SelectObject(mdc, gear_pn);
        RoundRect(mdc, g_gear_rect.left, g_gear_rect.top,
                       g_gear_rect.right, g_gear_rect.bottom, 6, 6);
        DeleteObject(gear_bg); DeleteObject(gear_pn);
        SetTextColor(mdc, RGB(170, 172, 196));
        DrawText(mdc, L"⚙", -1, &g_gear_rect, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // Hotkey hint — left of gear
        std::wstring hk_w;
        if (G.config.trigger_mode == "longpress") {
            hk_w = L"长按空格";
        } else {
            std::string s = hotkey_to_string(g_hotkey_start);
            hk_w = std::wstring(s.begin(), s.end());
        }
        RECT rC = { 10, DIV2+1, g_gear_rect.left-6, rc.bottom };
        SetTextColor(mdc, RGB(88, 90, 108));
        DrawText(mdc, hk_w.c_str(), -1, &rC, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // Blit memory DC → real DC
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old_bmp);
        DeleteObject(bmp); DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ── Mouse: drag + click logic ─────────────────────────────────────────
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        GetCursorPos(&g_st_drag_orig);
        RECT wr; GetWindowRect(hwnd, &wr);
        g_st_wnd_orig = { wr.left, wr.top };
        g_st_dragging = false;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetCapture() != hwnd) return 0;
        POINT cur; GetCursorPos(&cur);
        int dx = cur.x - g_st_drag_orig.x;
        int dy = cur.y - g_st_drag_orig.y;
        if (!g_st_dragging && (abs(dx) > 4 || abs(dy) > 4))
            g_st_dragging = true;
        if (g_st_dragging)
            SetWindowPos(hwnd, NULL,
                g_st_wnd_orig.x + dx, g_st_wnd_orig.y + dy,
                0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() != hwnd) return 0;
        ReleaseCapture();
        if (!g_st_dragging) {
            POINT pt = { LOWORD(lp), HIWORD(lp) };
            if (PtInRect(&g_gear_rect, pt)) {
                // Open config dialog
                PostMessage(G.overlay, WM_APP + 7, 0, 0);
            } else {
                // Toggle auto-enter
                g_auto_enter = !g_auto_enter;
                log_info(std::string("Auto-enter: ") + (g_auto_enter ? "ON" : "OFF"));
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        g_st_dragging = false;
        return 0;
    }

    case WM_DESTROY:
        if (g_font_status)     { DeleteObject(g_font_status);     g_font_status     = NULL; }
        if (g_font_status_sub) { DeleteObject(g_font_status_sub); g_font_status_sub = NULL; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void status_refresh() {
    if (G.status_wnd) InvalidateRect(G.status_wnd, NULL, TRUE);
}

// ════════════════════════════════════════════════════════════════════════════
// Hotkey capture edit — subclassed Edit that records key presses
// ════════════════════════════════════════════════════════════════════════════

// Property names stored on each HWND
static const wchar_t* PROP_ORIG_PROC = L"HK_OrigProc";
static const wchar_t* PROP_WAITING   = L"HK_Waiting";
static const wchar_t* PROP_SAVED_TXT = L"HK_SavedTxt";

static LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)GetProp(hwnd, PROP_ORIG_PROC);
    if (!orig) return DefWindowProc(hwnd, msg, wp, lp);

    switch (msg) {
    // Tell the dialog we want ALL keys (prevents Tab/Enter navigation out)
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_SETFOCUS: {
        // Save current text so we can restore it if user bails out
        wchar_t prev[256] = {};
        GetWindowText(hwnd, prev, 256);
        SetProp(hwnd, PROP_SAVED_TXT, (HANDLE)_wcsdup(prev));

        SetProp(hwnd, PROP_WAITING, (HANDLE)1);
        SetWindowText(hwnd, L"请按下快捷键...");
        CallWindowProc(orig, hwnd, msg, wp, lp);   // let Edit handle cursor
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_KILLFOCUS: {
        bool captured = (GetProp(hwnd, PROP_WAITING) == 0);
        SetProp(hwnd, PROP_WAITING, (HANDLE)0);

        // If focus left without a key being captured, restore saved text
        if (!captured) {
            wchar_t cur[256] = {};
            GetWindowText(hwnd, cur, 256);
            if (wcscmp(cur, L"请按下快捷键...") == 0) {
                wchar_t* saved = (wchar_t*)GetProp(hwnd, PROP_SAVED_TXT);
                SetWindowText(hwnd, saved ? saved : L"");
            }
        }
        wchar_t* saved = (wchar_t*)GetProp(hwnd, PROP_SAVED_TXT);
        if (saved) { free(saved); RemoveProp(hwnd, PROP_SAVED_TXT); }

        InvalidateRect(hwnd, NULL, TRUE);
        return CallWindowProc(orig, hwnd, msg, wp, lp);
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        DWORD vk = (DWORD)wp;
        // Ignore standalone modifiers — stay in "请按下快捷键..." mode
        if (vk == VK_MENU || vk == VK_CONTROL || vk == VK_SHIFT ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL)
            return 0;

        // Escape = cancel, restore saved text
        if (vk == VK_ESCAPE) {
            SetProp(hwnd, PROP_WAITING, (HANDLE)0);
            wchar_t* saved = (wchar_t*)GetProp(hwnd, PROP_SAVED_TXT);
            SetWindowText(hwnd, saved ? saved : L"");
            if (saved) { free(saved); RemoveProp(hwnd, PROP_SAVED_TXT); }
            SendMessage(GetParent(hwnd), WM_NEXTDLGCTL, 0, FALSE);
            return 0;
        }

        bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

        // Build canonical string that parse_hotkey() can reliably decode
        std::wstring s;
        if (ctrl)  s += L"Ctrl+";
        if (alt)   s += L"Alt+";
        if (shift) s += L"Shift+";
        s += vk_canonical_name(vk);

        SetWindowText(hwnd, s.c_str());
        SetProp(hwnd, PROP_WAITING, (HANDLE)0);   // captured!
        wchar_t* saved = (wchar_t*)GetProp(hwnd, PROP_SAVED_TXT);
        if (saved) { free(saved); RemoveProp(hwnd, PROP_SAVED_TXT); }
        // Move focus to next control so result is visible
        SendMessage(GetParent(hwnd), WM_NEXTDLGCTL, 0, FALSE);
        return 0;
    }

    case WM_CHAR:
        return 0;  // suppress all typed characters

    case WM_PAINT: {
        LRESULT r = CallWindowProc(orig, hwnd, msg, wp, lp);
        // Blue border overlay when waiting for input
        if (GetProp(hwnd, PROP_WAITING)) {
            HDC dc = GetDC(hwnd);
            RECT rc; GetClientRect(hwnd, &rc);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
            HPEN old = (HPEN)SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, old); DeleteObject(pen);
            ReleaseDC(hwnd, dc);
        }
        return r;
    }
    }
    return CallWindowProc(orig, hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════════════════════════
// Config dialog window
// ════════════════════════════════════════════════════════════════════════════
static HWND make_label(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowEx(0, L"STATIC", text,
        WS_CHILD|WS_VISIBLE|SS_RIGHT,
        x, y, w, h, parent, NULL, NULL, NULL);
}
static HWND make_edit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND e = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
        x, y, w, h, parent, (HMENU)(UINT_PTR)id, NULL, NULL);
    return e;
}

static LRESULT CALLBACK ConfigProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Larger font for the bigger dialog
        HFONT hf = CreateFont(-16, 0,0,0, FW_NORMAL, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        HFONT hf_bold = CreateFont(-16, 0,0,0, FW_SEMIBOLD, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        auto set_font = [&](HWND h){ SendMessage(h, WM_SETFONT, (WPARAM)hf, TRUE); };
        auto ws = [](const std::string& s) -> std::wstring {
            return std::wstring(s.begin(), s.end());
        };

        int lw = 160, ew = 440, row = 48, eh = 32, x0 = 20, ex = 190, y = 24;

        set_font(make_label(hwnd, L"App ID:",        x0, y+6, lw, 28));
        set_font(make_edit (hwnd, IDC_EDIT_APPID,    L"", ex, y, ew, eh));
        y += row;
        set_font(make_label(hwnd, L"Access Token:",  x0, y+6, lw, 28));
        set_font(make_edit (hwnd, IDC_EDIT_TOKEN,    L"", ex, y, ew, eh));
        y += row;
        set_font(make_label(hwnd, L"Secret Key:",    x0, y+6, lw, 28));
        set_font(make_edit (hwnd, IDC_EDIT_SECRET,   L"", ex, y, ew, eh));
        y += row + 10;

        // Section separator
        HWND sep = CreateWindowEx(0, L"STATIC",
            L"─────────── 触发方式 ───────────",
            WS_CHILD|WS_VISIBLE|SS_LEFT, x0, y, ew+lw+10, 24, hwnd, NULL, NULL, NULL);
        SendMessage(sep, WM_SETFONT, (WPARAM)hf_bold, TRUE);
        y += 32;

        // Long-press toggle checkbox
        HWND chk = CreateWindowEx(0, L"BUTTON",
            L"长按空格 1.5 秒触发（不使用自定义快捷键）",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            x0, y, ew+lw+4, 28, hwnd, (HMENU)IDC_CHK_LONGPRESS, NULL, NULL);
        SendMessage(chk, WM_SETFONT, (WPARAM)hf, TRUE);
        if (G.config.trigger_mode == "longpress")
            SendMessage(chk, BM_SETCHECK, BST_CHECKED, 0);
        y += 36;

        sep = CreateWindowEx(0, L"STATIC",
            L"─────────── 快捷键设置  （点击输入框后按组合键）───────────",
            WS_CHILD|WS_VISIBLE|SS_LEFT, x0, y, ew+lw+10, 24, hwnd, NULL, NULL, NULL);
        SendMessage(sep, WM_SETFONT, (WPARAM)hf_bold, TRUE);
        y += 32;

        set_font(make_label(hwnd, L"开始录音:",  x0, y+6, lw, 28));
        HWND hk_start = make_edit(hwnd, IDC_EDIT_HOTKEY, L"", ex, y, ew, eh);
        set_font(hk_start);
        {
            WNDPROC orig = (WNDPROC)SetWindowLongPtr(hk_start, GWLP_WNDPROC, (LONG_PTR)HotkeyEditProc);
            SetProp(hk_start, PROP_ORIG_PROC, (HANDLE)orig);
        }
        y += row;
        set_font(make_label(hwnd, L"停止录音:",  x0, y+6, lw, 28));
        HWND hk_stop = make_edit(hwnd, IDC_EDIT_HKSTOP, L"", ex, y, ew, eh);
        set_font(hk_stop);
        {
            WNDPROC orig = (WNDPROC)SetWindowLongPtr(hk_stop, GWLP_WNDPROC, (LONG_PTR)HotkeyEditProc);
            SetProp(hk_stop, PROP_ORIG_PROC, (HANDLE)orig);
        }
        y += row + 16;

        HWND bsave   = CreateWindowEx(0, L"BUTTON", L"保  存",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            ex,      y, 160, 40, hwnd, (HMENU)IDC_BTN_SAVE,   NULL, NULL);
        HWND bcancel = CreateWindowEx(0, L"BUTTON", L"取  消",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            ex+172,  y, 160, 40, hwnd, (HMENU)IDC_BTN_CANCEL, NULL, NULL);
        SendMessage(bsave,   WM_SETFONT, (WPARAM)hf_bold, TRUE);
        SendMessage(bcancel, WM_SETFONT, (WPARAM)hf,      TRUE);

        // Populate all fields
        SetDlgItemText(hwnd, IDC_EDIT_APPID,  ws(G.config.app_id).c_str());
        SetDlgItemText(hwnd, IDC_EDIT_TOKEN,  ws(G.config.access_token).c_str());
        SetDlgItemText(hwnd, IDC_EDIT_SECRET, ws(G.config.secret_key).c_str());
        SetDlgItemText(hwnd, IDC_EDIT_HOTKEY, ws(G.config.hotkey).c_str());
        SetDlgItemText(hwnd, IDC_EDIT_HKSTOP, ws(G.config.hotkey_stop).c_str());
        // Disable hotkey fields if in longpress mode
        if (G.config.trigger_mode == "longpress") {
            EnableWindow(GetDlgItem(hwnd, IDC_EDIT_HOTKEY), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_EDIT_HKSTOP),  FALSE);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHK_LONGPRESS) {
            // Toggle: disable/enable hotkey fields based on checkbox
            bool longpress = (SendDlgItemMessage(hwnd, IDC_CHK_LONGPRESS,
                                                  BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_EDIT_HOTKEY), !longpress);
            EnableWindow(GetDlgItem(hwnd, IDC_EDIT_HKSTOP),  !longpress);
            return 0;
        }
        if (LOWORD(wp) == IDC_BTN_CANCEL) {
            G.config_wnd = NULL;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDC_BTN_SAVE) {
            auto get = [&](int id) -> std::string {
                wchar_t buf[512] = {};
                GetDlgItemText(hwnd, id, buf, 512);
                return std::string(buf, buf + wcslen(buf));
            };
            Config c = G.config;
            c.app_id        = get(IDC_EDIT_APPID);
            c.access_token  = get(IDC_EDIT_TOKEN);
            c.secret_key    = get(IDC_EDIT_SECRET);
            c.hotkey        = get(IDC_EDIT_HOTKEY);
            c.hotkey_stop   = get(IDC_EDIT_HKSTOP);
            bool lp = (SendDlgItemMessage(hwnd, IDC_CHK_LONGPRESS,
                                          BM_GETCHECK, 0, 0) == BST_CHECKED);
            c.trigger_mode = lp ? "longpress" : "hotkey";
            if (c.app_id.empty() || c.access_token.empty()) {
                MessageBoxW(hwnd, L"App ID 和 Access Token 不能为空。",
                            L"VoiceInput", MB_ICONWARNING|MB_OK);
                return 0;
            }
            if (c.hotkey.empty()) c.hotkey = "Alt+Space";
            if (c.hotkey_stop.empty()) c.hotkey_stop = "Space";
            save_config(c);
            G.config = c;
            // Apply hotkeys live
            g_hotkey_start = parse_hotkey(G.config.hotkey);
            g_hotkey_stop  = parse_hotkey(G.config.hotkey_stop);
            log_info("Hotkey start: " + hotkey_to_string(g_hotkey_start));
            log_info("Hotkey stop:  " + hotkey_to_string(g_hotkey_stop));
            // Recreate ASR client
            delete G.asr;
            G.asr = new AsrClient(G.config);
            log_info("Config updated and applied live");
            // Refresh status window to show new hotkey hint
            if (G.status_wnd) InvalidateRect(G.status_wnd, NULL, TRUE);
            MessageBoxW(hwnd, L"配置已保存，立即生效。", L"VoiceInput", MB_ICONINFORMATION|MB_OK);
            G.config_wnd = NULL;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        G.config_wnd = NULL;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void open_config_dialog(HINSTANCE hInst) {
    if (G.config_wnd && IsWindow(G.config_wnd)) {
        SetForegroundWindow(G.config_wnd);
        return;
    }
    int dw = 700, dh = 580;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    G.config_wnd = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        CONFIG_CLASS, L"VoiceInput — 配置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (sx - dw) / 2, (sy - dh) / 2, dw, dh,
        NULL, NULL, hInst, NULL
    );
    if (G.config_wnd) ShowWindow(G.config_wnd, SW_SHOW);
}

// ════════════════════════════════════════════════════════════════════════════
// System tray icon
// ════════════════════════════════════════════════════════════════════════════
static void tray_add(HINSTANCE hInst) {
    G.nid.cbSize           = sizeof(G.nid);
    G.nid.hWnd             = G.overlay;
    G.nid.uID              = 1;
    G.nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    G.nid.uCallbackMessage = WM_APP_TRAY;
    G.nid.hIcon            = LoadIcon(NULL, IDI_INFORMATION);
    wcscpy_s(G.nid.szTip, L"VoiceInput — 点击状态窗口⚙配置快捷键");
    Shell_NotifyIcon(NIM_ADD, &G.nid);
    G.tray_added = true;
    log_info("Tray icon added");
}

static void tray_balloon(const wchar_t* title, const wchar_t* msg) {
    if (!G.tray_added) return;
    NOTIFYICONDATA nid2 = G.nid;
    nid2.uFlags      |= NIF_INFO;
    nid2.dwInfoFlags  = NIIF_INFO;
    nid2.uTimeout     = 4000;
    wcscpy_s(nid2.szInfoTitle, title);
    wcscpy_s(nid2.szInfo,      msg);
    Shell_NotifyIcon(NIM_MODIFY, &nid2);
}

static void tray_remove() {
    if (G.tray_added) {
        Shell_NotifyIcon(NIM_DELETE, &G.nid);
        G.tray_added = false;
        log_info("Tray icon removed");
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Keyboard hook
// ════════════════════════════════════════════════════════════════════════════
static HHOOK              g_hook     = NULL;
static DWORD              g_hook_tid = 0;
static std::atomic<bool>  g_recording{false};
static std::atomic<bool>  g_hotkey_consumed{false};

// Long-press mode state
static const ULONGLONG    LONG_PRESS_MS  = 1500;
static const ULONGLONG    LONG_PRESS_TOL = 200;
static std::atomic<bool>  g_lp_space_dn{false};
static std::atomic<bool>  g_lp_fired{false};
static ULONGLONG          g_lp_press_ms = 0;

static bool hotkey_matches(const HotkeyDef& h, DWORD vkCode) {
    if (vkCode != h.vk) return false;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool win   = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
    return alt == h.alt && ctrl == h.ctrl && shift == h.shift && win == h.win;
}

static LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0) return CallNextHookEx(g_hook, code, wParam, lParam);

    auto* kb      = (KBDLLHOOKSTRUCT*)lParam;
    bool injected = (kb->flags & LLKHF_INJECTED) != 0;
    bool is_down  = (wParam == WM_KEYDOWN  || wParam == WM_SYSKEYDOWN);
    bool is_up    = (wParam == WM_KEYUP    || wParam == WM_SYSKEYUP);

    // Injected keys pass through (our own re-injected events)
    if (injected) return CallNextHookEx(g_hook, code, wParam, lParam);

    // ESC = cancel current recording/transcribing state
    if (kb->vkCode == VK_ESCAPE && is_down) {
        if (g_recording.load()) {
            log_info("ESC — cancel recording");
            g_recording = false; g_lp_space_dn = false; g_lp_fired = false;
            g_hotkey_consumed = false;
            PostMessage(G.overlay, WM_APP_STOP, 0, 0);
            return 1;
        }
        if (G.state == TRANSCRIBING || G.state == RECORDING) {
            log_info("ESC — cancel state");
            PostMessage(G.overlay, WM_APP_ERROR, 0,
                (LPARAM)new std::wstring(L"已取消"));
            return 1;
        }
        return 1;  // absorb ESC silently in idle
    }

    bool longpress_mode = (G.config.trigger_mode == "longpress");

    // ════════════════════════════════════════════════════════════════════
    // Long-press Space (1.5 s) mode
    // ════════════════════════════════════════════════════════════════════
    if (longpress_mode) {
        // Only handle Space
        if (kb->vkCode != VK_SPACE)
            return CallNextHookEx(g_hook, code, wParam, lParam);

        // While recording: Space → stop
        if (g_recording.load()) {
            if (is_down) {
                g_recording = false; g_lp_space_dn = false; g_lp_fired = false;
                PostMessage(G.overlay, WM_APP_STOP, 0, 0);
                return 1;
            }
            return 1;
        }

        if (is_down && !g_lp_space_dn.exchange(true)) {
            // Fresh press — start timer thread
            g_lp_fired = false;
            g_lp_press_ms = GetTickCount64();
            ULONGLONG t0 = g_lp_press_ms;
            HWND wnd = G.overlay;
            std::thread([wnd, t0](){
                Sleep((DWORD)LONG_PRESS_MS);
                if (g_lp_space_dn.load() && g_lp_press_ms == t0
                        && !g_lp_fired.exchange(true)) {
                    PostMessage(wnd, WM_APP_START, 0, 0);
                }
            }).detach();
            return 1;
        }
        if (is_down) return 1;  // key-repeat suppressed

        if (is_up) {
            bool was_dn = g_lp_space_dn.exchange(false);
            ULONGLONG held = GetTickCount64() - g_lp_press_ms;
            if (g_lp_fired.exchange(false)) return 1;  // timer already fired
            if (was_dn && held >= LONG_PRESS_MS - LONG_PRESS_TOL) {
                // Released right at threshold — trigger now
                g_lp_fired = true;
                PostMessage(G.overlay, WM_APP_START, 0, 0);
                return 1;
            }
            if (was_dn) {
                // Short press — inject space back
                keybd_event(VK_SPACE, 0, 0, 0);
                keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
            }
            return 1;
        }
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // ════════════════════════════════════════════════════════════════════
    // Custom hotkey mode
    // ════════════════════════════════════════════════════════════════════

    // While recording: stop hotkey → stop
    if (g_recording.load()) {
        if (is_down) {
            bool stop_hit = hotkey_matches(g_hotkey_stop, kb->vkCode)
                         || hotkey_matches(g_hotkey_start, kb->vkCode);
            if (stop_hit) {
                g_recording = false; g_hotkey_consumed = false;
                PostMessage(G.overlay, WM_APP_STOP, 0, 0);
                return 1;
            }
        }
        if (kb->vkCode == g_hotkey_start.vk || kb->vkCode == g_hotkey_stop.vk)
            return 1;
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // Start hotkey
    if (is_down && hotkey_matches(g_hotkey_start, kb->vkCode)) {
        g_hotkey_consumed = true;
        PostMessage(G.overlay, WM_APP_START, 0, 0);
        return 1;
    }
    if (is_up && g_hotkey_consumed.exchange(false)
              && kb->vkCode == g_hotkey_start.vk)
        return 1;

    return CallNextHookEx(g_hook, code, wParam, lParam);
}

static void hook_thread_fn() {
    g_hook_tid = GetCurrentThreadId();
    log_info("Hook thread started, tid=" + std::to_string(g_hook_tid));
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, HookProc, NULL, 0);
    if (!g_hook) {
        DWORD err = GetLastError();
        log_error("SetWindowsHookEx failed, error=" + std::to_string(err));
        PostMessage(G.overlay, WM_APP_ERROR, 0,
            (LPARAM)new std::wstring(L"Cannot install keyboard hook."));
        return;
    }
    log_info("Keyboard hook installed OK");
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
        DispatchMessage(&msg);
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    log_info("Hook thread exited");
}

// ════════════════════════════════════════════════════════════════════════════
// Overlay helpers
// ════════════════════════════════════════════════════════════════════════════
static void overlay_show(const wchar_t* main, const wchar_t* sub) {
    G.main_text = main;
    G.sub_text  = sub;
    ShowWindow(G.overlay, SW_SHOWNA);
    SetWindowPos(G.overlay, HWND_TOPMOST, 0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    InvalidateRect(G.overlay, NULL, TRUE);
}

static void overlay_hide() { ShowWindow(G.overlay, SW_HIDE); }

// ════════════════════════════════════════════════════════════════════════════
// Action handlers — all called on the main thread via WndProc
// ════════════════════════════════════════════════════════════════════════════
static void on_start_recording() {
    if (G.state != IDLE) {
        log_warn("on_start_recording: ignored, state=" + std::to_string(G.state));
        return;
    }
    log_info("on_start_recording");
    G.target = GetForegroundWindow();
    try { G.recorder.start(); }
    catch (std::exception& e) {
        std::string s = e.what();
        log_error("Recorder start failed: " + s);
        PostMessage(G.overlay, WM_APP_ERROR, 0,
            (LPARAM)new std::wstring(s.begin(), s.end()));
        return;
    }
    G.state  = RECORDING;
    g_recording = true;
    overlay_show(L"Listening", L"Press Space to stop");
    status_refresh();
    log_info("State → RECORDING");
}

static void on_stop_recording() {
    if (G.state != RECORDING) {
        log_warn("on_stop_recording: ignored, state=" + std::to_string(G.state));
        return;
    }
    log_info("on_stop_recording");
    g_recording = false;
    G.state = TRANSCRIBING;
    overlay_show(L"Transcribing", L"Please wait\u2026");
    status_refresh();
    log_info("State → TRANSCRIBING");

    auto pcm = G.recorder.stop();
    size_t min_samp = (size_t)(SAMPLE_RATE * MIN_RECORD_SEC);

    if (pcm.empty()) {
        log_error("No audio captured (empty PCM)");
        PostMessage(G.overlay, WM_APP_ERROR, 0,
            (LPARAM)new std::wstring(L"No audio captured. Check microphone permission."));
        return;
    }
    if (pcm.size() < min_samp) {
        log_warn("Audio too short: samples=" + std::to_string(pcm.size()) +
                 " min=" + std::to_string(min_samp));
        PostMessage(G.overlay, WM_APP_ERROR, 0,
            (LPARAM)new std::wstring(L"Audio too short \u2014 speak longer and try again."));
        return;
    }
    log_info("PCM OK: samples=" + std::to_string(pcm.size()) +
             " duration_ms=" + std::to_string(pcm.size() * 1000 / SAMPLE_RATE));

    HWND wnd = G.overlay;
    AsrClient* asr = G.asr;
    std::thread([pcm, wnd, asr]() mutable {
        try {
            auto wav  = pcm_to_wav(pcm, SAMPLE_RATE);
            log_info("WAV built: bytes=" + std::to_string(wav.size()));
            auto text = asr->recognize(wav);
            log_info("Transcription done, posting WM_APP_PASTE");
            PostMessage(wnd, WM_APP_PASTE, 0, (LPARAM)new std::wstring(std::move(text)));
        } catch (std::exception& e) {
            log_error(std::string("Transcription exception: ") + e.what());
            std::string s = e.what();
            PostMessage(wnd, WM_APP_ERROR, 0,
                (LPARAM)new std::wstring(s.begin(), s.end()));
        }
    }).detach();
}

static void on_paste(std::wstring* raw) {
    std::unique_ptr<std::wstring> text(raw);
    G.state = IDLE;
    status_refresh();
    log_info("on_paste: text_len=" + std::to_string(text->size()) + " State → IDLE");
    overlay_hide();

    // Write to clipboard
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        size_t bytes = (text->size() + 1) * sizeof(wchar_t);
        HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hm) {
            memcpy(GlobalLock(hm), text->c_str(), bytes);
            GlobalUnlock(hm);
            SetClipboardData(CF_UNICODETEXT, hm);
            log_info("Clipboard set OK");
        } else {
            log_error("GlobalAlloc failed");
        }
        CloseClipboard();
    } else {
        log_error("OpenClipboard failed, error=" + std::to_string(GetLastError()));
    }

    // Restore target window then Ctrl+V
    if (G.target && IsWindow(G.target)) {
        SetForegroundWindow(G.target);
        Sleep(80);
    }
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    if (g_auto_enter) {
        Sleep(200);
        keybd_event(VK_RETURN, 0, 0, 0);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
        log_info("Ctrl+V + Enter sent (auto-enter ON)");
    } else {
        log_info("Ctrl+V sent (auto-enter OFF)");
    }
}

static void on_error(std::wstring* raw) {
    std::unique_ptr<std::wstring> msg(raw);
    g_recording = false;
    G.state = IDLE;
    status_refresh();
    std::string narrow(msg->begin(), msg->end());
    log_error("on_error: " + narrow);
    overlay_show(L"Error", msg->c_str());
    HWND wnd = G.overlay;
    std::thread([wnd]{ Sleep(2600); PostMessage(wnd, WM_APP_HIDE, 0, 0); }).detach();
}

// ════════════════════════════════════════════════════════════════════════════
// Overlay window procedure
// ════════════════════════════════════════════════════════════════════════════
static HFONT g_font_main = NULL, g_font_sub = NULL;

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_font_main = CreateFont(-64, 0,0,0, FW_BOLD, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        g_font_sub  = CreateFont(-24, 0,0,0, FW_NORMAL, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        log_info("Overlay window created");
        return 0;

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(17,17,17));
        FillRect((HDC)wp, &rc, bg);
        DeleteObject(bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        SetBkMode(dc, TRANSPARENT);

        // Main text — upper 55% of screen, vertically centered in that zone
        SelectObject(dc, g_font_main);
        SetTextColor(dc, RGB(245,245,245));
        RECT r1 = rc; r1.bottom = rc.top + (rc.bottom - rc.top) * 55 / 100;
        DrawText(dc, G.main_text.c_str(), -1, &r1, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        // Sub text — below
        SelectObject(dc, g_font_sub);
        SetTextColor(dc, RGB(201,201,201));
        RECT r2 = rc; r2.top = rc.top + (rc.bottom - rc.top) * 55 / 100 + 16;
        DrawText(dc, G.sub_text.c_str(), -1, &r2, DT_CENTER|DT_TOP|DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }

    // App messages dispatched here on the main thread
    case WM_APP_START:  on_start_recording();        return 0;
    case WM_APP_STOP:   on_stop_recording();         return 0;
    case WM_APP_PASTE:  on_paste((std::wstring*)lp); return 0;
    case WM_APP_ERROR:  on_error((std::wstring*)lp); return 0;
    case WM_APP_HIDE:   overlay_hide();              return 0;
    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP) {
            // Right-click: show context menu with Exit option
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"退出 VoiceInput");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            if (cmd == 1) {
                log_info("Exit via tray menu");
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    case WM_APP + 7:   // open config dialog (posted by status window gear click)
        open_config_dialog((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
        return 0;

    case WM_CLOSE:
        log_info("WM_CLOSE received — shutting down");
        if (g_hook_tid) PostThreadMessage(g_hook_tid, WM_QUIT, 0, 0);
        tray_remove();
        if (G.status_wnd) { DestroyWindow(G.status_wnd); G.status_wnd = NULL; }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_font_main) DeleteObject(g_font_main);
        if (g_font_sub)  DeleteObject(g_font_sub);
        log_info("Overlay destroyed, posting WM_QUIT");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════════════════════════
// Entry point
// ════════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // DPI awareness — prevents blurry fonts on high-DPI displays
    SetProcessDPIAware();

    // Write a separator so multiple runs are easy to distinguish in the log
    log_info("════════════════════════════════════════");
    log_info("VoiceInput starting up");
    log_info("════════════════════════════════════════");

    // Log working directory so paths are unambiguous
    {
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        log_info(std::string("Working directory: ") + cwd);
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    log_info("COM initialized");

    // Load config
    try { G.config = load_config("config.json"); }
    catch (std::exception& e) {
        MessageBoxA(NULL, e.what(), "VoiceInput — Config Error", MB_ICONERROR|MB_OK);
        CoUninitialize(); return 1;
    }

    try { G.asr = new AsrClient(G.config); }
    catch (std::exception& e) {
        log_error(std::string("AsrClient init failed: ") + e.what());
        MessageBoxA(NULL, e.what(), "VoiceInput — Init Error", MB_ICONERROR|MB_OK);
        CoUninitialize(); return 1;
    }

    // Parse hotkeys
    g_hotkey_start = parse_hotkey(G.config.hotkey);
    g_hotkey_stop  = parse_hotkey(G.config.hotkey_stop);
    log_info("Hotkey start: " + hotkey_to_string(g_hotkey_start));
    log_info("Hotkey stop:  " + hotkey_to_string(g_hotkey_stop));

    // Register overlay window class
    WNDCLASSEX wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = CreateSolidBrush(RGB(17,17,17));
    wc.lpszClassName = OVERLAY_CLASS;
    if (!RegisterClassEx(&wc)) {
        log_error("RegisterClassEx failed, error=" + std::to_string(GetLastError()));
    } else {
        log_info("Window class registered");
    }

    // Register status mini-window class
    WNDCLASSEX wcs = { sizeof(wcs) };
    wcs.style         = CS_HREDRAW | CS_VREDRAW;
    wcs.lpfnWndProc   = StatusProc;
    wcs.hInstance     = hInst;
    wcs.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcs.lpszClassName = STATUS_CLASS;
    RegisterClassEx(&wcs);

    // Register config dialog class
    WNDCLASSEX wcc = { sizeof(wcc) };
    wcc.style         = CS_HREDRAW | CS_VREDRAW;
    wcc.lpfnWndProc   = ConfigProc;
    wcc.hInstance     = hInst;
    wcc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcc.lpszClassName = CONFIG_CLASS;
    RegisterClassEx(&wcc);

    // Full virtual-screen coverage (multi-monitor friendly)
    int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    log_info("Virtual screen: x=" + std::to_string(sx) + " y=" + std::to_string(sy) +
             " w=" + std::to_string(sw) + " h=" + std::to_string(sh));

    G.overlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"VoiceInput",
        WS_POPUP,
        sx, sy, sw, sh,
        NULL, NULL, hInst, NULL
    );
    if (!G.overlay) {
        DWORD err = GetLastError();
        log_error("CreateWindowEx failed, error=" + std::to_string(err));
        MessageBoxA(NULL, "CreateWindow failed.", "VoiceInput", MB_ICONERROR|MB_OK);
        CoUninitialize(); return 1;
    }
    log_info("Overlay window handle: " + std::to_string((uintptr_t)G.overlay));

    // 88% opacity, same as original
    SetLayeredWindowAttributes(G.overlay, 0, (BYTE)(0.88 * 255), LWA_ALPHA);

    // Create status mini-window — top-right corner of primary screen
    {
        int scr_w = GetSystemMetrics(SM_CXSCREEN);
        int pos_x = scr_w - STATUS_W - 10;
        int pos_y = 10;
        G.status_wnd = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            STATUS_CLASS, L"VoiceInput Status",
            WS_POPUP | WS_VISIBLE,
            pos_x, pos_y, STATUS_W, STATUS_H,
            NULL, NULL, hInst, NULL
        );
        log_info("Status window created at (" + std::to_string(pos_x) +
                 "," + std::to_string(pos_y) + ")");
    }

    // Add tray icon so the user can see the program is running
    tray_add(hInst);

    // Show startup balloon
    tray_balloon(L"VoiceInput 已就绪", L"点击右上角 ⚙ 可配置快捷键和 API");
    log_info("Startup balloon shown");

    // Start keyboard hook in its own thread
    std::thread(hook_thread_fn).detach();
    log_info("Hook thread launched");

    // Main message loop
    log_info("Entering message loop");
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    log_info("Message loop exited");
    delete G.asr;
    CoUninitialize();
    log_info("VoiceInput shutdown complete");
    return 0;
}
