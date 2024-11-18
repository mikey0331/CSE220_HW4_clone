#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <netinet/in.h>

#define PORT1 2201
#define PORT2 2202
#define BUFFER_SIZE 1024
#define MAX_SHIPS 5
#define MAX_BOARD 20

typedef struct {
    int socket;
    int ready;
    int board[MAX_BOARD][MAX_BOARD];
    int shots[MAX_BOARD][MAX_BOARD];
    int ships_remaining;
} Player;

typedef struct {
    Player p1;
    Player p2;
    int width;
    int height;
    int phase;
    int current_turn;
} GameState;

const int TETRIS_PIECES[7][4][2] = {
    {{0,0}, {0,1}, {0,2}, {0,3}}, // I
    {{0,0}, {0,1}, {1,0}, {1,1}}, // O
    {{0,1}, {1,0}, {1,1}, {1,2}}, // T
    {{0,0}, {1,0}, {2,0}, {2,1}}, // J
    {{0,0}, {1,0}, {2,0}, {2,-1}}, // L
    {{0,0}, {0,1}, {1,-1}, {1,0}}, // S
    {{0,-1}, {0,0}, {1,0}, {1,1}} // Z
};

void send_response(int socket, const char *msg) {
    write(socket, msg, strlen(msg));
}

void send_error(int socket, int code) {
    char response[16];
    sprintf(response, "E %d", code);
    send_response(socket, response);
}

void send_ack(int socket) {
    send_response(socket, "A");
}

void send_halt(int socket, int is_winner) {
    char response[16];
    sprintf(response, "H %d", is_winner);
    send_response(socket, response);
}

void send_shot_response(int socket, int ships_remaining, char result) {
    char response[32];
    sprintf(response, "R %d %c", ships_remaining, result);
    send_response(socket, response);
}

int verify_ship_placement(GameState *game, Player *current, int positions[4][2]) {
    for (int i = 0; i < 4; i++) {
        if (positions[i][0] < 0 || positions[i][0] >= game->height ||
            positions[i][1] < 0 || positions[i][1] >= game->width) {
            return 302;
        }
    }
    
    for (int i = 0; i < 4; i++) {
        if (current->board[positions[i][0]][positions[i][1]]) {
            return 303;
        }
    }
    
    return 0;
}

int validate_board_command(const char* packet, int is_p1) {
    // Skip 'B' and count parameters
    char *temp = strdup(packet + 1);
    char *token = strtok(temp, " ");
    int param_count = 0;
    while (token) {
        param_count++;
        token = strtok(NULL, " ");
    }
    free(temp);

    // P1 must have exactly 2 parameters, P2 must have 0
    if (is_p1 && param_count != 2) return 0;
    if (!is_p1 && param_count != 0) return 0;

    if (is_p1) {
        int w = 0, h = 0;
        if (sscanf(packet + 1, "%d %d", &w, &h) != 2) return 0;
        if (w < 10 || h < 10) return 0;
    }
    return 1;
}

void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    while (*packet == ' ') packet++;

    if (packet[0] == 'F') {
        send_halt(current->socket, 0);
        send_halt(other->socket, 1);
        game->phase = 3;
        return;
    }

    switch (game->phase) {
        case 0:  // Board setup
            if (packet[0] != 'B') {
                send_error(current->socket, 100);
                return;
            }
            
            if (!validate_board_command(packet, is_p1)) {
                send_error(current->socket, 200);
                return;
            }

            if (is_p1) {
                int w, h;
                sscanf(packet + 1, "%d %d", &w, &h);
                game->width = w;
                game->height = h;
            }

            send_ack(current->socket);
            current->ready = 1;
            
            if (game->p1.ready && game->p2.ready) {
                game->phase = 1;
            }
            break;

        case 1:  // Ship placement
            if (packet[0] != 'I') {
                send_error(current->socket, 101);
                return;
            }

            // Count and validate parameters
            int params[MAX_SHIPS * 4] = {0};
            int param_count = 0;
            char *temp = strdup(packet + 1);
            char *token = strtok(temp, " ");
            
            while (token && param_count < MAX_SHIPS * 4) {
                params[param_count++] = atoi(token);
                token = strtok(NULL, " ");
            }
            free(temp);

            if (param_count != MAX_SHIPS * 4) {
                send_error(current->socket, 201);
                return;
            }

            // Validate piece types first
            for (int i = 0; i < MAX_SHIPS; i++) {
                int type = params[i * 4];
                if (type < 1 || type > 7) {
                    send_error(current->socket, 300);
                    return;
                }
                int rotation = params[i * 4 + 1];
                if (rotation < 0 || rotation > 3) {
                    send_error(current->socket, 301);
                    return;
                }
            }

            // Clear board
            memset(current->board, 0, sizeof(current->board));
            current->ships_remaining = 20;  // 5 ships * 4 pieces each

            // Place ships
            for (int i = 0; i < MAX_SHIPS; i++) {
                int type = params[i * 4] - 1;
                int rotation = params[i * 4 + 1];
                int col = params[i * 4 + 2];
                int row = params[i * 4 + 3];

                int positions[4][2];
                for (int j = 0; j < 4; j++) {
                    int piece_row = TETRIS_PIECES[type][j][0];
                    int piece_col = TETRIS_PIECES[type][j][1];
                    
                    for (int r = 0; r < rotation; r++) {
                        int temp = piece_row;
                        piece_row = -piece_col;
                        piece_col = temp;
                    }
                    
                    positions[j][0] = row + piece_row;
                    positions[j][1] = col + piece_col;
                    
                    // Check boundaries
                    if (positions[j][0] < 0 || positions[j][0] >= game->height ||
                        positions[j][1] < 0 || positions[j][1] >= game->width) {
                        send_error(current->socket, 302);
                        return;
                    }
                    
                    // Check overlap
                    if (current->board[positions[j][0]][positions[j][1]]) {
                        send_error(current->socket, 303);
                        return;
                    }
                }

                // Place ship
                for (int j = 0; j < 4; j++) {
                    current->board[positions[j][0]][positions[j][1]] = 1;
                }
            }

            send_ack(current->socket);
            current->ready = 2;
            if (game->p1.ready == 2 && game->p2.ready == 2) {
                game->phase = 2;
                game->current_turn = 1;
            }
            break;

        case 2:  // Gameplay
            if (packet[0] == 'S') {
                if ((is_p1 && game->current_turn != 1) || (!is_p1 && game->current_turn != 2)) {
                    send_error(current->socket, 102);
                    return;
                }

                int row, col;
                char extra;
                if (sscanf(packet + 1, "%d %d%c", &row, &col, &extra) != 2) {
                    send_error(current->socket, 202);
                    return;
                }

                if (row < 0 || row >= game->height || col < 0 || col >= game->width) {
                    send_error(current->socket, 400);
                    return;
                }

                if (current->shots[row][col]) {
                    send_error(current->socket, 401);
                    return;
                }

                current->shots[row][col] = 1;
                if (other->board[row][col]) {
                    other->ships_remaining--;
                    send_shot_response(current->socket, other->ships_remaining, 'H');
                    if (other->ships_remaining == 0) {
                        send_halt(other->socket, 0);
                        send_halt(current->socket, 1);
                        game->phase = 3;
                    }
                } else {
                    send_shot_response(current->socket, other->ships_remaining, 'M');
                }
                game->current_turn = game->current_turn == 1 ? 2 : 1;
            }
            else if (packet[0] == 'Q') {
                char response[BUFFER_SIZE] = {0};
                sprintf(response, "G %d", other->ships_remaining);
                for (int i = 0; i < game->height; i++) {
                    for (int j = 0; j < game->width; j++) {
                        if (current->shots[i][j]) {
                            char hit = other->board[i][j] ? 'H' : 'M';
                            sprintf(response + strlen(response), " %c %d %d", hit, i, j);
                        }
                    }
                }
                send_response(current->socket, response);
            }
            else {
                send_error(current->socket, 102);
            }
            break;
    }
}
int main() {
    int server1_fd, server2_fd;
    struct sockaddr_in addr1, addr2;
    int opt = 1;
    GameState game = {0};
    game.phase = 0;
    game.current_turn = 1;

    // Create sockets
    if ((server1_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0 ||
        (server2_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options
    if (setsockopt(server1_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ||
        setsockopt(server2_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // Configure addresses
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = INADDR_ANY;
    addr1.sin_port = htons(PORT1);
    
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = INADDR_ANY;
    addr2.sin_port = htons(PORT2);

    // Bind sockets
    if (bind(server1_fd, (struct sockaddr *)&addr1, sizeof(addr1)) < 0 ||
        bind(server2_fd, (struct sockaddr *)&addr2, sizeof(addr2)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server1_fd, 1) < 0 || listen(server2_fd, 1) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    // Accept connections
    game.p1.socket = accept(server1_fd, NULL, NULL);
    game.p2.socket = accept(server2_fd, NULL, NULL);

    if (game.p1.socket < 0 || game.p2.socket < 0) {
        perror("accept failed");
        exit(EXIT_FAILURE);
    }

    // Game loop
    char buffer[BUFFER_SIZE];
    fd_set readfds;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(game.p1.socket, &readfds);
        FD_SET(game.p2.socket, &readfds);
        
        int maxfd = (game.p1.socket > game.p2.socket) ? game.p1.socket : game.p2.socket;
        
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0 && errno != EINTR) {
            break;
        }

        if (FD_ISSET(game.p1.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p1.socket, buffer, BUFFER_SIZE-1);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            process_packet(&game, buffer, 1);
        }

        if (FD_ISSET(game.p2.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p2.socket, buffer, BUFFER_SIZE-1);
            if (bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            process_packet(&game, buffer, 0);
        }

        if (game.phase == 3) break;
    }

    // Cleanup
    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);
    return 0;
}