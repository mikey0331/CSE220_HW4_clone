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

void send_exact_response(int socket, const char *msg) {
    write(socket, msg, strlen(msg));
}

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

int validate_begin_packet(const char* packet, int* width, int* height) {
    // Check if packet is just "B"
    if (strlen(packet) == 1) {
        return 0;
    }
    
    // Check if second character is a space
    if (packet[1] != ' ') {
        return 0;
    }
    
    // Count number of parameters
    int params = 0;
    int w = 0, h = 0;
    char extra;
    int result = sscanf(packet + 2, "%d %d%c", &w, &h, &extra);
    
    // Check if exactly two numbers were read
    if (result != 2) {
        return 0;
    }
    
    // Check if numbers are in valid range
    if (w < 10 || h < 10 || w > MAX_BOARD || h > MAX_BOARD) {
        return 0;
    }
    
    *width = w;
    *height = h;
    return 1;
}

void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    // Handle invalid packet type first
    if (game->phase == 0 && packet[0] != 'B') {
        send_error(current->socket, 100);
        return;
    }

    if (game->phase == 0) {
        if (is_p1) {
            int width, height;
            if (!validate_begin_packet(packet, &width, &height)) {
                send_error(current->socket, 200);
                return;
            }
            game->width = width;
            game->height = height;
        } else {
            // Player 2 should only send "B"
            if (strlen(packet) > 1) {
                send_error(current->socket, 200);
                return;
            }
        }

        send_ack(current->socket);
        current->ready = 1;

        if (game->p1.ready && game->p2.ready) {
            game->phase = 1;
        }
        return;
    }

    if(game->phase == 1) {
        if(packet[0] != 'I') {
            send_error(current->socket, 101);
            return;
        }

        int params[MAX_SHIPS * 4];
        int param_count = 0;
        char *token = strtok(packet + 1, " ");
        
        while(token && param_count < MAX_SHIPS * 4) {
            params[param_count++] = atoi(token);
            token = strtok(NULL, " ");
        }

        if(param_count != MAX_SHIPS * 4) {
            send_error(current->socket, 201);
            return;
        }

        for(int i = 0; i < MAX_SHIPS; i++) {
            int type = params[i * 4];
            if(type < 1 || type > 7) {
                send_error(current->socket, 300);
                return;
            }
        }

        for(int i = 0; i < MAX_SHIPS; i++) {
            int rotation = params[i * 4 + 1];
            if(rotation < 0 || rotation > 3) {
                send_error(current->socket, 301);
                return;
            }
        }

        int temp_board[MAX_BOARD][MAX_BOARD] = {0};
        
        for(int i = 0; i < MAX_SHIPS; i++) {
            int type = params[i * 4];
            int rotation = params[i * 4 + 1];
            int col = params[i * 4 + 2];
            int row = params[i * 4 + 3];

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

                if(new_col < 0 || new_col >= game->width ||
                   new_row < 0 || new_row >= game->height) {
                    send_error(current->socket, 302);
                    return;
                }

                if(temp_board[new_row][new_col]) {
                    send_error(current->socket, 303);
                    return;
                }
                temp_board[new_row][new_col] = 1;
            }
        }

        memcpy(current->board, temp_board, sizeof(temp_board));
        current->ships_remaining = MAX_SHIPS * 4;
        send_ack(current->socket);
        current->ready = 2;

        if(game->p1.ready == 2 && game->p2.ready == 2) {
            game->phase = 2;
            game->current_turn = 1;
        }
        return;
    }

    if(game->phase == 2) {
        if(packet[0] != 'S' && packet[0] != 'Q') {
            send_error(current->socket, 102);
            return;
        }

        if((is_p1 && game->current_turn != 1) || (!is_p1 && game->current_turn != 2)) {
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
        if(sscanf(packet + 1, "%d %d", &row, &col) != 2) {
            send_error(current->socket, 202);
            return;
        }

        if(col < 0 || col >= game->width || row < 0 || row >= game->height) {
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
                return;
            }
        } else {
            send_shot_response(current->socket, other->ships_remaining, 'M');
        }
        game->current_turn = is_p1 ? 2 : 1;
        return;
    }
}


int main() {
    GameState game = {0};
    game.phase = 0;
    game.current_turn = 1;
    memset(&game.p1, 0, sizeof(Player));
    memset(&game.p2, 0, sizeof(Player));

    int server1_fd = socket(AF_INET, SOCK_STREAM, 0);
    int server2_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server1_fd < 0 || server2_fd < 0) {
        return 1;
    }

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

    if (bind(server1_fd, (struct sockaddr *)&addr1, sizeof(addr1)) < 0 ||
        bind(server2_fd, (struct sockaddr *)&addr2, sizeof(addr2)) < 0) {
        return 1;
    }

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
        int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0 && errno != EINTR) {
            break;
        }

        if(FD_ISSET(game.p1.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p1.socket, buffer, BUFFER_SIZE-1);
            if(bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            process_packet(&game, buffer, 1);
        }

        if(FD_ISSET(game.p2.socket, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes = read(game.p2.socket, buffer, BUFFER_SIZE-1);
            if(bytes <= 0) break;
            buffer[bytes] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
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
