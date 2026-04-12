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
#include <cmath>
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
static const wchar_t*  CONFIG_CLASS    = L"VoiceInputConfig";
static const wchar_t*  WAVE_CLASS      = L"VoiceInputWave";
static const int       WAVE_W          = 540;
static const int       WAVE_H          = 86;

// Custom window messages
enum {
    WM_APP_START  = WM_APP + 1,   // hook → main: begin recording
    WM_APP_STOP   = WM_APP + 2,   // hook → main: stop recording
    WM_APP_PASTE  = WM_APP + 3,   // worker → main: lParam = new std::wstring*
    WM_APP_ERROR  = WM_APP + 4,   // any   → main: lParam = new std::wstring*
    WM_APP_HIDE   = WM_APP + 5,   // timer → main: hide overlay
    WM_APP_TRAY   = WM_APP + 6,   // tray icon messages
    WM_APP_CANCEL = WM_APP + 8,   // hook → main: cancel recording (discard audio)
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

    // Wave indicator + config dialog
    HWND wave_wnd  = NULL;
    HWND config_wnd = NULL;

    // Tray icon
    NOTIFYICONDATA nid = {};
    bool tray_added    = false;
} G;

static bool   g_auto_enter      = true;

// ════════════════════════════════════════════════════════════════════════════
// Wave indicator window (small floating bar above taskbar during recording)
// ════════════════════════════════════════════════════════════════════════════
static HFONT    g_font_wave       = NULL;
static HFONT    g_font_wave_sm    = NULL;
static int      g_wave_tick       = 0;
static UINT_PTR g_wave_timer_id   = 0;

// Per-bar random phase offsets for organic movement
static double   g_bar_phase[48]   = {};
static bool     g_bar_phase_init  = false;

static void wave_init_phases() {
    if (g_bar_phase_init) return;
    srand((unsigned)GetTickCount());
    for (int i = 0; i < 48; i++)
        g_bar_phase[i] = (rand() % 1000) / 1000.0 * 6.28318;
    g_bar_phase_init = true;
}

static void wave_set_round_rgn(HWND hwnd) {
    HRGN rgn = CreateRoundRectRgn(0, 0, WAVE_W + 1, WAVE_H + 1, 24, 24);
    SetWindowRgn(hwnd, rgn, TRUE);
}

// Interpolate two COLORREFs by t (0..1)
static COLORREF lerp_color(COLORREF a, COLORREF b, double t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    int r = GetRValue(a) + (int)((GetRValue(b) - GetRValue(a)) * t);
    int g = GetGValue(a) + (int)((GetGValue(b) - GetGValue(a)) * t);
    int bl= GetBValue(a) + (int)((GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

static LRESULT CALLBACK WaveProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_font_wave = CreateFont(-28, 0,0,0, FW_BOLD, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        g_font_wave_sm = CreateFont(-15, 0,0,0, FW_NORMAL, 0,0,0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
        wave_set_round_rgn(hwnd);
        wave_init_phases();
        return 0;

    case WM_ERASEBKGND: return 1;

    case WM_TIMER:
        g_wave_tick++;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        const int W = rc.right, H = rc.bottom;
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HBITMAP old_bmp = (HBITMAP)SelectObject(mdc, bmp);

        // Clean dark background
        HBRUSH bg = CreateSolidBrush(RGB(8, 8, 18));
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mdc, TRANSPARENT);

        bool is_rec = (G.state == RECORDING);
        double t = g_wave_tick * 0.033;
        double pulse = is_rec
            ? 0.75 + 0.25 * sin(t * 4.0)
            : 0.5  + 0.5  * sin(t * 1.5);

        // Layout: 48 thin bars for fine-grained look
        const int bar_count = 48;
        const int bars_x0   = 10;
        const int bars_x1   = W - 122;
        const int bar_step  = (bars_x1 - bars_x0) / bar_count;  // ~8px
        const int bar_w     = bar_step - 2;                      // ~6px, fine & clean
        const int cy        = H / 2;
        const int max_h     = H / 2 - 5;

        // Center line — subtle, anchors the animation
        {
            HPEN cl = CreatePen(PS_SOLID, 1, RGB(22, 22, 40));
            SelectObject(mdc, cl);
            MoveToEx(mdc, bars_x0, cy, NULL);
            LineTo(mdc, bars_x1, cy);
            DeleteObject(cl);
        }

        // Neon gradient: teal→blue→violet (recording) or blue→indigo (transcribing)
        COLORREF c_left  = is_rec ? RGB(0, 220, 160) : RGB(30, 120, 255);
        COLORREF c_mid   = is_rec ? RGB(0, 140, 255) : RGB(80,  60, 255);
        COLORREF c_right = is_rec ? RGB(180, 0, 255) : RGB(160, 30, 255);

        for (int i = 0; i < bar_count; i++) {
            double frac = (double)i / (bar_count - 1);
            COLORREF bar_col = (frac < 0.5)
                ? lerp_color(c_left, c_mid, frac * 2.0)
                : lerp_color(c_mid, c_right, (frac - 0.5) * 2.0);

            double h_norm;
            if (is_rec) {
                double s1 = sin(t * 5.0  + g_bar_phase[i]);
                double s2 = sin(t * 8.0  + i * 0.4);
                double s3 = sin(t * 12.0 + i * 0.7 + g_bar_phase[i] * 0.5);
                h_norm = 0.05 + 0.55*(0.5+0.5*s1) + 0.25*(0.5+0.5*s2) + 0.15*(0.5+0.5*s3);
            } else {
                double s1 = sin(t * 1.8 + g_bar_phase[i]);
                double s2 = sin(t * 3.0 + i * 0.3);
                h_norm = 0.05 + 0.50*(0.5+0.5*s1) + 0.25*(0.5+0.5*s2);
            }
            h_norm *= (0.85 + 0.15 * pulse);
            int h = (int)(h_norm * max_h);
            if (h < 2) h = 2;

            int bx = bars_x0 + i * bar_step;

            // Subtle glow shadow (one pass, small expansion)
            COLORREF glow = lerp_color(RGB(8,8,18), bar_col, 0.20);
            HBRUSH gbr = CreateSolidBrush(glow);
            HPEN   gpn = CreatePen(PS_SOLID, 0, glow);
            SelectObject(mdc, gbr); SelectObject(mdc, gpn);
            RoundRect(mdc, bx-1, cy-h-2, bx+bar_w+1, cy+h+2, 4, 4);
            DeleteObject(gbr); DeleteObject(gpn);

            // Bar — clean, fully-saturated, rounded ends
            HBRUSH br = CreateSolidBrush(bar_col);
            HPEN   pn = CreatePen(PS_SOLID, 0, bar_col);
            SelectObject(mdc, br); SelectObject(mdc, pn);
            RoundRect(mdc, bx, cy-h, bx+bar_w, cy+h, bar_w, bar_w);
            DeleteObject(br); DeleteObject(pn);
        }

        // Right: state label + stop hint
        {
            const wchar_t* label = is_rec ? L"聆听中" : L"识别中";
            SelectObject(mdc, g_font_wave);
            COLORREF txt_col = is_rec ? RGB(255, 65, 65) : RGB(50, 150, 255);
            SetTextColor(mdc, txt_col);
            RECT rT = { W - 118, 2, W - 4, H/2 + 4 };
            DrawText(mdc, label, -1, &rT, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

            std::wstring hk;
            { std::string s = hotkey_to_string(g_hotkey_stop); hk = std::wstring(s.begin(), s.end()); }
            std::wstring hint = L"按 " + hk + L" 停止";
            SelectObject(mdc, g_font_wave_sm);
            SetTextColor(mdc, RGB(90, 90, 120));
            RECT rH = { W - 118, H/2 + 2, W - 4, H - 2 };
            DrawText(mdc, hint.c_str(), -1, &rH, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }

        BitBlt(dc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old_bmp);
        DeleteObject(bmp); DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (g_font_wave)    { DeleteObject(g_font_wave);    g_font_wave    = NULL; }
        if (g_font_wave_sm) { DeleteObject(g_font_wave_sm); g_font_wave_sm = NULL; }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void wave_show() {
    if (!G.wave_wnd) return;
    g_wave_tick = 0;
    wave_init_phases();
    InvalidateRect(G.wave_wnd, NULL, TRUE);
    ShowWindow(G.wave_wnd, SW_SHOWNA);
    SetWindowPos(G.wave_wnd, HWND_TOPMOST, 0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    // ~30fps animation
    g_wave_timer_id = SetTimer(G.wave_wnd, 1, 33, NULL);
}

static void wave_hide() {
    if (!G.wave_wnd) return;
    if (g_wave_timer_id) { KillTimer(G.wave_wnd, 1); g_wave_timer_id = 0; }
    ShowWindow(G.wave_wnd, SW_HIDE);
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

// Forward declarations for functions defined after this section
static void tray_update_icon();

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
        return 0;
    }

    case WM_COMMAND:
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
            tray_update_icon();  // refresh tooltip with new hotkey
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
// System tray icon — dynamic color per state, full context menu
// ════════════════════════════════════════════════════════════════════════════
static HICON g_tray_icons[3] = {}; // IDLE, RECORDING, TRANSCRIBING

// Create a simple 16×16 filled-circle icon with the given color
static HICON create_dot_icon(COLORREF col) {
    const int SZ = 16;
    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP color_bmp = CreateCompatibleBitmap(sdc, SZ, SZ);
    SelectObject(mdc, color_bmp);

    // Fill background with black (transparent via mask)
    RECT rc = {0,0,SZ,SZ};
    HBRUSH black = CreateSolidBrush(RGB(0,0,0));
    FillRect(mdc, &rc, black);
    DeleteObject(black);

    // Draw filled circle
    HBRUSH br = CreateSolidBrush(col);
    HPEN   pn = CreatePen(PS_SOLID, 0, col);
    SelectObject(mdc, br); SelectObject(mdc, pn);
    Ellipse(mdc, 2, 2, SZ-2, SZ-2);
    DeleteObject(br); DeleteObject(pn);

    // Mask: black = opaque, white = transparent
    HDC mdc2 = CreateCompatibleDC(sdc);
    HBITMAP mask_bmp = CreateBitmap(SZ, SZ, 1, 1, NULL);
    SelectObject(mdc2, mask_bmp);
    HBRUSH white = CreateSolidBrush(RGB(255,255,255));
    FillRect(mdc2, &rc, white);
    DeleteObject(white);
    HBRUSH bk = CreateSolidBrush(RGB(0,0,0));
    HPEN   pk = CreatePen(PS_SOLID, 0, RGB(0,0,0));
    SelectObject(mdc2, bk); SelectObject(mdc2, pk);
    Ellipse(mdc2, 2, 2, SZ-2, SZ-2);
    DeleteObject(bk); DeleteObject(pk);

    DeleteDC(mdc); DeleteDC(mdc2);
    ReleaseDC(NULL, sdc);

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask_bmp;
    ii.hbmColor = color_bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon;
}

static void tray_init_icons() {
    g_tray_icons[IDLE]         = create_dot_icon(RGB(120, 120, 120));
    g_tray_icons[RECORDING]    = create_dot_icon(RGB(255, 70,  70));
    g_tray_icons[TRANSCRIBING] = create_dot_icon(RGB(70,  140, 255));
}

static void tray_update_icon() {
    if (!G.tray_added) return;
    G.nid.hIcon = g_tray_icons[G.state];
    // Update tooltip with current state
    const wchar_t* st = L"待机";
    if (G.state == RECORDING)    st = L"录音中";
    if (G.state == TRANSCRIBING) st = L"识别中";
    std::wstring hk;
    { std::string s = hotkey_to_string(g_hotkey_start); hk = std::wstring(s.begin(), s.end()); }
    swprintf_s(G.nid.szTip, L"VoiceInput — %s (%s)", st, hk.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &G.nid);
}

#define IDM_AUTO_SEND  4001
#define IDM_SETTINGS   4002
#define IDM_EXIT       4003

static void tray_show_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_auto_enter ? MF_CHECKED : 0),
                IDM_AUTO_SEND, L"自动发送");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出 VoiceInput");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(G.overlay);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, G.overlay, NULL);
    DestroyMenu(menu);
    switch (cmd) {
    case IDM_AUTO_SEND:
        g_auto_enter = !g_auto_enter;
        log_info(std::string("Auto-enter: ") + (g_auto_enter ? "ON" : "OFF"));
        break;
    case IDM_SETTINGS:
        PostMessage(G.overlay, WM_APP + 7, 0, 0);
        break;
    case IDM_EXIT:
        log_info("Exit via tray menu");
        PostMessage(G.overlay, WM_CLOSE, 0, 0);
        break;
    }
}

static void tray_add(HINSTANCE hInst) {
    G.nid.cbSize           = sizeof(G.nid);
    G.nid.hWnd             = G.overlay;
    G.nid.uID              = 1;
    G.nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    G.nid.uCallbackMessage = WM_APP_TRAY;
    G.nid.hIcon            = g_tray_icons[IDLE];
    wcscpy_s(G.nid.szTip, L"VoiceInput — 待机");
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
    for (auto& ic : g_tray_icons) { if (ic) { DestroyIcon(ic); ic = NULL; } }
}

// ════════════════════════════════════════════════════════════════════════════
// Keyboard hook
// ════════════════════════════════════════════════════════════════════════════
static HHOOK              g_hook     = NULL;
static DWORD              g_hook_tid = 0;
static std::atomic<bool>  g_recording{false};
static std::atomic<bool>  g_hotkey_consumed{false};


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
            g_recording = false;
            g_hotkey_consumed = false;
            PostMessage(G.overlay, WM_APP_CANCEL, 0, 0);
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
// Overlay helpers (overlay kept for message routing, never shown visually)
// ════════════════════════════════════════════════════════════════════════════
static int       g_ov_tick      = 0;
static UINT_PTR  g_ov_timer_id  = 0;
static const COLORREF OV_KEY    = RGB(1, 1, 1);  // color-key = transparent

// Update overlay alpha to create pulsing glow effect
static void overlay_update_pulse() {
    if (!G.overlay) return;
    bool is_rec = (G.state == RECORDING);
    double t = g_ov_tick * 0.033;
    // Fast pulse when recording, slow breathe when transcribing
    double pulse = is_rec
        ? 0.4 + 0.6 * (0.5 + 0.5 * sin(t * 4.5))
        : 0.25 + 0.75 * (0.5 + 0.5 * sin(t * 1.6));
    // Alpha range: glow visible but screen content stays clear
    int alpha = 30 + (int)(pulse * 90);
    if (alpha > 180) alpha = 180;
    SetLayeredWindowAttributes(G.overlay, 0, (BYTE)alpha, LWA_ALPHA);
    InvalidateRect(G.overlay, NULL, TRUE);
}

static void overlay_show(const wchar_t*, const wchar_t*) {
    g_ov_tick = 0;
    overlay_update_pulse();
    ShowWindow(G.overlay, SW_SHOWNA);
    SetWindowPos(G.overlay, HWND_TOPMOST, 0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    if (!g_ov_timer_id)
        g_ov_timer_id = SetTimer(G.overlay, 2, 33, NULL);
}

static void overlay_hide() {
    if (g_ov_timer_id) { KillTimer(G.overlay, 2); g_ov_timer_id = 0; }
    ShowWindow(G.overlay, SW_HIDE);
}

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
    tray_update_icon();
    wave_show();
    overlay_show(NULL, NULL);
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
    tray_update_icon();
    InvalidateRect(G.wave_wnd, NULL, TRUE);  // refresh wave to show "识别中"
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
    tray_update_icon();
    wave_hide();
    overlay_hide();
    log_info("on_paste: text_len=" + std::to_string(text->size()) + " State → IDLE");

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
        // Attach thread input to ensure SetForegroundWindow succeeds
        DWORD target_tid = GetWindowThreadProcessId(G.target, NULL);
        DWORD our_tid    = GetCurrentThreadId();
        bool attached = false;
        if (target_tid != our_tid) {
            attached = AttachThreadInput(our_tid, target_tid, TRUE) != 0;
        }
        SetForegroundWindow(G.target);
        // Wait until target window is actually foreground (up to 500ms)
        for (int i = 0; i < 10; i++) {
            Sleep(50);
            if (GetForegroundWindow() == G.target) break;
        }
        if (attached) AttachThreadInput(our_tid, target_tid, FALSE);
        log_info("Target window restored, fg=" +
            std::to_string(GetForegroundWindow() == G.target));
    }
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    if (g_auto_enter) {
        Sleep(350);  // give target app time to process paste
        keybd_event(VK_RETURN, 0, 0, 0);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
        log_info("Ctrl+V + Enter sent (auto-enter ON)");
    } else {
        log_info("Ctrl+V sent (auto-enter OFF)");
    }
}

static void on_cancel_recording() {
    log_info("on_cancel_recording");
    g_recording = false;
    if (G.state == RECORDING) {
        G.recorder.stop();   // discard audio
    }
    G.state = IDLE;
    tray_update_icon();
    wave_hide();
    overlay_hide();
    tray_balloon(L"VoiceInput", L"已取消");
}

static void on_error(std::wstring* raw) {
    std::unique_ptr<std::wstring> msg(raw);
    g_recording = false;
    G.state = IDLE;
    tray_update_icon();
    wave_hide();
    overlay_hide();
    std::string narrow(msg->begin(), msg->end());
    log_error("on_error: " + narrow);
    tray_balloon(L"VoiceInput 错误", msg->c_str());
}

// ════════════════════════════════════════════════════════════════════════════
// Overlay window procedure
// ════════════════════════════════════════════════════════════════════════════

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        log_info("Overlay window created");
        return 0;

    case WM_ERASEBKGND: return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        // Memory DC to avoid flicker
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HBITMAP old_bmp = (HBITMAP)SelectObject(mdc, bmp);

        bool is_rec = (G.state == RECORDING);
        double t = g_ov_tick * 0.033;

        // Fill entire screen with a very subtle dark tint (NOT the color key)
        // This gives a barely-there dimming effect
        HBRUSH tint = CreateSolidBrush(RGB(3, 3, 6));
        FillRect(mdc, &rc, tint);
        DeleteObject(tint);

        // === Edge glow: smooth gradient, 60 strips, wider reach ===
        const int GLOW_W = 200;
        const int STRIPS = 60;

        // Glow base color + animated color shift
        COLORREF glow_core, glow_edge;
        if (is_rec) {
            double sh = 0.5 + 0.5 * sin(t * 3.0);
            double sh2 = 0.5 + 0.5 * sin(t * 7.0);
            glow_core = RGB((int)(255*(0.9+0.1*sh)), (int)(50+40*sh), (int)(20+30*sh2));
            glow_edge = RGB((int)(120+60*sh2), (int)(15+20*sh), 8);
        } else {
            double sh = 0.5 + 0.5 * sin(t * 1.5);
            glow_core = RGB((int)(30+50*sh), (int)(80+50*sh), (int)(240+15*(1-sh)));
            glow_edge = RGB((int)(10+20*sh), (int)(20+25*sh), (int)(100+40*(1-sh)));
        }

        for (int s = 0; s < STRIPS; s++) {
            double frac = 1.0 - (double)s / STRIPS;
            // Cubic falloff for smooth, wide glow
            double intensity = frac * frac * frac;
            // Blend between edge color (at screen border) and core color (peak)
            double peak_frac = frac * frac;
            COLORREF col = lerp_color(
                lerp_color(RGB(3,3,6), glow_edge, intensity),
                glow_core,
                peak_frac * intensity
            );
            int strip_w = GLOW_W / STRIPS + 1;
            int x = s * (GLOW_W / STRIPS);

            HBRUSH br = CreateSolidBrush(col);
            RECT rl = { x, 0, x + strip_w, H };
            FillRect(mdc, &rl, br);
            RECT rr = { W - x - strip_w, 0, W - x, H };
            FillRect(mdc, &rr, br);
            DeleteObject(br);
        }

        // === Top and bottom thin glow lines for extra sci-fi feel ===
        {
            COLORREF line_col = is_rec
                ? lerp_color(RGB(3,3,6), RGB(200, 50, 30), 0.5 + 0.5 * sin(t * 4.0))
                : lerp_color(RGB(3,3,6), RGB(40, 80, 220), 0.5 + 0.5 * sin(t * 2.0));
            for (int dy = 0; dy < 4; dy++) {
                double dim = 1.0 - dy * 0.3;
                COLORREF c = lerp_color(RGB(3,3,6), line_col, dim * 0.6);
                HPEN pen = CreatePen(PS_SOLID, 1, c);
                SelectObject(mdc, pen);
                // Top line
                MoveToEx(mdc, 0, dy, NULL); LineTo(mdc, W, dy);
                // Bottom line
                MoveToEx(mdc, 0, H - 1 - dy, NULL); LineTo(mdc, W, H - 1 - dy);
                DeleteObject(pen);
            }
        }

        BitBlt(dc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old_bmp);
        DeleteObject(bmp); DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == 2) {
            g_ov_tick++;
            overlay_update_pulse();
        }
        return 0;

    // App messages dispatched here on the main thread
    case WM_APP_START:  on_start_recording();        return 0;
    case WM_APP_STOP:   on_stop_recording();         return 0;
    case WM_APP_CANCEL: on_cancel_recording();       return 0;
    case WM_APP_PASTE:  on_paste((std::wstring*)lp); return 0;
    case WM_APP_ERROR:  on_error((std::wstring*)lp); return 0;
    case WM_APP_HIDE:   overlay_hide();              return 0;
    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            tray_show_menu();
        }
        return 0;
    case WM_APP + 7:   // open config dialog (posted by status window gear click)
        open_config_dialog((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE));
        return 0;

    case WM_CLOSE:
        log_info("WM_CLOSE received — shutting down");
        if (g_hook_tid) PostThreadMessage(g_hook_tid, WM_QUIT, 0, 0);
        tray_remove();
        wave_hide();
        if (G.wave_wnd) { DestroyWindow(G.wave_wnd); G.wave_wnd = NULL; }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_ov_timer_id) { KillTimer(hwnd, 2); g_ov_timer_id = 0; }
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
    wc.hbrBackground = CreateSolidBrush(RGB(1,1,1));  // color-key = transparent
    wc.lpszClassName = OVERLAY_CLASS;
    if (!RegisterClassEx(&wc)) {
        log_error("RegisterClassEx failed, error=" + std::to_string(GetLastError()));
    } else {
        log_info("Window class registered");
    }

    // Register wave indicator window class
    WNDCLASSEX wcw = { sizeof(wcw) };
    wcw.style         = CS_HREDRAW | CS_VREDRAW;
    wcw.lpfnWndProc   = WaveProc;
    wcw.hInstance     = hInst;
    wcw.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcw.lpszClassName = WAVE_CLASS;
    RegisterClassEx(&wcw);

    // Register config dialog class
    WNDCLASSEX wcc = { sizeof(wcc) };
    wcc.style         = CS_HREDRAW | CS_VREDRAW;
    wcc.lpfnWndProc   = ConfigProc;
    wcc.hInstance     = hInst;
    wcc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcc.lpszClassName = CONFIG_CLASS;
    RegisterClassEx(&wcc);

    // Primary monitor only (not virtual screen — avoids bleeding into other monitors)
    int sx = 0, sy = 0;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    log_info("Primary screen: w=" + std::to_string(sw) + " h=" + std::to_string(sh));

    G.overlay = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
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

    // Start fully transparent; overlay_show sets alpha when recording starts
    SetLayeredWindowAttributes(G.overlay, 0, 0, LWA_ALPHA);

    // Create wave indicator window — centered, ~200px above taskbar, hidden initially
    {
        int scr_w = GetSystemMetrics(SM_CXSCREEN);
        int scr_h = GetSystemMetrics(SM_CYSCREEN);
        RECT tb_rc = {};
        HWND taskbar = FindWindow(L"Shell_TrayWnd", NULL);
        int taskbar_h = 40;
        if (taskbar && GetWindowRect(taskbar, &tb_rc))
            taskbar_h = tb_rc.bottom - tb_rc.top;
        int pos_x = (scr_w - WAVE_W) / 2;
        int pos_y = scr_h - taskbar_h - 350 - WAVE_H;
        G.wave_wnd = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            WAVE_CLASS, L"VoiceInput Wave",
            WS_POPUP,   // not WS_VISIBLE — starts hidden
            pos_x, pos_y, WAVE_W, WAVE_H,
            NULL, NULL, hInst, NULL
        );
        log_info("Wave window created at (" + std::to_string(pos_x) +
                 "," + std::to_string(pos_y) + ")");
    }

    // Create tray icon with state-colored dots
    tray_init_icons();
    tray_add(hInst);

    // Show startup balloon
    tray_balloon(L"VoiceInput 已就绪", L"右键托盘图标可配置快捷键和 API");
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
