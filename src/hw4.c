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
    int type;
    int rotation;
    int row;
    int col;
    int hits;
} Ship;

typedef struct {
    int socket;
    int ready;
    Ship ships[MAX_SHIPS];
    int num_ships;
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
    {{0,0}, {0,1}, {0,2}, {0,3}},     // I
    {{0,0}, {0,1}, {1,0}, {1,1}},     // O
    {{0,1}, {1,0}, {1,1}, {1,2}},     // T
    {{0,0}, {1,0}, {2,0}, {2,1}},     // J
    {{0,0}, {1,0}, {2,0}, {2,-1}},    // L
    {{0,0}, {0,1}, {1,-1}, {1,0}},    // S
    {{0,-1}, {0,0}, {1,0}, {1,1}}     // Z
};

void send_error(int socket, int code) {
    char response[16];
    sprintf(response, "E %d", code);
    write(socket, response, strlen(response));
}

void send_ack(int socket) {
    write(socket, "A", 1);
}

void send_halt(int socket, int is_winner) {
    char response[16];
    sprintf(response, "H %d", is_winner);
    write(socket, response, strlen(response));
}

void send_shot_response(int socket, int ships_remaining, char result) {
    char response[32];
    sprintf(response, "R %d %c", ships_remaining, result);
    write(socket, response, strlen(response));
}

void rotate_point(int *row, int *col, int rotation) {
    int temp;
    for(int i = 0; i < rotation; i++) {
        temp = *row;
        *row = -*col;
        *col = temp;
    }
}

void place_ships(GameState *game, Player *player, Ship *ships) {
    memset(player->board, 0, sizeof(player->board));
    for(int i = 0; i < MAX_SHIPS; i++) {
        int piece_idx = ships[i].type - 1;
        for(int j = 0; j < 4; j++) {
            int row = TETRIS_PIECES[piece_idx][j][0];
            int col = TETRIS_PIECES[piece_idx][j][1];
            rotate_point(&row, &col, ships[i].rotation);
            row += ships[i].row;
            col += ships[i].col;
            player->board[row][col] = 1;
        }
    }
    player->ships_remaining = MAX_SHIPS * 4;
}

void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    // Handle Forfeit first
    if(packet[0] == 'F') {
        send_halt(current->socket, 0);
        send_halt(other->socket, 1);
        game->phase = 3;
        return;
    }

    // Phase 0: Begin phase
    if(game->phase == 0) {
        if(packet[0] != 'B') {
            send_error(current->socket, 100);
            return;
        }

        char cmd;
        int w, h, params;
        params = sscanf(packet, "%c %d %d", &cmd, &w, &h);
        
        if(is_p1) {
            if(params != 3 || w < 10 || h < 10) {
                send_error(current->socket, 200);
                return;
            }
            game->width = w;
            game->height = h;
        } else {
            if(params != 1) {
                send_error(current->socket, 200);
                return;
            }
        }
        
        send_ack(current->socket);
        current->ready = 1;
        if(game->p1.ready && game->p2.ready) {
            game->phase = 1;
        }
        return;
    }

    // Phase 1: Initialize phase
    if(game->phase == 1) {
        if(packet[0] != 'I') {
            send_error(current->socket, 101);
            return;
        }

        char *temp_packet = strdup(packet);
        char *token = strtok(temp_packet, " ");
        int param_count = -1;
        while(token != NULL) {
            param_count++;
            token = strtok(NULL, " ");
        }
        free(temp_packet);

        if(param_count != MAX_SHIPS * 4) {
            send_error(current->socket, 201);
            return;
        }

        token = strtok(packet + 2, " ");
        int temp_board[MAX_BOARD][MAX_BOARD] = {0};
        
        for(int i = 0; i < MAX_SHIPS; i++) {
            int type = atoi(token);
            if(type < 1 || type > 7) {
                send_error(current->socket, 300);
                return;
            }
            
            token = strtok(NULL, " ");
            int rotation = atoi(token);
            if(rotation < 0 || rotation > 3) {
                send_error(current->socket, 301);
                return;
            }
            
            token = strtok(NULL, " ");
            int row = atoi(token);
            token = strtok(NULL, " ");
            int col = atoi(token);

            int piece_idx = type - 1;
            for(int j = 0; j < 4; j++) {
                int new_row = TETRIS_PIECES[piece_idx][j][0];
                int new_col = TETRIS_PIECES[piece_idx][j][1];
                
                for(int r = 0; r < rotation; r++) {
                    int temp = new_row;
                    new_row = -new_col;
                    new_col = temp;
                }
                
                new_row += row;
                new_col += col;
                
                if(new_row < 0 || new_row >= game->height || 
                   new_col < 0 || new_col >= game->width) {
                    send_error(current->socket, 302);
                    return;
                }
                if(temp_board[new_row][new_col]) {
                    send_error(current->socket, 303);
                    return;
                }
                temp_board[new_row][new_col] = 1;
            }
            token = strtok(NULL, " ");
        }

        memcpy(current->board, temp_board, sizeof(temp_board));
        current->ships_remaining = MAX_SHIPS * 4;
        
        send_ack(current->socket);
        current->ready = 2;
        if(game->p1.ready == 2 && game->p2.ready == 2) {
            game->phase = 2;
        }
        return;
    }

    // Phase 2: Game play phase
    if(game->phase == 2) {
        if(packet[0] != 'S' && packet[0] != 'Q') {
            send_error(current->socket, 102);
            return;
        }

        if(packet[0] == 'Q') {
            char response[BUFFER_SIZE] = {0};
            sprintf(response, "G %d", other->ships_remaining);
            for(int i = 0; i < game->height; i++) {
                for(int j = 0; j < game->width; j++) {
                    if(current->shots[i][j]) {
                        char hit = other->board[i][j] ? 'H' : 'M';
                        sprintf(response + strlen(response), " %c %d %d", hit, i, j);
                    }
                }
            }
            write(current->socket, response, strlen(response));
            return;
        }

        int row, col;
        if(sscanf(packet + 2, "%d %d", &row, &col) != 2) {
            send_error(current->socket, 202);
            return;
        }
        
        if(row < 0 || row >= game->height || col < 0 || col >= game->width) {
            send_error(current->socket, 400);
            return;
        }
        
        if(current->shots[row][col]) {
            send_error(current->socket, 401);
            return;
        }

        current->shots[row][col] = 1;
        if(other->board[row][col]) {
            other->ships_remaining--;
            send_shot_response(current->socket, other->ships_remaining, 'H');
            if(other->ships_remaining == 0) {
                send_halt(other->socket, 0);
                send_halt(current->socket, 1);
                game->phase = 3;
            }
        } else {
            send_shot_response(current->socket, other->ships_remaining, 'M');
        }
    }
}


int main() {
    GameState game = {0};
    game.phase = 0;
    game.current_turn = 1;

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

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(game.p1.socket, &readfds);
        FD_SET(game.p2.socket, &readfds);
        int maxfd = (game.p1.socket > game.p2.socket) ? game.p1.socket : game.p2.socket;
        select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if(FD_ISSET(game.p1.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p1.socket, buffer, BUFFER_SIZE-1);
            if(bytes <= 0) break;
            buffer[bytes] = '\0';
            process_packet(&game, buffer, 1);
        }

        if(FD_ISSET(game.p2.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p2.socket, buffer, BUFFER_SIZE-1);
            if(bytes <= 0) break;
            buffer[bytes] = '\0';
            process_packet(&game, buffer, 0);
        }

        if(game.phase == 3) break;
    }

    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);
    return 0;
}
