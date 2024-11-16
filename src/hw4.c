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

void send_exact_response(int socket, const char *msg) {
    char response[BUFFER_SIZE];
    sprintf(response, "%s\n", msg);
    write(socket, response, strlen(response));
}

void send_shot_response(int socket, int ships_remaining, char result) {
    char response[BUFFER_SIZE];
    sprintf(response, "R %d %c\n", ships_remaining, result);
    write(socket, response, strlen(response));
}

void send_halt(int socket, int is_winner) {
    char response[BUFFER_SIZE];
    sprintf(response, "H %d\n", is_winner);
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

int validate_ship_placement(GameState *game, Ship ship, int board[MAX_BOARD][MAX_BOARD]) {
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

int validate_init(GameState *game, char *packet, Ship *ships) {
    char *token = strtok(packet + 2, " ");
    for(int i = 0; i < MAX_SHIPS; i++) {
        if(!token) return 201;
        ships[i].type = atoi(token);
        if(ships[i].type < 1 || ships[i].type > 7) return 300;
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].rotation = atoi(token);
        if(ships[i].rotation < 0 || ships[i].rotation > 3) return 301;
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].row = atoi(token);
        
        token = strtok(NULL, " ");
        if(!token) return 201;
        ships[i].col = atoi(token);
        
        token = strtok(NULL, " ");
    }

    int board[MAX_BOARD][MAX_BOARD] = {0};
    for(int i = 0; i < MAX_SHIPS; i++) {
        int result = validate_ship_placement(game, ships[i], board);
        if(result != 0) return result;
    }
    return 0;
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

void process_shot(GameState *game, Player *shooter, Player *target, int row, int col) {
    if (shooter->shots[row][col]) {
        return;
    }
    shooter->shots[row][col] = 1;
    if (target->board[row][col]) {
        target->ships_remaining--;
        send_shot_response(shooter->socket, target->ships_remaining, 'H');
        if (target->ships_remaining == 0) {
            game->phase = 3;
            return;
        }
    } else {
        send_shot_response(shooter->socket, target->ships_remaining, 'M');
    }
    game->current_turn = (game->current_turn == 1) ? 2 : 1;
}

void build_query_response(GameState *game, Player *player, Player *opponent, char *response) {
    sprintf(response, "G %d", opponent->ships_remaining);
    for(int i = 0; i < game->height; i++) {
        for(int j = 0; j < game->width; j++) {
            if(player->shots[i][j]) {
                char temp[32];
                sprintf(temp, " %c %d %d", opponent->board[i][j] ? 'H' : 'M', i, j);
                strcat(response, temp);
            }
        }
    }
}

void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    if(packet[0] == 'F') {
        send_halt(current->socket, 0);
        game->phase = 3;
        return;
    }

    if(game->phase == 0 && packet[0] != 'B') {
        send_exact_response(current->socket, "E 100");
        return;
    }
    if(game->phase == 1 && packet[0] != 'I') {
        send_exact_response(current->socket, "E 101");
        return;
    }
    if(game->phase == 2 && packet[0] != 'S' && packet[0] != 'Q') {
        send_exact_response(current->socket, "E 102");
        return;
    }

    switch(packet[0]) {
        case 'B': {
            if(is_p1) {
                int w, h;
                if(sscanf(packet, "B %d %d", &w, &h) != 2 || w < 10 || h < 10) {
                    send_exact_response(current->socket, "E 200");
                    return;
                }
                game->width = w;
                game->height = h;
            }
            send_exact_response(current->socket, "A");
            current->ready = 1;
            if(game->p1.ready && game->p2.ready) {
                game->phase = 1;
            }
            break;
        }

        case 'I': {
            Ship ships[MAX_SHIPS];
            int error = validate_init(game, packet, ships);
            if(error) {
                char error_msg[16];
                sprintf(error_msg, "E %d", error);
                send_exact_response(current->socket, error_msg);
                return;
            }
            place_ships(game, current, ships);
            send_exact_response(current->socket, "A");
            current->ready = 2;
            if(game->p1.ready == 2 && game->p2.ready == 2) {
                game->phase = 2;
            }
            break;
        }

        case 'S': {
            if((is_p1 && game->current_turn != 1) || (!is_p1 && game->current_turn != 2)) {
                send_exact_response(current->socket, "E 103");
                return;
            }
            int row, col;
            if(sscanf(packet, "S %d %d", &row, &col) != 2) {
                send_exact_response(current->socket, "E 202");
                return;
            }
            if(row < 0 || row >= game->height || col < 0 || col >= game->width) {
                send_exact_response(current->socket, "E 400");
                return;
            }
            if(current->shots[row][col]) {
                send_exact_response(current->socket, "E 401");
                return;
            }
            process_shot(game, current, other, row, col);
            break;
        }

        case 'Q': {
            if((is_p1 && game->current_turn != 1) || (!is_p1 && game->current_turn != 2)) {
                send_exact_response(current->socket, "E 103");
                return;
            }
            char response[BUFFER_SIZE];
            build_query_response(game, current, other, response);
            send_exact_response(current->socket, response);
            break;
        }
    }
}

int setup_socket(int port) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

void handle_player_input(GameState *game, int is_p1) {
    char buffer[BUFFER_SIZE] = {0};
    Player *current = is_p1 ? &game->p1 : &game->p2;
    
    ssize_t bytes = read(current->socket, buffer, BUFFER_SIZE-1);
    if (bytes <= 0) return;
    
    buffer[bytes] = '\0';
    buffer[strcspn(buffer, "\n")] = '\0';
    
    process_packet(game, buffer, is_p1);
}

int main() {
    GameState game = {0};
    game.phase = 0;
    game.current_turn = 1;

    int server1_fd = setup_socket(PORT1);
    int server2_fd = setup_socket(PORT2);
    
    printf("Waiting for P1 connection on port %d\n", PORT1);
    game.p1.socket = accept(server1_fd, NULL, NULL);
    
    printf("Waiting for P2 connection on port %d\n", PORT2);
    game.p2.socket = accept(server2_fd, NULL, NULL);

    while(1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(game.p1.socket, &readfds);
        FD_SET(game.p2.socket, &readfds);
        
        int maxfd = (game.p1.socket > game.p2.socket) ? game.p1.socket : game.p2.socket;
        select(maxfd + 1, &readfds, NULL, NULL, NULL);

        if (FD_ISSET(game.p1.socket, &readfds)) {
            handle_player_input(&game, 1);
        }
        if (FD_ISSET(game.p2.socket, &readfds)) {
            handle_player_input(&game, 0);
        }

        if (game.phase == 3) {
            break;
        }
    }

    close(game.p1.socket);
    close(game.p2.socket);
    close(server1_fd);
    close(server2_fd);

    return 0;
}
