#pragma once
// A small blocking HTTP/1.1 server for one browser: one request per connection, answered in full, then closed.
// Enough for the ReefDO page and curl; not a web server.
#include <cstdint>
#include <functional>
#include <string>

namespace sim
{

struct HttpRequest
{
    std::string method; // "GET", "PUT", "POST", "OPTIONS"
    std::string path;
    std::string query; // without the '?'
    std::string body;
    std::string authorization;
};

struct HttpReply
{
    int status = 200;
    std::string type = "text/plain";
    std::string disposition; // Content-Disposition, when set
    std::string cache = "no-store";
    std::string body;
};

class HttpServer
{
public:
    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // pLan: every interface (a phone on the LAN); otherwise only this computer.
    bool Listen(uint16_t pPort, bool pLan);
    // Waits up to pTimeoutMs for a connection and answers it with pHandler.
    void Poll(int pTimeoutMs, const std::function<HttpReply(const HttpRequest&)>& pHandler);

private:
    void Serve(uintptr_t pClient, const std::function<HttpReply(const HttpRequest&)>& pHandler);

    uintptr_t mListen;
    bool mStarted = false;
};

} // namespace sim
