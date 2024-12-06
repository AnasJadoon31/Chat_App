#pragma once
#include <WinSock2.h>
#include <string>

bool sendFile(SOCKET clientSocket, const std::string& filePath);
bool receiveFile(SOCKET clientSocket);