#include "Request.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>

Request::Request()
    : _rawRequest(""), _server(nullptr), _location(nullptr), _proxyInfo(nullptr)
{
}

Request::Request(const std::string& rawRequest, const std::vector<Server>& servers, const std::unordered_map<std::string, addrinfo*>& proxyInfoMap)
    : _rawRequest(rawRequest), _server(nullptr), _location(nullptr), _proxyInfo(nullptr)
{
    try
    {
        parseRequest();
        RequestValidator(*this, servers, proxyInfoMap).validate();
        if (!_server || !_location)
            throw std::runtime_error("Error validating request: No matching Server or location found for request");
    }
    catch (const std::exception& e)
    {
        throw ;
    }
}

void Request::parseRequest(void)
{
    try
    {
        std::istringstream stream(_rawRequest);
        std::string requestLine;
        std::getline(stream, requestLine);

        parseRequestLine(requestLine);
        parseHeadersAndBody(stream);
        setContentTypeAndLength();
    }
    catch (const std::exception& e)
    {
        throw ;
    }
}


void Request::parseRequestLine(const std::string& requestLine)
{
    try
    {
        std::string::size_type methodEnd = requestLine.find(' ');
        if (methodEnd == std::string::npos)
            throw std::runtime_error("Invalid request line: missing method");

        std::string::size_type uriEnd = requestLine.find(' ', methodEnd + 1);
        if (uriEnd == std::string::npos)
            throw std::runtime_error("Invalid request line: missing URI");

        _requestData.method = requestLine.substr(0, methodEnd);
        _requestData.uri = requestLine.substr(methodEnd + 1, uriEnd - methodEnd - 1);
        _requestData.httpVersion = WebParser::trimSpaces(requestLine.substr(uriEnd + 1));

        size_t queryPos = _requestData.uri.find('?');
        if (queryPos != std::string::npos)
        {
            _requestData.query_string = _requestData.uri.substr(queryPos + 1);
            _requestData.uri = _requestData.uri.substr(0, queryPos);
        }
        std::cout << _requestData.query_string << std::endl;
        std::cout << _requestData.query_string << std::endl;
        std::cout << _requestData.query_string << std::endl;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("Error parsing request line: ") + e.what());
    }
}


void Request::parseHeadersAndBody(std::istringstream& stream)
{
    try
    {
        std::string line;
        bool isHeaderSection = true;

        while (std::getline(stream, line))
        {
            if (isHeaderSection)
            {
                if (line == "\r" || line.empty())
                {
                    isHeaderSection = false;
                    continue;
                }

                parseHeaderLine(line);
            }
            else
            {
                _requestData.body += line + "\n";
            }
        }
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("Error parsing headers and body: ") + e.what());
    }
}

void Request::parseHeaderLine(const std::string& line)
{
    try
    {
        size_t pos = line.find(':');
        if (pos == std::string::npos)
            throw std::runtime_error("Invalid header line: missing ':'");
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        _requestData.headers[key] = value;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("Error parsing header line: ") + e.what());
    }
}

void Request::setContentTypeAndLength()
{
    try
    {
        _requestData.content_type = _requestData.headers["Content-Type"];
        if (_requestData.headers.find("Content-Length") != _requestData.headers.end())
        {
            _requestData.content_length = std::stoi(_requestData.headers["Content-Length"]);
        }
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("Error setting content type and length: ") + e.what());
    }
}

const std::string&  Request::getRawRequest() const { return _rawRequest; }

const RequestData&  Request::getRequestData() const { return _requestData; }

const Server*       Request::getServer() const { return _server; }

const Location*     Request::getLocation() const { return _location; }

addrinfo*           Request::getProxyInfo() const { return _proxyInfo; }

int                 Request::getErrorCode() const { return _errorCode; }
