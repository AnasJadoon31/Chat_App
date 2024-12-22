#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <tchar.h>
#include <thread>
#include <vector>
#include <set>
#include <mutex>
#include <algorithm>
#include <string>
#include <map>
#include <limits>
#include <fstream>
#include <sstream>
#include <filesystem>

using namespace std;
namespace fs = std::filesystem;
#pragma comment(lib, "ws2_32.lib")

vector<string> userNames;
mutex userNamesMutex;
mutex clientsMutex;
map<string, SOCKET> userMap; // Map of username to socket
mutex userMapMutex;          // Mutex for thread safety
map<string, set<string>> blockedUsers; // Map of usernames to the set of users they have blocked
mutex blockedUsersMutex;
map<string, string> userCredentials; // Map to store username and password
mutex credentialsMutex; // Mutex for thread safety when accessing user credentials
vector<SOCKET> clientsSnapshot;
map<string, set<SOCKET>> groupMap;
mutex groupMapMutex; // Protects access to groupMap


string getServerIPAddress() {
    char hostName[256];
    struct addrinfo hints, * res;
    char ipBuffer[INET_ADDRSTRLEN];

    // Get local host name
    if (gethostname(hostName, sizeof(hostName)) != 0) {
        cerr << "Error retrieving host name." << endl;
        return "Unknown";
    }

    // Set up the hints structure
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;  // IPv4
    hints.ai_socktype = SOCK_STREAM;

    // Get the address info for the host
    int result = getaddrinfo(hostName, NULL, &hints, &res);
    if (result != 0) {
        cerr << "Error retrieving address info: " << gai_strerror(result) << endl;
        return "Unknown";
    }

    // Convert the first address to a string
    struct sockaddr_in* sockaddr_in = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &(sockaddr_in->sin_addr), ipBuffer, sizeof(ipBuffer));

    freeaddrinfo(res); // Free the memory allocated by getaddrinfo

    return string(ipBuffer); // Return the IP address
}

// WinSock initialization
bool initialize() {
    WSADATA data;
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        cout << "WSAStartup failed: " << result << endl;
        return false;
    }
    return true;
}

// Function to list all users
void listUsers() {
    lock_guard<mutex> lock(userMapMutex);
    if (userMap.empty()) {
        cout << "No users connected." << endl;
    }
    else {
        cout << "Connected Users:" << endl;
        for (const auto& pair : userMap) {
            cout << "- " << pair.first << endl;
        }
    }
}

// Function to block a user
void blockUser(const string& usernameToBlock) {
    lock_guard<mutex> lock(userMapMutex);
    auto it = userMap.find(usernameToBlock);
    if (it != userMap.end()) {
        // Close the user's socket
        cout << "Kicking user: " << usernameToBlock << endl;
        closesocket(it->second);
        userMap.erase(it); // Remove from map
    }
    else {
        cout << "User not found: " << usernameToBlock << endl;
    }
}

void announceToAll(const string& message) {
    lock_guard<mutex> lock(clientsMutex);
    for (SOCKET client : clientsSnapshot) {
        send(client, message.c_str(), static_cast<int>(message.size()), 0);
    }
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

        // Step 5: Broadcast the file to other clients
        for (SOCKET client : clientsSnapshot) {
            if (client != senderSocket) { // Skip the sender
                send(client, metadata.c_str(), static_cast<int>(metadata.size()), 0);

                // Re-open the file to read its content
                ifstream inFile(fileName, ios::binary);
                if (!inFile) {
                    cerr << "Error opening file for broadcast!" << endl;
                    continue;
                }

                while (inFile) {
                    inFile.read(buffer, sizeof(buffer));
                    int bytesToSend = static_cast<int>(inFile.gcount());
                    if (send(client, buffer, bytesToSend, 0) == SOCKET_ERROR) {
                        cerr << "Error sending file data to client!" << endl;
                        break;
                    }
                }
                inFile.close();
            }
        }
    }
    else {
        cerr << "File transfer incomplete. Expected " << fileSize << " bytes but received " << bytesReceivedTotal << " bytes." << endl;
        send(senderSocket, "ERROR: File transfer incomplete.", 31, 0);
    }
}

// Interact with client
void interactWithClient(SOCKET clientSocket, vector<SOCKET>& clients) {
    char buffer[4096];
    string userName;
    bool isFile = false;
    // Loop until a unique username is provided
    while (true) {
        int user = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (user <= 0) {
            cout << "Client disconnected before choosing a username." << endl;
            closesocket(clientSocket);
            return;
        }

        userName = string(buffer, user);

        {
            lock_guard<mutex> lock(userNamesMutex);
            if (find(userNames.begin(), userNames.end(), userName) != userNames.end()) {
                // Send error message to client
                string clearScreenSignal = "CLEAR_SCREEN";
                send(clientSocket, clearScreenSignal.c_str(), static_cast<int>(clearScreenSignal.length()), 0);
                string userNameError = "\nUsername already taken! Please enter another username!\n\n";
                send(clientSocket, userNameError.c_str(), static_cast<int>(userNameError.length()), 0);
            }
            else {
                // Add username to the list and send success message
                userNames.push_back(userName);
                lock_guard<mutex> lock(userMapMutex);
                userMap[userName] = clientSocket; // Add username and socket to map
                string clearScreenSignal = "CLEAR_SCREEN";
                send(clientSocket, clearScreenSignal.c_str(), static_cast<int>(clearScreenSignal.length()), 0);
                string successMessage = "Username accepted!\n\nPlease use these commands for following actions:\n1./leave: Leave the server\n2./private <username> <message>: Start private conversation\n3./list: List all users\n4./block <username>: Block a user\n5./unblock <username>: Unblock a user\n6./clear: Clear old messages\n7./create_group <group Name>: Creates a private group\n8./join_group <group Name>: Joins already created group\n9./group <group Name> <Message>: Send a message to group\n10./leave_group <group Name>: Leaves a joined group\n11./sendfile <file Name>: Sends a file to everyone\n\n";
                send(clientSocket, successMessage.c_str(), static_cast<int>(successMessage.length()), 0);
                break; // Exit the loop once a unique username is chosen
            }
        }
    }

    cout << "\nClient connected with username: " << userName << endl;
    string msg = userName + " joined the server!";
    announceToAll(msg);

    while (true) {
        int bytesRecvd = recv(clientSocket, buffer, sizeof(buffer), 0);

        if (bytesRecvd <= 0) {
            cout << userName << " disconnected!" << endl;
            string msg = userName + " left the server!";
            announceToAll(msg);
            userNames.erase(remove(userNames.begin(), userNames.end(), userName), userNames.end());
            // Remove socket from userMap
            {
                lock_guard<mutex> lock(userMapMutex);
                for (auto it = userMap.begin(); it != userMap.end(); ++it) {
                    if (it->second == clientSocket) { // Find matching socket
                        userMap.erase(it);            // Remove entry
                        break;
                    }
                }
            }

            // Remove socket from clients vector
            {
                lock_guard<mutex> lock(clientsMutex);
                clients.erase(remove(clients.begin(), clients.end(), clientSocket), clients.end());
            }

            // Close socket
            closesocket(clientSocket);
            return; // Exit the function after cleanup
        }

        string message(buffer, bytesRecvd);
        string msg = userName + ": " + message;
        cout << msg << endl;

        if (message.find("/block ") == 0 && message.length() > 7) {
            string userToBlock = message.substr(7); // Extract username to block
            lock_guard<mutex> lock(blockedUsersMutex);
            blockedUsers[userName].insert(userToBlock);
            cout << userName << " has blocked " << userToBlock << endl;

            // Send confirmation to the sender
            string confirmation = "You have blocked " + userToBlock + "\n";
            send(clientSocket, confirmation.c_str(), static_cast<int>(confirmation.length()), 0);

            // Notify the user being blocked
            {
                lock_guard<mutex> lock(userMapMutex);
                auto it = userMap.find(userToBlock);
                if (it != userMap.end()) {
                    SOCKET blockedUserSocket = it->second; // Get the socket of the blocked user
                    string notify = "You are blocked by " + userName + "\n";
                    send(blockedUserSocket, notify.c_str(), static_cast<int>(notify.length()), 0);
                }
                else {
                    // User not found in the map
                    cout << "User to block (" << userToBlock << ") not found in the users' list." << endl;
                }
            }

            continue; // Skip further processing
        }
        else if (message.find("/unblock ") == 0 && message.length() > 9) {
            string userToUnblock = message.substr(9); // Extract username to unblock

            bool wasBlocked = false;

            {
                lock_guard<mutex> lock(blockedUsersMutex);

                // Check if the current user has any blocked users
                auto userBlocks = blockedUsers.find(userName);
                if (userBlocks != blockedUsers.end()) {
                    // Try to unblock the user
                    wasBlocked = userBlocks->second.erase(userToUnblock) > 0;

                    // Clean up empty blocked lists to save memory
                    if (userBlocks->second.empty()) {
                        blockedUsers.erase(userName);
                    }
                }
            }

            if (wasBlocked) {
                cout << userName << " has unblocked " << userToUnblock << endl;
                string confirmation = "You have unblocked " + userToUnblock + "\n";
                send(clientSocket, confirmation.c_str(), static_cast<int>(confirmation.length()), 0);
            }
            else {
                string notFound = "User " + userToUnblock + " is not in your block list.\n";
                send(clientSocket, notFound.c_str(), static_cast<int>(notFound.length()), 0);
            }
        }
        else if (message == "/list") {
            // Send list of connected users to the client
            string userList = "Connected Users:\n";
            lock_guard<mutex> lock(userMapMutex);
            for (const auto& pair : userMap) {
                userList += "-" + pair.first + "\n";
            }
            send(clientSocket, userList.c_str(), static_cast<int>(userList.length()), 0);
        }
        else if (message.find("/private ") == 0) {
            // Extract recipient username and message
            size_t spaceIndex = message.find(' ', 9); // Find the first space after "/private "
            if (spaceIndex != string::npos) {
                string recipient = message.substr(9, spaceIndex - 9); // Extract recipient's username
                string privateMessage = message.substr(spaceIndex + 1); // Extract the actual message

                lock_guard<mutex> lock(userMapMutex);
                auto it = userMap.find(recipient);
                if (it != userMap.end()) {
                    SOCKET recipientSocket = it->second;
                    string formattedMessage = "[Private] " + userName + ": " + privateMessage;
                    send(recipientSocket, formattedMessage.c_str(), static_cast<int>(formattedMessage.length()), 0);

                    // Notify the sender of success
                    string confirmation = "Private message sent to " + recipient + "\n";
                    send(clientSocket, confirmation.c_str(), static_cast<int>(confirmation.length()), 0);
                }
                else {
                    // Notify the sender that the recipient was not found
                    string errorMessage = "User " + recipient + " not found or not connected.\n";
                    send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
                }
            }

            else {
                // Notify the sender about incorrect command format
                string errorMessage = "Invalid /private command. Use: /private <username> <message>\n";
                send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
            }
            continue; // Skip further processing for this command
        }

        else if (message.find("/create_group ") == 0) {
            string groupName = message.substr(14); // Extract the group name
            {
                lock_guard<mutex> lock(groupMapMutex);
                if (groupMap.find(groupName) != groupMap.end()) {
                    string errorMessage = "Group " + groupName + " already exists.\n";
                    send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
                }
                else {
                    groupMap[groupName] = set<SOCKET>{ clientSocket }; // Create group and add the creator
                    string successMessage = "Group " + groupName + " created successfully.\n";
                    send(clientSocket, successMessage.c_str(), static_cast<int>(successMessage.length()), 0);
                }
            }
            continue;
        }
        else if (message.find("/join_group ") == 0) {
            string groupName = message.substr(12); // Extract the group name
            {
                lock_guard<mutex> lock(groupMapMutex);
                auto it = groupMap.find(groupName);
                if (it != groupMap.end()) {
                    it->second.insert(clientSocket); // Add client to the group
                    string successMessage = "Joined group " + groupName + " successfully.\n";
                    send(clientSocket, successMessage.c_str(), static_cast<int>(successMessage.length()), 0);
                }
                else {
                    string errorMessage = "Group " + groupName + " does not exist.\n";
                    send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
                }
            }
            continue;
        }
        else if (message.find("/leave_group ") == 0) {
            string groupName = message.substr(13); // Extract the group name
            {
                lock_guard<mutex> lock(groupMapMutex);
                auto it = groupMap.find(groupName);
                if (it != groupMap.end() && it->second.erase(clientSocket)) {
                    string successMessage = "Left group " + groupName + " successfully.\n";
                    send(clientSocket, successMessage.c_str(), static_cast<int>(successMessage.length()), 0);

                    // Remove group if empty
                    if (it->second.empty()) {
                        groupMap.erase(it);
                    }
                }
                else {
                    string errorMessage = "You are not part of group " + groupName + ".\n";
                    send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
                }
            }
            continue;
        }
        if (message.find("/group ") == 0) {
            size_t spaceIndex = message.find(' ', 7); // Find the first space after "/group "
            if (spaceIndex != string::npos) {
                string groupName = message.substr(7, spaceIndex - 7); // Extract group name
                string groupMessage = message.substr(spaceIndex + 1); // Extract the message
                {
                    lock_guard<mutex> lock(groupMapMutex);
                    auto it = groupMap.find(groupName);
                    if (it != groupMap.end() && it->second.count(clientSocket)) {
                        string formattedMessage = "[Group: " + groupName + "] " + userName + ": " + groupMessage + "\n";
                        for (SOCKET member : it->second) {
                            if (member != clientSocket) { // Don't echo back to the sender
                                send(member, formattedMessage.c_str(), static_cast<int>(formattedMessage.length()), 0);
                            }
                        }
                    }
                    else {
                        string errorMessage = "You are not part of group " + groupName + " or the group does not exist.\n";
                        send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
                    }
                }
            }
            else {
                string errorMessage = "Invalid /group command. Use: /group <group_name> <message>\n";
                send(clientSocket, errorMessage.c_str(), static_cast<int>(errorMessage.length()), 0);
            }
            continue;
        }
        if (message.find("/sendfile") == 0) {
            handleFileTransfer(clientSocket);
        }
        else {
            // Forward the message to other clients
            string msg = userName + ": " + message;

            {
                // Lock the clientsMutex to take a snapshot of the current clients
                lock_guard<mutex> lock(clientsMutex);
                clientsSnapshot = clients; // Copy the client list
            }

            for (auto client : clientsSnapshot) {
                if (client != clientSocket) {
                    bool canSend = true;

                    string recipientName;
                    {
                        // Lock blockedUsersMutex only for checking block lists
                        lock_guard<mutex> lock(blockedUsersMutex);

                        // Get the recipient's username
                        auto recipientIt = find_if(userMap.begin(), userMap.end(),
                            [client](const pair<string, SOCKET>& p) {
                                return p.second == client;
                            });

                        if (recipientIt != userMap.end()) {
                            recipientName = recipientIt->first;

                            // Check if the recipient has blocked the sender
                            if (blockedUsers[recipientName].count(userName) > 0) {
                                canSend = false;
                            }

                            // Check if the sender has blocked the recipient
                            if (blockedUsers[userName].count(recipientName) > 0) {
                                canSend = false;
                            }
                        }
                        else {
                            canSend = false; // Skip sending if recipient not found
                        }
                    } // Release blockedUsersMutex here

                    if (canSend) {
                        // Send the message to the recipient
                        send(client, msg.c_str(), static_cast<int>(msg.length()), 0);
                    }
                }
            }
        }
    }

    // Remove username and client socket
    {
        lock_guard<mutex> lock(userNamesMutex);
        userNames.erase(remove(userNames.begin(), userNames.end(), userName), userNames.end());
    }

    {
        lock_guard<mutex> lock(clientsMutex);
        clients.erase(remove(clients.begin(), clients.end(), clientSocket), clients.end());
    }

    closesocket(clientSocket);
}

// Chat server
void chatServer(int port) {
    if (!initialize()) {
        cout << "WinSock Initialization failed" << endl;
        return;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == INVALID_SOCKET) {
        cout << "Socket creation failed: " << WSAGetLastError() << endl;
        WSACleanup();
        return;
    }

    // Create address structure
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;  // Bind to any available interface

    // Bind
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "Binding failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    // Listen
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        cout << "Listening failed: " << WSAGetLastError() << endl;
        closesocket(listenSocket);
        WSACleanup();
        return;
    }

    string serverIP = getServerIPAddress();
    cout << "Server IP Address: " << serverIP << endl;
    cout << "Server port: " << port << endl;
    vector<SOCKET> clients;

    // Accept
    while (true) {
        SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            cout << "Invalid Client Socket: " << WSAGetLastError() << endl;
            closesocket(listenSocket);
            WSACleanup();
            return;
        }
        // Add client to clients vector in a thread-safe way
        {
            lock_guard<mutex> lock(clientsMutex);
            clients.push_back(clientSocket);
        }
        thread t1(interactWithClient, clientSocket, std::ref(clients));
        t1.detach();  // Detach the thread to allow it to run independently
    }
    closesocket(listenSocket);
    WSACleanup();
}

void waitFor(int seconds) {
    this_thread::sleep_for(chrono::seconds(seconds));
}

void announce() {
    if (userMap.empty()) {
        cout << "No users connected for the announcement." << endl;
        return;
    }
    cout << "Enter announcement: ";
    string announcement;
    getline(cin, announcement);  // Now, read the entire announcement
    announcement = "Message from server: " + announcement;
    // Send the announcement to all connected clients
    lock_guard<mutex> lock(userMapMutex);
    for (const auto& pair : userMap) {
        send(pair.second, announcement.c_str(), static_cast<int>(announcement.length()), 0);
    }

    cout << "Announcement successfully sent! " << endl;
}

void stopServer() {
    cout << "Shutting down server..." << endl;
    {
        lock_guard<mutex> lock(userMapMutex);
        for (const auto& pair : userMap) {
            send(pair.second, "Server shutting down...", 24, 0);
            closesocket(pair.second);
        }
        userMap.clear();
    }
    {
        lock_guard<mutex> lock(clientsMutex);
        for (SOCKET client : clientsSnapshot) {
            closesocket(client);
        }
        clientsSnapshot.clear();
    }
    WSACleanup();
}

int main() {
    cout << "Local Messaging and File Sharing Server" << endl;

    int port;
    cout << "Enter port for your server: ";
    cin >> port;
    system("cls");
    // Start server in a separate thread
    thread serverThread(chatServer, port);
    serverThread.detach();

    waitFor(3);
    cout << "\n\nPlease use these commands for following actions:\n\n1./stop: Stop server\n2./list: List all users\n3./kick: Kick users from server\n4./announce: Make an announcement\n5./clear: Clear logs\n6./sendfile <file Name>: Send a file to all clients\n\n";
    string command;
    while (true) {
        cin >> command;
        cin.clear();
        cin.ignore();
        if (command == "/stop") {
            stopServer();
            return 0;
        }
        else if (command == "/list") {
            listUsers();
        }
        else if (command == "/kick") {
            cout << "Enter username to kick: ";
            string username;
            getline(cin, username);
            blockUser(username);
        }
        else if (command == "/announce") {
            announce();
        }
        else if (command == "/clear") {
            system("cls");
            cout << "Server IP Address: " << getServerIPAddress() << endl;
            cout << "Server port: " << port << endl;
            cout << "\n\nPlease use these commands for following actions:\n\n1./stop: Stop server\n2./list: List all users\n3./kick: Kick users from server\n4./announce: Make an announcement\n5./clear: Clear logs\n\n";
        }
    }

    return 0;
}