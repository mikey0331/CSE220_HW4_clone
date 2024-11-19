
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
#define SHIP_SIZE 4

typedef struct {
    int cells[SHIP_SIZE][2];
    int hits;
} Ship;

typedef struct {
    int socket;
    int ready;
    int board[MAX_BOARD][MAX_BOARD];
    int shots[MAX_BOARD][MAX_BOARD];
    Ship ships[MAX_SHIPS];
    int ships_remaining;
    int ship_count;
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

int validate_board_command(const char* packet, int is_p1) {
    char *temp = strdup(packet + 1);
    char *token = strtok(temp, " ");
    int param_count = 0;
    while (token) {
        param_count++;
        token = strtok(NULL, " ");
    }
    free(temp);

    if (is_p1) {
        if (param_count != 2) return 0;
        int w = 0, h = 0;
        if (sscanf(packet + 1, "%d %d", &w, &h) != 2) return 0;
        if (w < 10 || h < 10) return 0;
    } else {
        if (param_count != 0) return 0;
    }
    return 1;
}

int check_positions(int positions[4][2], int width, int height, int board[MAX_BOARD][MAX_BOARD]) {
    for (int i = 0; i < 4; i++) {
        int row = positions[i][0];
        int col = positions[i][1];
        
        if (row < 0 || row >= height || col < 0 || col >= width) {
            return 302;
        }
        
        if (board[row][col]) {
            return 303;
        }
    }
    return 0;
}

int check_ship_placement(int piece_type, int rotation, int row, int col,
                        int width, int height, int board[MAX_BOARD][MAX_BOARD]) {
    if (piece_type < 0 || piece_type >= 7) return 300;
    if (rotation < 0 || rotation >= 4) return 301;

    int positions[4][2];
    for (int i = 0; i < 4; i++) {
        positions[i][0] = row + TETRIS_PIECES[piece_type][i][0];
        positions[i][1] = col + TETRIS_PIECES[piece_type][i][1];
    }

    return check_positions(positions, width, height, board);
}

void place_ship(Player *player, int ship_index, int piece_type, int row, int col) {
    for (int i = 0; i < 4; i++) {
        int new_row = row + TETRIS_PIECES[piece_type][i][0];
        int new_col = col + TETRIS_PIECES[piece_type][i][1];
        player->board[new_row][new_col] = 1;
        player->ships[ship_index].cells[i][0] = new_row;
        player->ships[ship_index].cells[i][1] = new_col;
    }
    player->ships[ship_index].hits = 0;
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
                sscanf(packet + 1, "%d %d", &game->width, &game->height);
            }

            current->ready = 1;
            current->ships_remaining = MAX_SHIPS;
            current->ship_count = 0;
            memset(current->board, 0, sizeof(current->board));
            memset(current->shots, 0, sizeof(current->shots));
            send_ack(current->socket);
            
            if (game->p1.ready && game->p2.ready) {
                game->phase = 1;
            }
            break;

        case 1:  // Ship placement
            if (packet[0] != 'I') {
                send_error(current->socket, 101);
                return;
            }

            char *str = packet + 1;
            int values[20];
            int count = 0;
            
            while (*str && count < 20) {
                while (*str == ' ') str++;
                if (sscanf(str, "%d", &values[count]) != 1) {
                    send_error(current->socket, 201);
                    return;
                }
                count++;
                while (*str && *str != ' ') str++;
            }

            if (count != 20) {
                send_error(current->socket, 201);
                return;
            }

            // Validate all ships first
            for (int i = 0; i < MAX_SHIPS; i++) {
                int error = check_ship_placement(
                    values[i*4],     // piece_type
                    values[i*4 + 1], // rotation
                    values[i*4 + 2], // row
                    values[i*4 + 3], // col
                    game->width, game->height, current->board
                );
                if (error) {
                    send_error(current->socket, error);
                    return;
                }
            }

            // Place all ships
            for (int i = 0; i < MAX_SHIPS; i++) {
                place_ship(current, i, values[i*4], values[i*4 + 2], values[i*4 + 3]);
            }

            current->ready = 2;
            send_ack(current->socket);
            
            if (game->p1.ready == 2 && game->p2.ready == 2) {
                game->phase = 2;
                game->current_turn = 1;
            }
            break;

        case 2:  // Gameplay
            if ((is_p1 && game->current_turn != 1) || (!is_p1 && game->current_turn != 2)) {
                send_error(current->socket, 102);
                return;
            }

            if (packet[0] == 'S') {
                int row, col;
                char extra[32];
                int matched = sscanf(packet + 1, "%d %d%[^\n]", &row, &col, extra);
                
                if (matched != 2) {
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

                int was_hit = other->board[row][col];
                current->shots[row][col] = 1;
                
                if (was_hit) {
                    // Find which ship was hit
                    for (int s = 0; s < MAX_SHIPS; s++) {
                        for (int c = 0; c < SHIP_SIZE; c++) {
                            if (other->ships[s].cells[c][0] == row && 
                                other->ships[s].cells[c][1] == col) {
                                other->ships[s].hits++;
                                if (other->ships[s].hits == SHIP_SIZE) {
                                    other->ships_remaining--;
                                }
                                break;
                            }
                        }
                    }
                    
                    send_shot_response(current->socket, other->ships_remaining, 'H');
                    if (other->ships_remaining == 0) {
                        send_halt(other->socket, 0);
                        send_halt(current->socket, 1);
                        game->phase = 3;
                        return;
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

    if ((server1_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0 ||
        (server2_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server1_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ||
        setsockopt(server2_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = INADDR_ANY;
    addr1.sin_port = htons(PORT1);
    
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = INADDR_ANY;
    addr2.sin_port = htons(PORT2);

    if (bind(server1_fd, (struct sockaddr *)&addr1, sizeof(addr1)) < 0 ||
        bind(server2_fd, (struct sockaddr *)&addr2, sizeof(addr2)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server1_fd, 1) < 0 || listen(server2_fd, 1) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    game.p1.socket = accept(server1_fd, NULL, NULL);
    game.p2.socket = accept(server2_fd, NULL, NULL);

    if (game.p1.socket < 0 || game.p2.socket < 0) {
        perror("accept failed");
        exit(EXIT_FAILURE);
    }

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