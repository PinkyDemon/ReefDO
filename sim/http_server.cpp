#include "http_server.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace sim
{

namespace
{

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket BAD = INVALID_SOCKET;
void CloseSocket(Socket pS)
{
    closesocket(pS);
}
#else
using Socket = int;
constexpr Socket BAD = -1;
void CloseSocket(Socket pS)
{
    close(pS);
}
#endif

constexpr std::size_t HEAD_MAX = 16 * 1024;
constexpr std::size_t BODY_MAX = 256 * 1024;

const char* Reason(int pStatus)
{
    switch(pStatus)
    {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

std::string Lower(std::string pS)
{
    for(char& c : pS)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return pS;
}

bool SendAll(Socket pS, const std::string& pData)
{
    std::size_t sent = 0;
    while(sent < pData.size())
    {
        const int n = send(pS, pData.data() + sent, static_cast<int>(pData.size() - sent), 0);
        if(n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void Reply(Socket pS, const HttpReply& pR)
{
    std::string head = "HTTP/1.1 " + std::to_string(pR.status) + " " + Reason(pR.status) + "\r\n";
    head += "Content-Type: " + pR.type + "\r\n";
    head += "Content-Length: " + std::to_string(pR.body.size()) + "\r\n";
    head += "Cache-Control: " + pR.cache + "\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Access-Control-Allow-Methods: GET, PUT, POST, OPTIONS\r\n";
    head += "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
    if(!pR.disposition.empty()) head += "Content-Disposition: " + pR.disposition + "\r\n";
    head += "Connection: close\r\n\r\n";
    if(SendAll(pS, head)) SendAll(pS, pR.body);
}

} // namespace

HttpServer::HttpServer()
    : mListen(static_cast<uintptr_t>(BAD))
{
#ifdef _WIN32
    WSADATA wsa;
    mStarted = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    mStarted = true;
#endif
}

HttpServer::~HttpServer()
{
    if(static_cast<Socket>(mListen) != BAD) CloseSocket(static_cast<Socket>(mListen));
#ifdef _WIN32
    if(mStarted) WSACleanup();
#endif
}

bool HttpServer::Listen(uint16_t pPort, bool pLan)
{
    if(!mStarted) return false;
    const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == BAD) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pPort);
    addr.sin_addr.s_addr = htonl(pLan ? INADDR_ANY : INADDR_LOOPBACK);
    if(bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 16) != 0)
    {
        CloseSocket(s);
        return false;
    }
    mListen = static_cast<uintptr_t>(s);
    return true;
}

void HttpServer::Poll(int pTimeoutMs, const std::function<HttpReply(const HttpRequest&)>& pHandler)
{
    const Socket ls = static_cast<Socket>(mListen);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(ls, &set);
    timeval tv{};
    tv.tv_sec = pTimeoutMs / 1000;
    tv.tv_usec = (pTimeoutMs % 1000) * 1000;
    if(select(static_cast<int>(ls + 1), &set, nullptr, nullptr, &tv) <= 0) return;
    const Socket c = accept(ls, nullptr, nullptr);
    if(c == BAD) return;
    Serve(static_cast<uintptr_t>(c), pHandler);
    CloseSocket(c);
}

void HttpServer::Serve(uintptr_t pClient, const std::function<HttpReply(const HttpRequest&)>& pHandler)
{
    const Socket c = static_cast<Socket>(pClient);
#ifdef _WIN32
    const DWORD timeout = 3000;
#else
    const timeval timeout{3, 0};
#endif
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);

    std::string in;
    std::size_t headEnd = std::string::npos;
    char buf[4096];
    while(headEnd == std::string::npos)
    {
        const int n = recv(c, buf, sizeof buf, 0);
        if(n <= 0) return;
        in.append(buf, static_cast<std::size_t>(n));
        headEnd = in.find("\r\n\r\n");
        if(headEnd == std::string::npos && in.size() > HEAD_MAX) return;
    }

    HttpRequest req;
    const std::string head = in.substr(0, headEnd);
    const std::size_t lineEnd = head.find("\r\n");
    const std::string line = head.substr(0, lineEnd);
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if(sp1 == std::string::npos || sp2 == std::string::npos)
        return Reply(c, {400, "text/plain", "", "no-store", "bad request"});
    req.method = line.substr(0, sp1);
    const std::string uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = uri.find('?');
    req.path = uri.substr(0, q);
    if(q != std::string::npos) req.query = uri.substr(q + 1);

    std::size_t length = 0;
    std::size_t at = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while(at < head.size())
    {
        std::size_t end = head.find("\r\n", at);
        if(end == std::string::npos) end = head.size();
        const std::string h = head.substr(at, end - at);
        const std::size_t colon = h.find(':');
        if(colon != std::string::npos)
        {
            const std::string name = Lower(h.substr(0, colon));
            std::string value = h.substr(colon + 1);
            while(!value.empty() && value.front() == ' ')
                value.erase(0, 1);
            if(name == "content-length") length = std::strtoul(value.c_str(), nullptr, 10);
            if(name == "authorization") req.authorization = value;
        }
        at = end + 2;
    }
    if(length > BODY_MAX) return Reply(c, {413, "text/plain", "", "no-store", "body too large"});
    req.body = in.substr(headEnd + 4);
    while(req.body.size() < length)
    {
        const int n = recv(c, buf, sizeof buf, 0);
        if(n <= 0) return;
        req.body.append(buf, static_cast<std::size_t>(n));
    }
    req.body.resize(length);
    Reply(c, pHandler(req));
}

} // namespace sim
