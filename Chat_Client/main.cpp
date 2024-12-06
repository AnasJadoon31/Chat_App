#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <thread>
#include <set>
#include "FileTransfer.h"

using namespace std;

#pragma comment (lib, "ws2_32.lib")

set <string> blockedUsers;

bool initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
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
            cout << "Please use these commands for following actions:\n1./leave: Leave the server\n2./private <username> <message>: Start private conversation\n3./list: List all users\n4./block: Block a user\n5./unblock: Unblock a user\n6./clear: Clear old messages\n7./create_group: Creates a private group\n8./join_group: Joins already created group\n9./group <group Name> <Message>: Send a message to group\n10./leave_group: Leaves a joined group\n\n";
        }
        else if (message.rfind("/sendfile ", 0) == 0) { // Command starts with "/sendfile "
            send(s, message.c_str(), static_cast<int>(message.length()), 0);
            std::string filePath = message.substr(10); // Extract file path
            if (!sendFile(s, filePath)) {
                std::cerr << "File transfer failed!" << std::endl;
            }
            continue;
        }
        else if (message.rfind("FILE_METADATA", 0) == 0) { // Custom tag for file metadata
            if (!receiveFile(s)) {
                std::cerr << "File reception failed!" << std::endl;
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

        string message(buffer, recvLength);

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