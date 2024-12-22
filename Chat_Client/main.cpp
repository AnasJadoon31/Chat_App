#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <thread>
#include <set>
#include "FileTransfer.h"
#include <fstream>

using namespace std;

#pragma comment (lib, "ws2_32.lib")

set <string> blockedUsers;

bool initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
void handleFileTransfer(SOCKET senderSocket) {
    char buffer[4096];

    // Step 1: Receive metadata
    int bytesReceived = recv(senderSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cerr << "Error receiving metadata or connection closed!" << endl;
        return;
    }

    buffer[bytesReceived] = '\0'; // Null-terminate the received metadata
    string metadata(buffer);

    // Step 2: Parse metadata to get file name and size
    size_t delimPos = metadata.find(':');
    if (delimPos == string::npos) {
        cerr << "Invalid metadata received!" << endl;
        return;
    }

    string fileName = metadata.substr(0, delimPos);
    uint64_t fileSize = stoull(metadata.substr(delimPos + 1));

    cout << "Receiving file: " << fileName << " (" << fileSize << " bytes)" << endl;

    // Step 3: Open a file to save the data on the server
    ofstream outFile(fileName, ios::binary);
    if (!outFile) {
        cerr << "Error: Unable to create file!" << endl;
        send(senderSocket, "ERROR: Unable to create file.", 29, 0);
        return;
    }

    // Step 4: Receive the file data
    uint64_t bytesReceivedTotal = 0;
    while (bytesReceivedTotal < fileSize) {
        ZeroMemory(buffer, sizeof(buffer));
        int chunkReceived = recv(senderSocket, buffer, sizeof(buffer), 0);

        if (chunkReceived <= 0) {
            cerr << "Error receiving file data!" << endl;
            break;
        }

        outFile.write(buffer, chunkReceived);
        bytesReceivedTotal += chunkReceived;
    }

    outFile.close();

    if (bytesReceivedTotal == fileSize) {
        cout << "File transfer complete: " << fileName << endl;
        send(senderSocket, "File transfer complete.", 23, 0);
        }
    }
void sendMessage(SOCKET s) {
    string user, message;
    cout << "Enter unique username: ";
    getline(cin, user);
    send(s, user.c_str(), static_cast<int>(user.length()), 0);
    while (true) {
        getline(cin, message);

        if (message == "/leave") {
            cout << "Leaving..." << endl;
            closesocket(s);
            break;
        }
        else if (message == "/clear") {
            system("cls");
            cout << "Please use these commands for following actions:\n1./leave: Leave the server\n2./private <username> <message>: Start private conversation\n3./list: List all users\n4./block <username>: Block a user\n5./unblock <username>: Unblock a user\n6./clear: Clear old messages\n7./create_group <group Name>: Creates a private group\n8./join_group <group Name>: Joins already created group\n9./group <group Name> <Message>: Send a message to group\n10./leave_group <group Name>: Leaves a joined group\n11./sendfile <file Name>: Sends a file to everyone\n\n";
        }
        else if (message.rfind("/sendfile ", 0) == 0) { // Command starts with "/sendfile "
            send(s, message.c_str(), static_cast<int>(message.length()), 0);
            std::string filePath = message.substr(10); // Extract file path
            if (!sendFile(s, filePath)) {
                std::cerr << "File transfer failed!" << std::endl;
            }
            continue;
        }
        else {
            string msg = user + message;
            int bytesSent = send(s, msg.c_str(), static_cast<int>(msg.length()), 0);
            if (bytesSent == SOCKET_ERROR) {
                cout << "Message sending failed: " << WSAGetLastError() << endl;
                break;
            }
        }
    }
}

void receiveMessage(SOCKET s) {
    char buffer[4096];
    int recvLength;
    while (true) {
        recvLength = recv(s, buffer, sizeof(buffer), 0);
        if (recvLength <= 0) {
            cout << "Disconnected from server: " << WSAGetLastError() << endl;
            break;
        }
        //buffer[recvLength] = '\0'; // Null-terminate the received data
        string message(buffer, recvLength);

        // Check for metadata (e.g., "fileName:fileSize")
        size_t delimPos = message.find(':');
        if (delimPos != string::npos && isdigit(message[delimPos + 1])) {
            // Metadata received
            string fileName = message.substr(0, delimPos);
            uint64_t fileSize = stoull(message.substr(delimPos + 1));

            cout << "Receiving file: " << fileName << " (" << fileSize << " bytes)" << endl;

            // Open a file for writing
            ofstream outFile(fileName, ios::binary);
            if (!outFile) {
                cerr << "Error: Unable to create file: " << fileName << endl;
                send(s, "ERROR: Unable to create file.", 29, 0);
                continue;
            }

            // Receive the file data
            uint64_t bytesReceivedTotal = 0;
            while (bytesReceivedTotal < fileSize) {
                recvLength = recv(s, buffer, sizeof(buffer), 0);
                if (recvLength <= 0) {
                    cerr << "Error receiving file data!" << endl;
                    break;
                }

                outFile.write(buffer, recvLength);
                bytesReceivedTotal += recvLength;

                // Progress update (optional)
                cout << "Progress: " << bytesReceivedTotal << "/" << fileSize << " bytes\r";
            }

            outFile.close();

            if (bytesReceivedTotal == fileSize) {
                cout << "\nFile transfer complete: " << fileName << endl;
                send(s, "File transfer complete.", 23, 0);
            }
            else {
                cerr << "File transfer incomplete: Received " << bytesReceivedTotal << " of " << fileSize << " bytes." << endl;
            }
            continue; // Skip further processing for this message
        }

        // Extract the username from the message (assuming format "username: message")
        size_t colonPos = message.find(':');
        if (colonPos != string::npos) {
            string sender = message.substr(0, colonPos);

            // Check if the sender is blocked
            if (blockedUsers.find(sender) != blockedUsers.end()) {
                // If the sender is blocked, skip displaying the message
                continue;
            }
        }
        
        if (message == "CLEAR_SCREEN") {
            system("cls"); // Clear the console screen
        }
        else {
            cout << message << endl;
        }
    }
}

int main() {
    if (!initialize()) {
        cout << "WinSock initialization failed!" << endl;
        return 1;
    }

    SOCKET s;
    string serverAddress;
    int port;
    bool isConnected = false;

    while (!isConnected) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            cout << "Invalid socket created: " << WSAGetLastError() << endl;
            WSACleanup();
            return 1;
        }

        cout << "Enter IP address: ";
        cin >> serverAddress;
        cin.clear();
        cout << "Enter port: ";
        cin >> port;
        cin.clear();

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        if (inet_pton(AF_INET, serverAddress.c_str(), &serverAddr.sin_addr) <= 0) {
            cout << "Invalid Address or Address not supported. Please try again." << endl;
            closesocket(s);
            continue; // Retry
        }

        if (connect(s, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            cout << "Connection failed: " << WSAGetLastError() << ". Please try again." << endl;
            closesocket(s);
            continue; // Retry
        }

        isConnected = true; // Connection successful
    }

    system("cls");
    cout << "Connection successful!" << endl;

    thread sendThread(sendMessage, s);
    thread receiveThread(receiveMessage, s);

    sendThread.join();
    receiveThread.join();

    closesocket(s);
    WSACleanup();
    return 0;
}