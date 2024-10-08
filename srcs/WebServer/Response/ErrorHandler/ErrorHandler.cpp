#include "ErrorHandler.hpp"

ErrorHandler::ErrorHandler(const Server* server)
    : _server(server)
{
}

void ErrorHandler::handleError(std::string& response, int errorCode) const
{
    std::string errorMessage = getErrorMessage(errorCode);
    std::string errorPagePath = WebParser::getErrorPage(errorCode, _server);
    std::string errorPage;
    if (errorPagePath.length() == 0)
        errorPage = generateDefaultErrorPage(errorCode);
    else
    {
        try
        {
            readFileContent(errorPagePath, errorPage);
        }
        catch(const std::exception& e)
        {
            errorCode = 500;
            errorMessage = getErrorMessage(errorCode);
            errorPagePath =  WebParser::getErrorPage(errorCode, _server);
            if (errorPagePath.length() == 0)
                errorPage = generateDefaultErrorPage(errorCode);
            else
            {
                try
                {
                    readFileContent(errorPagePath, errorPage);
                }
                catch(const std::exception& e)
                {
                    errorPage = generateDefaultErrorPage(errorCode);
                }
            }
        }
    }

    response = "HTTP/1.1 " + std::to_string(errorCode) + " " + errorMessage + "\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Content-Length: " + std::to_string(errorPage.length()) + "\r\n";
    response += "\r\n";
    response += errorPage;
}

std::string ErrorHandler::getErrorMessage(int errorCode) const
{
    switch (errorCode)
    {
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    default: return "Internal Server Error";
    }
}

void ErrorHandler::readFileContent(const std::string& path, std::string& content) const
{
    std::ifstream fileStream(path, std::ios::in | std::ios::binary);
    if (!fileStream)
    {
        throw std::runtime_error("Failed to read file: " + path);
    }

    std::ostringstream ss;
    ss << fileStream.rdbuf();
    content = ss.str();

}

std::string ErrorHandler::generateDefaultErrorPage(int errorCode)
{
    return ("<!doctype html>\n<head>\n\t<meta charset=\"UTF-8\" />\n</head>\n<html>\n\t<body>\n\t\t<h1>ERROR - " + std::to_string(errorCode) + "</h1>\n\t\t<p>(This page was generated by the server)</p>\n\t</body>\n</html>");
}