#include "FileTransfer.h"
#include <fstream>
#include <iostream>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;

bool sendFile(SOCKET clientSocket, const std::string& filePath) {
    if (!fs::exists(filePath)) {
        std::cerr << "File does not exist!" << std::endl;
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file!" << std::endl;
        return false;
    }

    // Send file metadata
    std::string fileName = fs::path(filePath).filename().string();
    uint64_t fileSize = fs::file_size(filePath);

    std::string metadata = fileName + ":" + std::to_string(fileSize);
    send(clientSocket, metadata.c_str(), static_cast<int>(metadata.size()), 0);

    char buffer[4096];
    while (file) {
        file.read(buffer, sizeof(buffer));
        int bytesRead = static_cast<int>(file.gcount());

        if (send(clientSocket, buffer, bytesRead, 0) == SOCKET_ERROR) {
            std::cerr << "Error sending file data!" << std::endl;
            file.close();
            return false;
        }
    }

    file.close();
    std::cout << "File sent successfully!" << std::endl;
    return true;
}

bool receiveFile(SOCKET serverSocket) {
    char buffer[4096];

    // Step 1: Receive metadata
    int bytesReceived = recv(serverSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cerr << "Error receiving metadata or connection closed by server!" << endl;
        return false;
    }

    buffer[bytesReceived] = '\0'; // Null-terminate the metadata
    string metadata(buffer);

    // Step 2: Parse metadata to get file name and size
    size_t delimPos = metadata.find(':');
    if (delimPos == string::npos) {
        cerr << "Invalid metadata received!" << endl;
        return false;
    }

    string fileName = metadata.substr(0, delimPos);
    uint64_t fileSize = stoull(metadata.substr(delimPos + 1));

    cout << "Receiving file: " << fileName << " (" << fileSize << " bytes)" << endl;

    // Step 3: Open file for writing
    ofstream outFile(fileName, ios::binary);
    if (!outFile) {
        cerr << "Error: Unable to create file for saving!" << endl;
        return false;
    }

    // Step 4: Receive file data
    uint64_t bytesReceivedTotal = 0;
    while (bytesReceivedTotal < fileSize) {
        ZeroMemory(buffer, sizeof(buffer));
        int chunkReceived = recv(serverSocket, buffer, sizeof(buffer), 0);

        if (chunkReceived <= 0) {
            std::cerr << "Error receiving file data!" << endl;
            break;
        }

        outFile.write(buffer, chunkReceived);
        bytesReceivedTotal += chunkReceived;
    }

    outFile.close();

    if (bytesReceivedTotal == fileSize) {
        cout << "File saved successfully as: " << fileName << endl;
    }
    else {
        std::cerr << "File transfer incomplete. Expected " << fileSize << " bytes but received " << bytesReceivedTotal << " bytes." << endl;
    }
}

