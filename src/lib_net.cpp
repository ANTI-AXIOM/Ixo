// net: an HTTP client (WinHTTP on Windows, sockets for http:// elsewhere, curl
// for https:// elsewhere) and a small HTTP server whose requests arrive in a flow.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "builtins.hpp"
#include "json.hpp"
#include "par.hpp"
#include "process.hpp"
#include "vm.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

namespace {

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
bool netInit() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
#else
using Sock = int;
constexpr Sock kBadSock = -1;
void closeSock(Sock s) { ::close(s); }
bool netInit() { return true; }
#endif

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

struct HttpResponse {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// "HTTP/1.1 200 OK\r\nA: b\r\n..." -> status and headers
void parseHead(const std::string& head, HttpResponse& r) {
    size_t lineEnd = head.find("\r\n");
    std::string first = head.substr(0, lineEnd);
    size_t sp = first.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(first.c_str() + sp + 1);
    size_t p = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (p < head.size()) {
        size_t e = head.find("\r\n", p);
        std::string line = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            v.erase(0, v.find_first_not_of(' '));
            r.headers.push_back({lower(line.substr(0, colon)), v});
        }
        if (e == std::string::npos) break;
        p = e + 2;
    }
}

// ------------------------------------------------------------------ client

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool httpRequest(const std::string& method, const std::string& url, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse& out, std::string& err) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096], extra[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        err = "invalid URL: " + url;
        return false;
    }
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET s = WinHttpOpen(L"ixo", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ixo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        err = "can't start WinHTTP";
        return false;
    }
    WinHttpSetTimeouts(s, 10000, 10000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    std::wstring target = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    if (target.empty()) target = L"/";
    HINTERNET r = c ? WinHttpOpenRequest(c, widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    auto cleanup = [&] {
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
    };
    if (!r) {
        err = "can't connect to " + url;
        cleanup();
        return false;
    }
    std::wstring hdrs;
    for (auto& [k, v] : headers) hdrs += widen(k + ": " + v + "\r\n");
    BOOL ok = WinHttpSendRequest(r, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1L,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(r, nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        err = "request to " + url + " failed (WinHTTP error " + std::to_string(e) + ")";
        cleanup();
        return false;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &len, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;
    DWORD hlen = 0;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hlen, WINHTTP_NO_HEADER_INDEX);
    if (hlen > 0) {
        std::wstring raw(hlen / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &hlen,
                                WINHTTP_NO_HEADER_INDEX)) {
            HttpResponse tmp;
            parseHead(narrow(raw), tmp);
            out.headers = tmp.headers;
        }
    }
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
        out.body.append(chunk.data(), got);
    }
    cleanup();
    return true;
}

#else

// http:// with plain sockets (HTTP/1.1, Connection: close)
bool httpPlain(const std::string& method, const std::string& url, const std::string& body,
               const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse& out, std::string& err) {
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostPort = rest.substr(0, slash), target = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string host = hostPort, port = "80";
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "can't find the host " + host;
        return false;
    }
    Sock fd = kBadSock;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kBadSock) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        closeSock(fd);
        fd = kBadSock;
    }
    freeaddrinfo(res);
    if (fd == kBadSock) {
        err = "can't connect to " + hostPort;
        return false;
    }
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: " + hostPort + "\r\nConnection: close\r\nUser-Agent: ixo\r\n";
    for (auto& [k, v] : headers) req += k + ": " + v + "\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t k = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (k <= 0) break;
        sent += (size_t)k;
    }
    std::string resp;
    char buf[8192];
    ssize_t k;
    while ((k = ::recv(fd, buf, sizeof buf, 0)) > 0) resp.append(buf, (size_t)k);
    closeSock(fd);
    size_t he = resp.find("\r\n\r\n");
    if (he == std::string::npos) {
        err = "bad response from " + url;
        return false;
    }
    parseHead(resp.substr(0, he), out);
    std::string raw = resp.substr(he + 4);
    bool chunked = false;
    for (auto& [hk, hv] : out.headers)
        if (hk == "transfer-encoding" && lower(hv).find("chunked") != std::string::npos) chunked = true;
    if (!chunked) {
        out.body = raw;
        return true;
    }
    size_t p = 0;
    while (p < raw.size()) {
        size_t e = raw.find("\r\n", p);
        if (e == std::string::npos) break;
        size_t len = std::strtoul(raw.substr(p, e - p).c_str(), nullptr, 16);
        if (len == 0) break;
        out.body += raw.substr(e + 2, len);
        p = e + 2 + len + 2;
    }
    return true;
}

// https:// through curl (no TLS library is built in)
bool httpCurl(const std::string& method, const std::string& url, const std::string& body,
              const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse& out, std::string& err) {
    std::vector<std::string> argv = {"curl", "-sS", "-L", "-i", "-X", method};
    for (auto& [k, v] : headers) {
        argv.push_back("-H");
        argv.push_back(k + ": " + v);
    }
    if (!body.empty()) {
        argv.push_back("--data-binary");
        argv.push_back("@-");
    }
    argv.push_back("--");
    argv.push_back(url);
    RunResult r = runProcess(argv, body);
    if (!r.started) {
        err = "https:// needs the 'curl' program on this system";
        return false;
    }
    if (r.code != 0) {
        err = "request to " + url + " failed: " + r.err;
        return false;
    }
    // with -L there may be several header blocks: use the last
    size_t p = 0, head = 0;
    while (true) {
        size_t he = r.out.find("\r\n\r\n", p);
        if (he == std::string::npos) break;
        head = p;
        p = he + 4;
        if (r.out.compare(p, 5, "HTTP/") != 0) break;
    }
    parseHead(r.out.substr(head, p - 4 - head), out);
    out.body = r.out.substr(p);
    return true;
}

bool httpRequest(const std::string& method, const std::string& url, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse& out, std::string& err) {
    if (url.rfind("https://", 0) == 0) return httpCurl(method, url, body, headers, out, err);
    return httpPlain(method, url, body, headers, out, err);
}

#endif

std::vector<std::pair<std::string, std::string>> headerArg(VM& vm, Value v) {
    std::vector<std::pair<std::string, std::string>> out;
    if (v.isNil()) return out;
    if (!isObjType(v, OT::Map)) vm.panic("headers must be a map, got " + vm.typeName(v));
    for (auto& [k, x] : as<ObjMap>(v)->entries)
        if (!k.isUndef()) out.push_back({vm.toStr(k), vm.toStr(x)});
    return out;
}

std::string bodyText(VM& vm, Value v, std::vector<std::pair<std::string, std::string>>* headers) {
    if (v.isNil()) return "";
    if (isObjType(v, OT::String)) return as<ObjString>(v)->s;
    if (isObjType(v, OT::Map) || isObjType(v, OT::List)) {
        if (headers) {
            bool has = false;
            for (auto& [k, x] : *headers)
                if (lower(k) == "content-type") has = true;
            if (!has) headers->push_back({"Content-Type", "application/json"});
        }
        return jsonEncode(vm, v, -1);
    }
    return vm.toStr(v);
}

Value doRequest(VM& vm, const std::string& method, const std::string& url, Value bodyV, Value headersV, bool full) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return vm.newError("InvalidUrl", "only http:// and https:// URLs are supported: " + url);
    if (!netInit()) return vm.newError("NetUnavailable", "networking isn't available");
    auto headers = headerArg(vm, headersV);
    std::string body = bodyText(vm, bodyV, &headers);
    HttpResponse r;
    std::string err;
    if (!httpRequest(method, url, body, headers, r, err)) return vm.newError("Http", err);
    if (!full) {
        if (r.status >= 400)
            return vm.newError("Http", method + " " + url + " -> " + std::to_string(r.status) + ": " + r.body.substr(0, 300),
                               Value::integer(r.status));
        return vm.vstr(r.body);
    }
    auto* hm = vm.alloc<ObjMap>();
    for (auto& [k, v] : r.headers) hm->set(vm.vstr(k), vm.vstr(v));
    auto* m = vm.alloc<ObjMap>();
    m->set(vm.vstr("status"), Value::integer(r.status));
    m->set(vm.vstr("body"), vm.vstr(r.body));
    m->set(vm.vstr("headers"), Value::object(hm));
    m->set(vm.vstr("ok"), Value::boolean(r.status >= 200 && r.status < 300));
    return Value::object(m);
}

// ------------------------------------------------------------------ server

struct Pending {
    Sock sock;
};
std::mutex pendingMutex;
std::unordered_map<int64_t, Pending> pending;  // request id -> its connection
std::atomic<int64_t> nextRequestId{1};

struct Server {
    Sock listener = kBadSock;
    std::shared_ptr<SharedChan> chan;
    ParRuntime* rt = nullptr;
    uint64_t requestClass = 0;
    int port = 0;
};

bool readRequest(Sock s, std::string& method, std::string& target, std::vector<std::pair<std::string, std::string>>& headers,
                 std::string& body) {
    std::string data;
    char buf[8192];
    size_t he;
    while ((he = data.find("\r\n\r\n")) == std::string::npos) {
        int k = (int)::recv(s, buf, sizeof buf, 0);
        if (k <= 0 || data.size() > (1 << 20)) return false;
        data.append(buf, (size_t)k);
    }
    std::string head = data.substr(0, he);
    size_t le = head.find("\r\n");
    std::string first = head.substr(0, le);
    size_t a = first.find(' '), b = first.rfind(' ');
    if (a == std::string::npos || b == a) return false;
    method = first.substr(0, a);
    target = first.substr(a + 1, b - a - 1);
    HttpResponse tmp;
    parseHead("X " + head.substr(le == std::string::npos ? head.size() : le), tmp);
    headers = tmp.headers;
    size_t len = 0;
    for (auto& [k, v] : headers)
        if (k == "content-length") len = std::strtoul(v.c_str(), nullptr, 10);
    if (len > (64u << 20)) return false;
    body = data.substr(he + 4);
    while (body.size() < len) {
        int k = (int)::recv(s, buf, sizeof buf, 0);
        if (k <= 0) return false;
        body.append(buf, (size_t)k);
    }
    body.resize(std::min(body.size(), len));
    return true;
}

// A Request(method, path, query, headers, body, id) built as a packet: the
// server thread isn't a VM, so it can't make objects; the receiver decodes it.
Packet requestPacket(uint64_t classId, const std::string& method, const std::string& path, const std::string& query,
                     const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body, int64_t id) {
    Packet p;
    auto strNode = [&](const std::string& s) {
        PNode n;
        n.type = OT::String;
        n.s = s;
        p.nodes.push_back(std::move(n));
        PValue v;
        v.t = VT::Obj;
        v.node = (uint32_t)p.nodes.size() - 1;
        return v;
    };
    PNode cls;
    cls.type = OT::Class;
    cls.id = classId;
    p.nodes.push_back(cls);
    PValue clsV;
    clsV.t = VT::Obj;
    clsV.node = 0;
    PNode inst;
    inst.type = OT::Instance;
    p.nodes.push_back(inst);
    uint32_t instIndex = 1;
    PValue m = strNode(method), pa = strNode(path), q = strNode(query);
    PNode hm;
    hm.type = OT::Map;
    std::vector<std::pair<PValue, PValue>> pairs;
    for (auto& [k, v] : headers) pairs.push_back({strNode(k), strNode(v)});
    hm.pairs = pairs;
    p.nodes.push_back(hm);
    PValue hv;
    hv.t = VT::Obj;
    hv.node = (uint32_t)p.nodes.size() - 1;
    PValue bv = strNode(body);
    PValue idv;
    idv.t = VT::Int;
    idv.i = id;
    p.nodes[instIndex].items = {clsV, m, pa, q, hv, bv, idv};
    PValue root;
    root.t = VT::Obj;
    root.node = instIndex;
    p.roots.push_back(root);
    return p;
}

void serve(std::shared_ptr<Server> sv) {
    while (true) {
        {
            std::lock_guard<std::mutex> lk(sv->chan->m);
            if (sv->chan->closed) break;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sv->listener, &set);
        timeval tv{0, 200000};
        int ready = ::select((int)sv->listener + 1, &set, nullptr, nullptr, &tv);
        if (ready <= 0) continue;
        Sock c = ::accept(sv->listener, nullptr, nullptr);
        if (c == kBadSock) continue;
#ifdef _WIN32
        DWORD timeout = 10000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
#else
        timeval rt{10, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
#endif
        std::string method, target, body;
        std::vector<std::pair<std::string, std::string>> headers;
        if (!readRequest(c, method, target, headers, body)) {
            closeSock(c);
            continue;
        }
        size_t qm = target.find('?');
        std::string path = target.substr(0, qm), query = qm == std::string::npos ? "" : target.substr(qm + 1);
        int64_t id = nextRequestId++;
        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            pending[id] = {c};
        }
        externalSend(sv->chan, requestPacket(sv->requestClass, method, path, query, headers, body, id));
    }
    closeSock(sv->listener);
    sv->rt->addExternal(-1);
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

}  // namespace

NATIVE(net_read) { return doRequest(vm, "GET", vm.argStr(a, 0, "net.read")->s, Value(), n > 1 ? a[1] : Value(), false); }
NATIVE(net_write) {
    return doRequest(vm, "POST", vm.argStr(a, 0, "net.write")->s, a[1], n > 2 ? a[2] : Value(), false);
}
NATIVE(net_request) {
    std::string method = vm.argStr(a, 0, "net.request")->s;
    for (auto& c : method) c = (char)std::toupper((unsigned char)c);
    return doRequest(vm, method, vm.argStr(a, 1, "net.request")->s, n > 2 ? a[2] : Value(), n > 3 ? a[3] : Value(), true);
}

NATIVE(net_listen) {
    int port = (int)vm.argInt(a, 0, "net.listen");
    if (!netInit()) return vm.newError("NetUnavailable", "networking isn't available");
    int ci = vm.builtinIndex("Request");
    if (ci < 0 || !isObjType(vm.builtins[ci].value, OT::Class)) vm.panic("internal: the Request class is missing");
    Sock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) return vm.newError("NetUnavailable", "can't open a socket");
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(s, 64) != 0) {
        closeSock(s);
        return vm.newError("AddressInUse", "can't listen on port " + std::to_string(port) + " (is it in use?)");
    }
    auto sv = std::make_shared<Server>();
    sv->listener = s;
    sv->port = port;
    sv->requestClass = as<ObjClass>(vm.builtins[ci].value)->id;
    sv->rt = vm.ensurePar();
    auto* ch = vm.alloc<ObjChannel>(1 << 20);
    makeChannelShared(vm, ch);
    sv->chan = ch->shared;
    sv->rt->addExternal(1);
    std::thread(serve, sv).detach();
    return Value::object(ch);
}

NATIVE(net_reply) {
    int64_t id;
    if (isObjType(a[0], OT::Instance) && as<ObjInstance>(a[0])->cls->fieldIndex.count(VM::sym("id"))) {
        auto* inst = as<ObjInstance>(a[0]);
        Value v = inst->fields[(size_t)inst->cls->fieldIndex[VM::sym("id")]];
        id = v.isInt() ? v.i : -1;
    } else {
        id = vm.argInt(a, 0, "net.reply");
    }
    int status = n > 1 ? (int)vm.argInt(a, 1, "net.reply") : 200;
    auto headers = headerArg(vm, n > 3 ? a[3] : Value());
    std::string body = bodyText(vm, n > 2 ? a[2] : Value(), &headers);
    Sock s;
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        auto it = pending.find(id);
        if (it == pending.end()) return vm.newError("AlreadyAnswered", "this request was already answered");
        s = it->second.sock;
        pending.erase(it);
    }
    bool hasType = false;
    for (auto& [k, v] : headers)
        if (lower(k) == "content-type") hasType = true;
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    if (!hasType) out += "Content-Type: text/plain; charset=utf-8\r\n";
    for (auto& [k, v] : headers) out += k + ": " + v + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < out.size()) {
        int k = (int)::send(s, out.data() + sent, (int)(out.size() - sent), 0);
        if (k <= 0) break;
        sent += (size_t)k;
    }
    closeSock(s);
    return Value();
}

void registerNet(VM& vm) {
    auto* net = vm.defineNamespace("net", "HTTP: requests, and a small server.");
    vm.nativeModules.push_back("net");
    vm.nsFunc(net, "read", net_read, 1, 2, "fn(str, map?) -> str ! Error", "GET a URL; returns the body.");
    vm.nsFunc(net, "write", net_write, 2, 3, "fn(str, any, map?) -> str ! Error",
              "POST a body (maps and lists are sent as JSON) to a URL; returns the response body.");
    vm.nsFunc(net, "request", net_request, 2, 4, "fn(str, str, any?, map?) -> map ! Error",
              "Any request: net.request(\"PUT\", url, body, headers) -> {status, body, headers, ok}.");
    vm.nsFunc(net, "listen", net_listen, 1, 1, "fn(int) -> flow ! Error",
              "Serves HTTP on a port: requests arrive in the flow (for req <: server); answer with req.respond.");
    vm.nsFunc(net, "reply", net_reply, 2, 4, "fn(any, int, any?, map?) -> nil ! Error",
              "Answers a request: status, body (maps and lists become JSON) and headers.");
}

}  // namespace ixo
