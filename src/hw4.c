#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define PORT1 2201
#define PORT2 2202
#define BUFFER_SIZE 1024
#define MAX_BOARD 20

typedef struct {
    int socket;
    int board[MAX_BOARD][MAX_BOARD];
    int shots[MAX_BOARD][MAX_BOARD];
    int ships_remaining;
    int initialized;
} Player;

typedef struct {
    int width;
    int height;
    Player p1;
    Player p2;
    int phase;
    int current_player;
} GameState;

void send_packet(int socket, const char *msg) {
    write(socket, msg, strlen(msg));
    write(socket, "\n", 1);
}

void handle_packet(GameState *game, char *packet, int player_num) {
    Player *current = (player_num == 1) ? &game->p1 : &game->p2;
    Player *other = (player_num == 1) ? &game->p2 : &game->p1;

    // Phase validation
    if (game->phase == 0 && packet[0] != 'B') {
        send_packet(current->socket, "E 100");
        return;
    }
    if (game->phase == 1 && packet[0] != 'I') {
        send_packet(current->socket, "E 101");
        return;
    }
    if (game->phase == 2 && packet[0] != 'S' && packet[0] != 'Q') {
        send_packet(current->socket, "E 102");
        return;
    }

    switch(packet[0]) {
        case 'F':
            send_packet(current->socket, "H 0");
            send_packet(other->socket, "H 1");
            game->phase = 3;
            break;

        case 'B':
            if (player_num == 1) {
                int w, h;
                if (sscanf(packet, "B %d %d", &w, &h) != 2 || w < 10 || h < 10) {
                    send_packet(current->socket, "E 200");
                    return;
                }
                game->width = w;
                game->height = h;
            }
            send_packet(current->socket, "A");
            current->initialized = 1;
            if (game->p1.initialized && game->p2.initialized) {
                game->phase = 1;
            }
            break;

        case 'I':
            if (!current->initialized) {
                send_packet(current->socket, "E 101");
                return;
            }
            send_packet(current->socket, "A");
            current->initialized = 2;
            if (game->p1.initialized == 2 && game->p2.initialized == 2) {
                game->phase = 2;
                game->current_player = 1;
            }
            break;

        case 'S':
            if (game->current_player != player_num) {
                send_packet(current->socket, "E 103");
                return;
            }

            int row, col;
            if (sscanf(packet, "S %d %d", &row, &col) != 2) {
                send_packet(current->socket, "E 202");
                return;
            }

            if (row < 0 || row >= game->height || col < 0 || col >= game->width) {
                send_packet(current->socket, "E 400");
                return;
            }

            if (current->shots[row][col]) {
                send_packet(current->socket, "E 401");
                return;
            }

            current->shots[row][col] = 1;
            if (other->board[row][col]) {
                other->ships_remaining--;
                char response[32];
                sprintf(response, "R %d H", other->ships_remaining);
                send_packet(current->socket, response);
                
                if (other->ships_remaining == 0) {
                    send_packet(current->socket, "H 1");
                    send_packet(other->socket, "H 0");
                    game->phase = 3;
                    return;
                }
            } else {
                char response[32];
                sprintf(response, "R %d M", other->ships_remaining);
                send_packet(current->socket, response);
            }
            game->current_player = (game->current_player == 1) ? 2 : 1;
            break;

        case 'Q':
            if (game->current_player != player_num) {
                send_packet(current->socket, "E 103");
                return;
            }
            char response[BUFFER_SIZE];
            sprintf(response, "G %d", other->ships_remaining);
            for (int i = 0; i < game->height; i++) {
                for (int j = 0; j < game->width; j++) {
                    if (current->shots[i][j]) {
                        sprintf(response + strlen(response), " %c %d %d", 
                                other->board[i][j] ? 'H' : 'M', i, j);
                    }
                }
            }
            send_packet(current->socket, response);
            break;
    }
}

int main() {
    GameState game = {0};
    game.phase = 0;
    
    int server1_fd = socket(AF_INET, SOCK_STREAM, 0);
    int server2_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    int opt = 1;
    setsockopt(server1_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server2_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr1 = {0}, addr2 = {0};
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = INADDR_ANY;
    addr1.sin_port = htons(PORT1);
    
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = INADDR_ANY;
    addr2.sin_port = htons(PORT2);
    
    bind(server1_fd, (struct sockaddr *)&addr1, sizeof(addr1));
    bind(server2_fd, (struct sockaddr *)&addr2, sizeof(addr2));
    
    listen(server1_fd, 1);
    listen(server2_fd, 1);
    
    game.p1.socket = accept(server1_fd, NULL, NULL);
    game.p2.socket = accept(server2_fd, NULL, NULL);
    
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(game.p1.socket, &readfds);
        FD_SET(game.p2.socket, &readfds);
        
        int maxfd = (game.p1.socket > game.p2.socket) ? game.p1.socket : game.p2.socket;
        select(maxfd + 1, &readfds, NULL, NULL, NULL);
        
        if (FD_ISSET(game.p1.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p1.socket, buffer, BUFFER_SIZE-1);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = 0;
            handle_packet(&game, buffer, 1);
        }
        
        if (FD_ISSET(game.p2.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p2.socket, buffer, BUFFER_SIZE-1);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = 0;
            handle_packet(&game, buffer, 2);
        }
        
        if (game.phase == 3) break;
    }
    
    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);
    
    return 0;
}
