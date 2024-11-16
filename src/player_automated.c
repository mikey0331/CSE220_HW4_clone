#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT1 2201
#define PORT2 2202
#define BUFFER_SIZE 1024

void getInput(char* prompt, char* buffer) {
    printf("%s", prompt);
    fgets(buffer, BUFFER_SIZE, stdin);
}

int main(int argc, char **argv) {
    FILE *fp;
    fp = fopen(argv[1], "r");
    char player_number[2];
    getInput("Which player are you? (1 or 2)", player_number);
    int client_fd = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};

    // Create socket
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[Client] socket() failed.");
        exit(EXIT_FAILURE);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(player_number[0]=='1' ? PORT1 : PORT2);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        perror("[Client] Invalid address/ Address not supported.");
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Client] connect() failed.");
        exit(EXIT_FAILURE);
    }
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        buffer[strcspn(buffer, "\r\n")] = 0;
        send(client_fd, buffer, strlen(buffer), 0);
        memset(buffer, 0, BUFFER_SIZE);
        int nbytes = read(client_fd, buffer, BUFFER_SIZE);
        if (nbytes <= 0) {
            perror("[Client] read() failed.");
            exit(EXIT_FAILURE);
        }
        printf("[Client%c] Received from server: %s\n", player_number[0], buffer);
        if (strcmp(buffer, "H 1") == 0) {
            printf("[Client%c] We have Won!\n",player_number[0]);
            break;  
        }
        if (strcmp(buffer, "H 0") == 0) {
            printf("[Client%c] We have Lost!\n",player_number[0]);
            break;  
        }
    }

    printf("[Client%c] Shutting down.\n",player_number[0]);
    close(client_fd);
    return 0;
}