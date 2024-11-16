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
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s\n", msg);
    send(socket, buffer, strlen(buffer), 0);
}

void handle_packet(GameState *game, char *packet, int player_num) {
    Player *current = (player_num == 1) ? &game->p1 : &game->p2;
    Player *other = (player_num == 1) ? &game->p2 : &game->p1;

    switch(packet[0]) {
        case 'F':
            send_packet(current->socket, "H 0");
            send_packet(other->socket, "H 1");
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
            break;
            
        case 'I':
            if (!game->width || !game->height) {
                send_packet(current->socket, "E 101");
                return;
            }
            send_packet(current->socket, "A");
            break;
            
        case 'S':
            if (!current->initialized) {
                send_packet(current->socket, "E 102");
                return;
            }
            send_packet(current->socket, "A");
            break;
            
        case 'Q':
            if (!current->initialized) {
                send_packet(current->socket, "E 102");
                return;
            }
            send_packet(current->socket, "A");
            break;
            
        default:
            send_packet(current->socket, "E 100");
    }
}

int main() {
    GameState game = {0};
    int server1_fd = socket(AF_INET, SOCK_STREAM, 0);
    int server2_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in addr1 = {0}, addr2 = {0};
    int opt = 1;
    
    setsockopt(server1_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server2_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
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
            if (read(game.p1.socket, buffer, BUFFER_SIZE) <= 0) break;
            buffer[strcspn(buffer, "\n")] = 0;
            handle_packet(&game, buffer, 1);
        }
        
        if (FD_ISSET(game.p2.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            if (read(game.p2.socket, buffer, BUFFER_SIZE) <= 0) break;
            buffer[strcspn(buffer, "\n")] = 0;
            handle_packet(&game, buffer, 2);
        }
    }
    
    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);
    
    return 0;
}
