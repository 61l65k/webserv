#include "WebServer.hpp"
#include "CGIHandler.hpp"
#include "ErrorHandler.hpp"
#include "ScopedSocket.hpp"
#include "WebErrors.hpp"
#include <algorithm>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include "Response.hpp"
#include "Request.hpp"

volatile sig_atomic_t WebServer::s_serverRunning = 1;

WebServer::WebServer(WebParser &parser)
    : _epollFd(-1), _parser(parser), _events(MAX_EVENTS)
{
    try
    {
        std::cout << COLOR_GREEN_SERVER << "[ SERVER STARTED ] press Ctrl+C to stop 🏭 \n\n" << COLOR_RESET;
        _serverSockets = createServerSockets(parser.getServers());
        resolveProxyAddresses(parser.getServers());
        _epollFd = epoll_create(1);
        if (_epollFd == -1)
            throw WebErrors::ServerException("Error creating epoll");
        for (const auto& serverSocket : _serverSockets)
            epollController(serverSocket.getFd(), EPOLL_CTL_ADD, EPOLLIN, FdType::SERVER);
    }
    catch (const std::exception& e)
    {
        throw;
    }
}

WebServer::~WebServer()
{
    for (auto& entry : _proxyInfoMap)
    {
        if (entry.second)
        {
            freeaddrinfo(entry.second);
            entry.second = nullptr;
        }
    }
    if (_epollFd != -1)
    {
        close(_epollFd);
        _epollFd = -1;
    }
}

void WebServer::resolveProxyAddresses(const std::vector<Server>& server_confs)
{
    try
    {
        for (const auto& server : server_confs)
        {
            for (const auto& location : server.locations)
            {
                if (location.type == PROXY)
                {
                    std::string proxyHost;
                    std::string proxyPort;

                    size_t colonPos = location.target.rfind(':');
                    if (colonPos != std::string::npos)
                    {
                        proxyHost = location.target.substr(0, colonPos);
                        proxyPort = location.target.substr(colonPos + 1);
                    }
                    else
                    {
                        proxyHost = location.target;
                        proxyPort = std::to_string(server.port);
                    }

                    std::string key = proxyHost + ":" + proxyPort;
                    if (_proxyInfoMap.find(key) == _proxyInfoMap.end())
                    {
                        addrinfo hints{};
                        hints.ai_family = AF_UNSPEC;
                        hints.ai_socktype = SOCK_STREAM;

                        addrinfo* proxyInfo = nullptr;
                        int status = getaddrinfo(proxyHost.c_str(), proxyPort.c_str(), &hints, &proxyInfo);
                        if (status != 0)
                        {
                            throw WebErrors::ProxyException( "Error resolving proxy address" );
                        }
                        _proxyInfoMap[key] = proxyInfo;
                    }
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        throw;
    }
}

std::vector<ServerSocket> WebServer::createServerSockets(const std::vector<Server> &server_confs)
{
    try
    {
        std::vector<ServerSocket> serverSockets;

        for (const auto& server_conf : server_confs) 
        {
            ServerSocket serverSocket(server_conf, O_NONBLOCK | FD_CLOEXEC);
            serverSockets.push_back(std::move(serverSocket));
        }
        return serverSockets;
    }
    catch (const std::exception& e)
    {
        throw;
    }
}

void WebServer::epollController(int clientSocket, int operation, uint32_t events, FdType fdType)
{
    try
    {
        struct epoll_event event;

        std::memset(&event, 0, sizeof(event));
        event.data.fd = clientSocket;
        event.events = events;

        if (EPOLL_CTL_ADD)
        {
            switch (fdType)
            {
                case FdType::SERVER:
                    std::cout << COLOR_GREEN_SERVER << " { Server socket added to epoll 🏊 }\n\n" << COLOR_RESET;
                    break;
                case FdType::CLIENT:
                    std::cout << COLOR_GREEN_SERVER << " { Client socket added to epoll 🏊 }\n\n" << COLOR_RESET;
                    break;
                case FdType::CGI_PIPE:
                    std::cout << COLOR_GREEN_SERVER << " { CGI pipe added to epoll 🏊 }\n\n" << COLOR_RESET;
                    break;
            }
        }
        if (epoll_ctl(_epollFd, operation, clientSocket, &event) == -1)
        {
            close(clientSocket);
            throw std::runtime_error("Error changing epoll state: " + std::string(strerror(errno)));
        }
        if (operation == EPOLL_CTL_DEL)
        {
            close(clientSocket);
            clientSocket = -1;
        }
    }
    catch (const std::exception &e)
    {
        throw;
    }
}

void WebServer::acceptAddClientToEpoll(int clientSocketFd)
{
    try
    {
        struct sockaddr_in  clientAddr;
        socklen_t           clientLen = sizeof(clientAddr);
        ScopedSocket        clientSocket(accept(clientSocketFd, (struct sockaddr *)&clientAddr, &clientLen), 0);

        if (clientSocket.getFd() < 0)
            throw std::runtime_error( "Error accepting client" );
        setFdNonBlocking(clientSocketFd);
        epollController(clientSocket.getFd(), EPOLL_CTL_ADD, EPOLLIN, FdType::CLIENT);
        clientSocket.release();
    }
    catch (const std::exception &e)
    {
        throw;
    }
}

void WebServer::handleIncomingData(int clientSocket)
{
    bool stopProcessing = false;

    auto cleanupClient = [this](int clientSocket)
    {
        epollController(clientSocket, EPOLL_CTL_DEL, 0, FdType::CLIENT);
        _partialRequests.erase(clientSocket);
        _requestMap.erase(clientSocket);
    };

    auto isRequestComplete = [this, clientSocket, &stopProcessing](const std::string &request) -> bool
    {
        auto checkMaxBodySize = [&, this](const size_t &content_length, const std::string &request, int clientSocket) -> bool
        {
            auto hostIt = request.find("Host: ");
            if (hostIt == std::string::npos) return false;

            std::string host = request.substr(hostIt + 6, request.find("\r\n", hostIt) - hostIt - 6);

            for (const auto &server : _parser.getServers())
            {
                for (const auto &server_name : server.server_name)
                {
                    const std::string server_name_ports = server_name + ":" + std::to_string(server.port);
                    if (server_name_ports == host && static_cast<long>(content_length) > server.client_max_body_size)
                    {
                        std::cout << COLOR_RED_ERROR << "  Request body size exceeds client_max_body_size limit\n\n" << COLOR_RESET;
                        ErrorHandler(&server).handleError(_partialRequests[clientSocket], 413);
                        const int ret = send (clientSocket, _partialRequests[clientSocket].c_str(), _partialRequests[clientSocket].length(), 0);
                        if (ret == -1)
                            std::cerr << COLOR_RED_ERROR << "Error sending 413 response to client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                        else if (ret == 0)
                            std::cerr << COLOR_RED_ERROR << "Error sending 413 response to client, Connection closed by the client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                        stopProcessing = true;
                        return true;
                    }
                }
            }
            return false;
        };

        size_t              headerEnd = request.find("\r\n\r\n");

        if (headerEnd == std::string::npos)
            return false;

        size_t              contentLength = 0;
        std::istringstream  stream(request.substr(0, headerEnd + 4));
        std::string         line;

        while (std::getline(stream, line) && line != "\r")
        {
            if (line.find("Content-Length:") != std::string::npos)
            {
                contentLength = std::stoul(line.substr(15));
                if (checkMaxBodySize(contentLength, request, clientSocket))
                    return true;
                break;
            }
        }

        const size_t totalLength = headerEnd + 4 + contentLength;
        return request.length() >= totalLength;
    };

    auto extractCompleteRequest = [](const std::string &buffer) -> std::string
    {
        size_t              headerEnd = buffer.find("\r\n\r\n");

        if (headerEnd == std::string::npos)
            return "";
        size_t              contentLength = 0;
        std::istringstream  stream(buffer.substr(0, headerEnd + 4));
        std::string         line;

        while (std::getline(stream, line) && line != "\r")
        {
            if (line.find("Content-Length:") != std::string::npos)
            {
                contentLength = std::stoul(line.substr(15));
                break;
            }
        }
        const size_t        totalLength = headerEnd + 4 + contentLength;
        return buffer.substr(0, totalLength);
    };

    auto processRequest = [this](int clientSocket, const std::string &requestStr)
    {
        Request request(requestStr, _parser.getServers(), _proxyInfoMap);
        _requestMap[clientSocket] = request;

        std::cout << COLOR_MAGENTA_SERVER << "  Request to: " << request.getServer()->server_name[0]
                  << ":" << request.getServer()->port << request.getRequestData().originalUri << " ✉️\n\n"
                  << COLOR_RESET;

        if (request.getLocation()->type == LocationType::CGI && request.getErrorCode() == 0)
        {
            CGIHandler cgiHandler(request, *this);
            epoll_ctl(_epollFd, EPOLL_CTL_DEL, clientSocket, nullptr); // Only delete from epoll, don't close()
        }
        else
        {
            epollController(clientSocket, EPOLL_CTL_MOD, EPOLLOUT, FdType::CLIENT);
        }
        _partialRequests.erase(clientSocket);
    };

    try
    {
        char    buffer[1024];
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);

        if (bytesRead > 0)
        {
            _partialRequests[clientSocket].append(buffer, bytesRead);

            while (isRequestComplete(_partialRequests[clientSocket]))
            {
                if (stopProcessing)
                {
                    cleanupClient(clientSocket);
                    break;
                }
                std::string completeRequest = extractCompleteRequest(_partialRequests[clientSocket]);
                _partialRequests[clientSocket].erase(0, completeRequest.length());
                processRequest(clientSocket, completeRequest);
            }
        }
        else if (bytesRead == 0)
        {
            cleanupClient(clientSocket);
        }
        else if (bytesRead == -1)
        {
            cleanupClient(clientSocket);
            throw std::runtime_error("Error receiving data from client");
        }
    }
    catch (const std::exception &e)
    {
        try {
            ErrorHandler(&_parser.getServers().front()).handleError(_partialRequests[clientSocket], 400);
            const int ret = send(clientSocket, _partialRequests[clientSocket].c_str(), _partialRequests[clientSocket].length(), 0);
            if (ret == -1)
                std::cerr << COLOR_RED_ERROR << "Error sending 400 response to client: " << strerror(errno) << "\n\n" << COLOR_RESET;
            else if (ret == 0)
                std::cerr << COLOR_RED_ERROR << "Error sending 400 response to client, Connection closed by the client: " << strerror(errno) << "\n\n" << COLOR_RESET;
            cleanupClient(clientSocket);
        } catch (const std::exception &inner_e) {
            WebErrors::combineExceptions(e, inner_e);
            throw e;
        }
        throw ;
    }
}

void WebServer::handleOutgoingData(int clientSocket)
{
    try
    {
        auto          it = _requestMap.find(clientSocket);
        if (it != _requestMap.end())
        {
            const Request &request = it->second;
            Response res(request);

            const int bytesSent = send(clientSocket, res.getResponse().c_str(), res.getResponse().length(), 0);

            if (bytesSent == -1)
            {
                epollController(clientSocket, EPOLL_CTL_DEL, 0, FdType::CLIENT);
                throw std::runtime_error("Error sending response to client");
            }
            else if (bytesSent == 0)
            {
                epollController(clientSocket, EPOLL_CTL_DEL, 0, FdType::CLIENT);
                throw std::runtime_error("Connection closed by the client");
            }
            else
                epollController(clientSocket, EPOLL_CTL_DEL, 0, FdType::CLIENT);
        }
        _requestMap.erase(it);
    }
    catch (const std::exception &e)
    {
        try {
            epollController(clientSocket, EPOLL_CTL_DEL, 0, FdType::CLIENT);
        } catch (const std::exception &inner_e) {
            WebErrors::combineExceptions(e, inner_e);
        }
        throw;
    }
}

void WebServer::handleCGIinteraction(int pipeFd)
{
    try
    {
        for (auto it = _cgiInfoList.begin(); it != _cgiInfoList.end(); ++it)
        {
            if (it->readFromCgiFd == pipeFd)
            {
                char    buffer[4096];
                ssize_t bytes = read(pipeFd, buffer, sizeof(buffer));

                if (bytes > 0)
                    it->response.append(buffer, bytes);
                else if (bytes == 0)
                {
                    const int clientSocket = it->clientSocket;
                    epollController(pipeFd, EPOLL_CTL_DEL, 0, FdType::CGI_PIPE);
                    const int   ret = send(clientSocket, it->response.c_str(), it->response.length(), 0);
                    if (ret == -1)
                        std::cerr << COLOR_RED_ERROR << "Error sending 504 response to client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                    else if (ret == 0)
                        std::cerr << COLOR_RED_ERROR << "Error sending Cgi response to client, Connection closed by the client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                    close(clientSocket);
                    _cgiInfoList.erase(it);
                    _requestMap.erase(clientSocket);
                }
                else if (bytes == -1)
                    throw std::runtime_error("Error reading from CGI output pipe");
                break;
            }
        }
    }
    catch (std::exception &e)
    {
        throw;
    }
}

void WebServer::CGITimeoutChecker(void)
{
    try 
    {
        auto now = std::chrono::steady_clock::now();

        for (auto it = _cgiInfoList.begin(); it != _cgiInfoList.end();)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->startTime).count();

            if (elapsed > CGI_TIMEOUT_LIMIT)
            {
                std::cout << COLOR_YELLOW_CGI << "  CGI Script Timed Out ⏰\n\n" << COLOR_RESET;
                if (kill(it->pid, SIGKILL) == -1)
                    std::cerr << COLOR_RED_ERROR << "Failed to kill CGI process: " << strerror(errno) << "\n\n" << COLOR_RESET;
                if (it->clientSocket >= 0 && fcntl(it->clientSocket, F_GETFD) != -1)
                {  
                    std::string response;
                    ErrorHandler(_requestMap[it->clientSocket].getServer()).handleError(response, 504);
                    const int   ret = send(it->clientSocket, response.c_str(), response.length(), 0);
                    if (ret == -1)
                        std::cerr << COLOR_RED_ERROR << "Error sending 504 response to client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                    else if (ret == 0)
                        std::cerr << COLOR_RED_ERROR << "Error sending 504 response to client, Connection closed by the client: " << strerror(errno) << "\n\n" << COLOR_RESET;
                    close(it->clientSocket);
                }
                if (_requestMap[it->clientSocket].getRequestData().method == "POST" && it->writeToCgiFd != -1)
                    epollController(it->writeToCgiFd, EPOLL_CTL_DEL, 0, FdType::CGI_PIPE);
                epollController(it->readFromCgiFd, EPOLL_CTL_DEL, 0, FdType::CGI_PIPE);
                _requestMap.erase(it->clientSocket);
                it = _cgiInfoList.erase(it);
            }
            else
                ++it;
        }
    }
    catch (const std::exception &e)
    {
        throw;
    }
}

void WebServer::handleEvents(int eventCount)
{
    try
    {
        auto getCorrectServerSocket = [this](int fd) -> bool {
            return std::any_of(_serverSockets.begin(), _serverSockets.end(),
                               [fd](const ScopedSocket& socket) { return socket.getFd() == fd; });
        };
        auto isCgiFd = [this](int fd) -> bool {
            for (const auto& cgiInfo : _cgiInfoList)
            {
                if (cgiInfo.readFromCgiFd == fd || cgiInfo.writeToCgiFd == fd)
                    return true;
            }
            return false;
        };

        for (int i = 0; i < eventCount; ++i)
        {
           _currentEventFd = _events[i].data.fd;

            if (getCorrectServerSocket(_currentEventFd))
            {
                acceptAddClientToEpoll(_currentEventFd);
            }
            else if (isCgiFd(_currentEventFd))
            {
                handleCGIinteraction(_currentEventFd);
            }
            else
            {
                if (_events[i].events & EPOLLIN)
                {
                    handleIncomingData(_currentEventFd);
                }
                else if (_events[i].events & EPOLLOUT)
                {
                    handleOutgoingData(_currentEventFd);
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        throw;
    }
}

void WebServer::start()
{
    try
    {
        std::signal(SIGINT, signalHandler);  
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGQUIT, signalHandler);
        std::signal(SIGTSTP, signalHandler); 
        std::signal(SIGPIPE, SIG_IGN);
    }
    catch (const std::exception &e) {
        std::cerr << "Error setting signal handlers: " << e.what() << "\n";
        throw;
    }

    while (s_serverRunning)
    {
        try
        {
            int eventCount = epoll_wait(_epollFd, _events.data(), MAX_EVENTS, 500);
            if (eventCount == -1)
            {
                if (errno == EINTR) continue;
                throw std::runtime_error("Epoll wait error");
            }
            if (eventCount > 0)
                handleEvents(eventCount);
            CGITimeoutChecker();
        }
        catch (const std::exception &e)
        {
            WebErrors::printerror("WebServer::start", e.what());
        }
    }
    std::cout << COLOR_GREEN_SERVER << "[ SERVER STOPPED ] 🔌\n" << COLOR_RESET;
}

void  WebServer::signalHandler(int signal) { (void) signal; s_serverRunning = 0; }

int WebServer::getEpollFd() const { return _epollFd; }

cgiInfoList& WebServer::getCgiInfoList() { return _cgiInfoList; }

int WebServer::getCurrentEventFd() const { return _currentEventFd; }

void WebServer::setFdNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        throw std::runtime_error("Failed to get pipe flags");
    flags |= O_NONBLOCK | FD_CLOEXEC;
    if (fcntl(fd, F_SETFL, flags) == -1)
        throw std::runtime_error("Failed to set non-blocking mode");
}
