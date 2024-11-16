#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>

#define PORT1 2201
#define PORT2 2202
#define BUFFER_SIZE 1024
#define MAX_SHIPS 5
#define MAX_BOARD 20
#define MIN_BOARD 10

typedef struct {
    int type;
    int rotation;
    int row;
    int col;
    int hits;
} Ship;

typedef struct {
    int row;
    int col;
    char result;  // 'H' for hit, 'M' for miss
} Shot;

typedef struct {
    int socket;
    int ready;
    Ship ships[MAX_SHIPS];
    int board[MAX_BOARD][MAX_BOARD];
    Shot shots[MAX_BOARD * MAX_BOARD];
    int num_shots;
    int ships_remaining;
} Player;

typedef struct {
    Player p1;
    Player p2;
    int width;
    int height;
    int phase;  // 0=waiting begin, 1=waiting init, 2=gameplay
    int current_turn;  // 1=p1, 2=p2
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

void send_exact_response(int socket, const char *msg) {
    ssize_t total_sent = 0;
    size_t len = strlen(msg);
    
    while (total_sent < len) {
        ssize_t sent = send(socket, msg + total_sent, len - total_sent, 0);
        if (sent < 0) {
            perror("send failed");
            exit(EXIT_FAILURE);
        }
        total_sent += sent;
    }
    
    if (send(socket, "\n", 1, 0) < 0) {
        perror("send newline failed");
        exit(EXIT_FAILURE);
    }
}

void rotate_point(int *row, int *col, int rotation) {
    int temp;
    for(int i = 0; i < rotation; i++) {
        temp = *row;
        *row = -*col;
        *col = temp;
    }
}

int validate_ship_placement(GameState *game, Ship ship, int board[MAX_BOARD][MAX_BOARD]) {
    if (ship.type < 1 || ship.type > 7) return 300;
    if (ship.rotation < 0 || ship.rotation > 3) return 301;
    
    int piece_idx = ship.type - 1;
    
    for(int i = 0; i < 4; i++) {
        int row = TETRIS_PIECES[piece_idx][i][0];
        int col = TETRIS_PIECES[piece_idx][i][1];
        
        rotate_point(&row, &col, ship.rotation);
        row += ship.row;
        col += ship.col;
        
        if(row < 0 || row >= game->height || col < 0 || col >= game->width) {
            return 302;
        }
        
        if(board[row][col]) {
            return 303;
        }
        
        board[row][col] = 1;
    }
    return 0;
}

void place_ships(GameState *game, Player *player, Ship *ships) {
    memset(player->board, 0, sizeof(player->board));
    player->ships_remaining = MAX_SHIPS * 4;  // Each ship has 4 cells
    
    for(int i = 0; i < MAX_SHIPS; i++) {
        memcpy(&player->ships[i], &ships[i], sizeof(Ship));
        int piece_idx = ships[i].type - 1;
        
        for(int j = 0; j < 4; j++) {
            int row = TETRIS_PIECES[piece_idx][j][0];
            int col = TETRIS_PIECES[piece_idx][j][1];
            rotate_point(&row, &col, ships[i].rotation);
            row += ships[i].row;
            col += ships[i].col;
            player->board[row][col] = i + 1;
        }
    }
}

int validate_init(GameState *game, char *packet, Ship *ships) {
    if (!packet || strlen(packet) < 2) return 201;
    
    char *token = strtok(packet + 2, " ");
    for(int i = 0; i < MAX_SHIPS; i++) {
        if(!token) return 201;
        ships[i].type = atoi(token);
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].rotation = atoi(token);
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].row = atoi(token);
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].col = atoi(token);
        
        ships[i].hits = 0;
        token = strtok(NULL, " ");
    }

    int board[MAX_BOARD][MAX_BOARD] = {0};
    for(int i = 0; i < MAX_SHIPS; i++) {
        int result = validate_ship_placement(game, ships[i], board);
        if(result != 0) return result;
    }
    return 0;
}

int handle_begin(GameState *game, char *packet, int player) {
    if (player == 1) {
        if (strlen(packet) < 2) return 200;
        char *token = strtok(packet + 2, " ");
        if (!token) return 200;
        game->width = atoi(token);
        
        token = strtok(NULL, " ");
        if (!token) return 200;
        game->height = atoi(token);
        
        if (game->width < MIN_BOARD || game->height < MIN_BOARD) return 200;
        
        token = strtok(NULL, " ");
        if (token) return 200;
    } else {
        if (strlen(packet) > 2) return 200;
    }
    return 0;
}

void build_query_response(Player *player, char *response, int max_len) {
    snprintf(response, max_len, "G %d", player->ships_remaining);
    int pos = strlen(response);
    
    for (int row = 0; row < MAX_BOARD; row++) {
        for (int col = 0; col < MAX_BOARD; col++) {
            for (int i = 0; i < player->num_shots; i++) {
                if (player->shots[i].row == row && player->shots[i].col == col) {
                    pos += snprintf(response + pos, max_len - pos, 
                                  " %c %d %d", 
                                  player->shots[i].result,
                                  player->shots[i].col,
                                  player->shots[i].row);
                }
            }
        }
    }
}

char process_shot(GameState *game, Player *shooter, Player *target, int row, int col) {
    if (row < 0 || row >= game->height || col < 0 || col >= game->width) {
        return 400;
    }
    
    for (int i = 0; i < shooter->num_shots; i++) {
        if (shooter->shots[i].row == row && shooter->shots[i].col == col) {
            return 401;
        }
    }
    
    Shot new_shot = {row, col, 'M'};
    
    if (target->board[row][col] > 0) {
        new_shot.result = 'H';
        target->ships_remaining--;
    }
    
    shooter->shots[shooter->num_shots++] = new_shot;
    return new_shot.result;
}

void send_halt(int socket, int is_winner) {
    char response[16];
    sprintf(response, "H %d", is_winner);
    send_exact_response(socket, response);
}

void handle_game_turn(GameState *game, char *packet, Player *current, Player *other) {
    char response[BUFFER_SIZE];
    
    switch(packet[0]) {
        case 'Q':
            if (strlen(packet) != 1) {
                send_exact_response(current->socket, "E 102");
                return;
            }
            build_query_response(other, response, BUFFER_SIZE);
            send_exact_response(current->socket, response);
            break;
            
        case 'S': {
            int row, col;
            if (sscanf(packet + 2, "%d %d", &row, &col) != 2) {
                send_exact_response(current->socket, "E 202");
                return;
            }
            
            char result = process_shot(game, current, other, row, col);
            if (result >= 400) {
                sprintf(response, "E %d", result);
            } else {
                sprintf(response, "R %d %c", other->ships_remaining, result);
            }
            send_exact_response(current->socket, response);
            
            if (other->ships_remaining == 0) {
                send_halt(other->socket, 0);
                send_halt(current->socket, 1);
                game->phase = -1;
                return;
            }
            game->current_turn = (game->current_turn == 1) ? 2 : 1;
            break;
        }
            
        case 'F':
            if (strlen(packet) != 1) {
                send_exact_response(current->socket, "E 102");
                return;
            }
            send_halt(other->socket, 1);
            send_halt(current->socket, 0);
            game->phase = -1;
            break;
            
        default:
            send_exact_response(current->socket, "E 102");
    }
}

int setup_socket(int port) {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_socket, 1) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    
    return server_socket;
}

int main() {
    GameState game = {0};
    game.phase = 0;
    game.current_turn = 1;
    
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
    }
    
    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);
    
    return 0;
}

