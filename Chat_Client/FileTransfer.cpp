#include "FileTransfer.h"
#include <fstream>
#include <iostream>
#include <filesystem>

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

bool receiveFile(SOCKET clientSocket) {
    char metadataBuffer[4096];
    int bytesReceived = recv(clientSocket, metadataBuffer, sizeof(metadataBuffer), 0);
    if (bytesReceived <= 0) {
        std::cerr << "Failed to receive file metadata!" << std::endl;
        return false;
    }

    std::string metadata(metadataBuffer, bytesReceived);
    size_t colonPos = metadata.find(':');
    if (colonPos == std::string::npos) {
        std::cerr << "Invalid file metadata format!" << std::endl;
        return false;
    }

    std::string fileName = metadata.substr(0, colonPos);
    uint64_t fileSize = std::stoull(metadata.substr(colonPos + 1));

    std::ofstream file(fileName, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to create file!" << std::endl;
        return false;
    }

    char buffer[4096];
    uint64_t totalBytesReceived = 0;
    while (totalBytesReceived < fileSize) {
        bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesReceived <= 0) {
            std::cerr << "Error receiving file data!" << std::endl;
            file.close();
            return false;
        }

        file.write(buffer, bytesReceived);
        totalBytesReceived += bytesReceived;
    }

    file.close();
    std::cout << "File received successfully: " << fileName << std::endl;
    return true;
}
