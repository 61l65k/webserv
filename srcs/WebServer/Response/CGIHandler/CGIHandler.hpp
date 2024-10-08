/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   CGIHandler.hpp                                     :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: alex <alex@student.42.fr>                  +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/08/09 10:52:47 by uahmed            #+#    #+#             */
/*   Updated: 2024/09/27 13:47:19 by alex             ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#pragma once

#include <string>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include "Request.hpp"
#include "WebServer.hpp"

#define PIPES 2
#define WRITEND 1
#define READEND 0
#define CODE404 "404"
#define CODE500 "500"
#define PYTHON3 "/bin/python3"
#define ERROR "\033[31ERROR: \033[0"
#define CGI_TIMEOUT_LIMIT 5

class   CGIHandler
{
    public:
        CGIHandler(const Request& request, WebServer &webServer);
        ~CGIHandler() = default;

        std::string      getCGIResponse( void ) const;
    private:
        WebServer       &_webServer;
        const Request&   _request;
        std::string      _response;
        std::string      _scriptPath;
        int              _fromCgi_pipe[PIPES];
        int              _toCgi_pipe[PIPES];

        bool            validateExecutable( void );
        bool            parentWaitForChild(pid_t pid);
        void            executeScript( void );
        void            child( void );
        void            parent( pid_t pid );
        void            childSetEnvp( char const *envp[] );
};
