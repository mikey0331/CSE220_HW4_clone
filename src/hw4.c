#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define PORT1 2201
#define PORT2 2202
#define BUFFER_SIZE 1024
#define MAX_PIECES 5
#define BOARD_MAX_SIZE 20

#define ERR_EXPECT_BEGIN 100
#define ERR_EXPECT_INIT 101
#define ERR_EXPECT_GAME 102
#define ERR_BEGIN_PARAMS 200
#define ERR_INIT_PARAMS 201
#define ERR_SHOOT_PARAMS 202
#define ERR_INIT_SHAPE 300
#define ERR_INIT_ROTATION 301
#define ERR_INIT_FIT 302
#define ERR_INIT_OVERLAP 303
#define ERR_SHOOT_BOUNDS 400
#define ERR_SHOOT_REPEAT 401

enum GameState {
    WAITING_BEGIN,
    WAITING_INIT,
    IN_GAME
};

typedef struct {
    int cells[4][2];
} PieceShape;

typedef struct {
    int type;
    int rotation;
    int col;
    int row;
    int hits;
    int total_cells;
    int is_sunk;
} Piece;

typedef struct {
    int socket;
    int board[BOARD_MAX_SIZE][BOARD_MAX_SIZE];
    int shot_board[BOARD_MAX_SIZE][BOARD_MAX_SIZE];
    Piece pieces[MAX_PIECES];
    int num_pieces;
    int is_turn;
    int ships_remaining;
    enum GameState state;
} Player;

typedef struct {
    Player player1;
    Player player2;
    int board_width;
    int board_height;
    int game_active;
    pthread_mutex_t game_mutex;
} GameState;

const PieceShape PIECE_SHAPES[7][4] = {
    // I piece
    {{{0,0}, {0,1}, {0,2}, {0,3}}, {{0,0}, {1,0}, {2,0}, {3,0}},
     {{0,0}, {0,1}, {0,2}, {0,3}}, {{0,0}, {1,0}, {2,0}, {3,0}}},
    // J piece
    {{{0,0}, {0,1}, {0,2}, {-1,2}}, {{0,0}, {1,0}, {2,0}, {2,1}},
     {{0,0}, {1,0}, {0,-1}, {0,-2}}, {{0,0}, {-1,0}, {-2,0}, {-2,-1}}},
    // L piece
    {{{0,0}, {0,1}, {0,2}, {1,2}}, {{0,0}, {1,0}, {2,0}, {2,-1}},
     {{0,0}, {-1,0}, {0,-1}, {0,-2}}, {{0,0}, {-1,0}, {-2,0}, {-2,1}}},
    // O piece
    {{{0,0}, {0,1}, {1,0}, {1,1}}, {{0,0}, {0,1}, {1,0}, {1,1}},
     {{0,0}, {0,1}, {1,0}, {1,1}}, {{0,0}, {0,1}, {1,0}, {1,1}}},
    // S piece
    {{{0,0}, {0,1}, {1,1}, {1,2}}, {{0,0}, {1,0}, {1,-1}, {2,-1}},
     {{0,0}, {0,1}, {1,1}, {1,2}}, {{0,0}, {1,0}, {1,-1}, {2,-1}}},
    // T piece
    {{{0,0}, {0,1}, {0,2}, {1,1}}, {{0,0}, {1,0}, {2,0}, {1,1}},
     {{0,0}, {0,1}, {0,2}, {-1,1}}, {{0,0}, {1,0}, {2,0}, {1,-1}}},
    // Z piece
    {{{0,0}, {0,1}, {-1,1}, {-1,2}}, {{0,0}, {1,0}, {1,1}, {2,1}},
     {{0,0}, {0,1}, {-1,1}, {-1,2}}, {{0,0}, {1,0}, {1,1}, {2,1}}}
};

void init_game(GameState *game) {
    memset(game, 0, sizeof(GameState));
    game->player1.ships_remaining = MAX_PIECES;
    game->player2.ships_remaining = MAX_PIECES;
    game->player1.is_turn = 1;
    game->player1.state = WAITING_BEGIN;
    game->player2.state = WAITING_BEGIN;
    game->game_active = 1;
    pthread_mutex_init(&game->game_mutex, NULL);
}

void send_error(int socket, int error_code) {
    char response[BUFFER_SIZE];
    sprintf(response, "E %d", error_code);
    send(socket, response, strlen(response), 0);
}
int is_valid_placement(GameState *game, Player *player, Piece *piece) {
    if (piece->type < 0 || piece->type >= 7 || 
        piece->rotation < 0 || piece->rotation >= 4) {
        return 0;
    }

    const PieceShape *shape = &PIECE_SHAPES[piece->type][piece->rotation];
    for (int i = 0; i < 4; i++) {
        int new_row = piece->row + shape->cells[i][0];
        int new_col = piece->col + shape->cells[i][1];
        
        if (new_row < 0 || new_row >= game->board_height ||
            new_col < 0 || new_col >= game->board_width ||
            player->board[new_row][new_col] != 0) {
            return 0;
        }
    }
    return 1;
}

void place_piece(Player *player, Piece *piece) {
    const PieceShape *shape = &PIECE_SHAPES[piece->type][piece->rotation];
    piece->total_cells = 4;
    piece->hits = 0;
    piece->is_sunk = 0;
    
    for (int i = 0; i < 4; i++) {
        int new_row = piece->row + shape->cells[i][0];
        int new_col = piece->col + shape->cells[i][1];
        player->board[new_row][new_col] = 1;
    }
}

int handle_begin(GameState *game, Player *player, char *msg) {
    pthread_mutex_lock(&game->game_mutex);
    
    if (player->state != WAITING_BEGIN) {
        send_error(player->socket, ERR_EXPECT_BEGIN);
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }

    if (player == &game->player1) {
        int width, height;
        if (sscanf(msg, "B %d %d", &width, &height) != 2 || width < 10 || height < 10) {
            send_error(player->socket, ERR_BEGIN_PARAMS);
            pthread_mutex_unlock(&game->game_mutex);
            return 0;
        }
        game->board_width = width;
        game->board_height = height;
    } else {
        if (strlen(msg) > 2) {
            send_error(player->socket, ERR_BEGIN_PARAMS);
            pthread_mutex_unlock(&game->game_mutex);
            return 0;
        }
    }
    
    player->state = WAITING_INIT;
    send(player->socket, "A", 1, 0);
    pthread_mutex_unlock(&game->game_mutex);
    return 1;
}

int handle_initialize(GameState *game, Player *player, char *msg) {
    pthread_mutex_lock(&game->game_mutex);
    
    if (player->state != WAITING_INIT) {
        send_error(player->socket, ERR_EXPECT_INIT);
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }

    if (player->num_pieces >= MAX_PIECES) {
        send_error(player->socket, ERR_INIT_PARAMS);
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    Piece piece;
    if (sscanf(msg, "I %d %d %d %d", &piece.type, &piece.rotation, 
               &piece.col, &piece.row) != 4) {
        send_error(player->socket, ERR_INIT_PARAMS);
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    if (!is_valid_placement(game, player, &piece)) {
        send_error(player->socket, ERR_INIT_FIT);
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    place_piece(player, &piece);
    player->pieces[player->num_pieces++] = piece;
    
    if (player->num_pieces == MAX_PIECES) {
        player->state = IN_GAME;
    }
    
    send(player->socket, "A", 1, 0);
    pthread_mutex_unlock(&game->game_mutex);
    return 1;
}

void check_ship_sunk(Player *defender, int row, int col) {
    for (int i = 0; i < defender->num_pieces; i++) {
        const PieceShape *shape = &PIECE_SHAPES[defender->pieces[i].type][defender->pieces[i].rotation];
        for (int j = 0; j < 4; j++) {
            int ship_row = defender->pieces[i].row + shape->cells[j][0];
            int ship_col = defender->pieces[i].col + shape->cells[j][1];
            
            if (row == ship_row && col == ship_col) {
                defender->pieces[i].hits++;
                if (defender->pieces[i].hits == defender->pieces[i].total_cells) {
                    defender->pieces[i].is_sunk = 1;
                    defender->ships_remaining--;
                }
                return;
            }
        }
    }
}

int handle_shoot(GameState *game, Player *attacker, Player *defender, char *msg) {
    pthread_mutex_lock(&game->game_mutex);
    
    if (attacker->state != IN_GAME || !attacker->is_turn) {
        send_error(attacker->socket, ERR_EXPECT_GAME);
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    int row, col;
    if (sscanf(msg, "S %d %d", &row, &col) != 2) {
        send_error(attacker->socket, ERR_SHOOT_PARAMS);
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    if (row < 0 || row >= game->board_height || 
        col < 0 || col >= game->board_width) {
        send_error(attacker->socket, ERR_SHOOT_BOUNDS);
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    if (attacker->shot_board[row][col] != 0) {
        send_error(attacker->socket, ERR_SHOOT_REPEAT);
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    char response[BUFFER_SIZE];
    if (defender->board[row][col] == 1) {
        defender->board[row][col] = 2;
        attacker->shot_board[row][col] = 1;
        check_ship_sunk(defender, row, col);
        sprintf(response, "R %d H", defender->ships_remaining);
        send(attacker->socket, response, strlen(response), 0);
        
        if (defender->ships_remaining == 0) {
            send(defender->socket, "H 0", 3, 0);
            send(attacker->socket, "H 1", 3, 0);
            game->game_active = 0;
        }
    } else {
        attacker->shot_board[row][col] = 2;
        sprintf(response, "R %d M", defender->ships_remaining);
        send(attacker->socket, response, strlen(response), 0);
    }
    
    attacker->is_turn = 0;
    defender->is_turn = 1;
    
    pthread_mutex_unlock(&game->game_mutex);
    return 1;
}

void handle_query(GameState *game, Player *player) {
    if (player->state != IN_GAME) {
        send_error(player->socket, ERR_EXPECT_GAME);
        return;
    }

    char response[BUFFER_SIZE];
    int offset = 0;
    Player *opponent = (player == &game->player1) ? &game->player2 : &game->player1;
    
    offset += sprintf(response, "G %d ", opponent->ships_remaining);
    
    for (int i = 0; i < game->board_height; i++) {
        for (int j = 0; j < game->board_width; j++) {
            if (player->shot_board[i][j] > 0) {
                offset += sprintf(response + offset, "%c %d %d ", 
                    player->shot_board[i][j] == 1 ? 'H' : 'M', j, i);
            }
        }
    }
    
    send(player->socket, response, strlen(response), 0);
}

void handle_forfeit(GameState *game, Player *player) {
    pthread_mutex_lock(&game->game_mutex);
    game->game_active = 0;
    
    if (player == &game->player1) {
        send(game->player1.socket, "H 0", 3, 0);
        send(game->player2.socket, "H 1", 3, 0);
    } else {
        send(game->player1.socket, "H 1", 3, 0);
        send(game->player2.socket, "H 0", 3, 0);
    }
    
    pthread_mutex_unlock(&game->game_mutex);
}

void *handle_client(void *arg) {
    GameState *game = (GameState *)arg;
    Player *player = (game->player1.socket == 0) ? &game->player1 : &game->player2;
    Player *opponent = (player == &game->player1) ? &game->player2 : &game->player1;
    char buffer[BUFFER_SIZE];
    
    while (game->game_active) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = read(player->socket, buffer, BUFFER_SIZE - 1);
        
        if (bytes_read <= 0) break;
        
        switch(buffer[0]) {
            case 'B':
                handle_begin(game, player, buffer);
                break;
            case 'I':
                handle_initialize(game, player, buffer);
                break;
            case 'S':
                handle_shoot(game, player, opponent, buffer);
                break;
            case 'Q':
                handle_query(game, player);
                break;
            case 'F':
                handle_forfeit(game, player);
                break;
            default:
                send_error(player->socket, ERR_EXPECT_GAME);
                break;
        }
    }
    return NULL;
}

int main() {
    int server_fd1, server_fd2;
    struct sockaddr_in address1, address2;
    int opt = 1;
    int addrlen = sizeof(address1);
    pthread_t thread1, thread2;
    GameState game;
    
    init_game(&game);
    
    if ((server_fd1 = socket(AF_INET, SOCK_STREAM, 0)) == 0 ||
        (server_fd2 = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    setsockopt(server_fd1, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address1.sin_family = AF_INET;
    address1.sin_addr.s_addr = INADDR_ANY;
    address1.sin_port = htons(PORT1);
    
    address2.sin_family = AF_INET;
    address2.sin_addr.s_addr = INADDR_ANY;
    address2.sin_port = htons(PORT2);
    
    if (bind(server_fd1, (struct sockaddr *)&address1, sizeof(address1)) < 0 ||
        bind(server_fd2, (struct sockaddr *)&address2, sizeof(address2)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd1, 1) < 0 || listen(server_fd2, 1) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    game.player1.socket = accept(server_fd1, (struct sockaddr *)&address1, 
                               (socklen_t*)&addrlen);
    game.player2.socket = accept(server_fd2, (struct sockaddr *)&address2, 
                               (socklen_t*)&addrlen);
    
    if (game.player1.socket < 0 || game.player2.socket < 0) {
        perror("Accept failed");
        exit(EXIT_FAILURE);
    }
    
    pthread_create(&thread1, NULL, handle_client, &game);
    pthread_create(&thread2, NULL, handle_client, &game);
    
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    
    close(server_fd1);
    close(server_fd2);
    close(game.player1.socket);
    close(game.player2.socket);
    pthread_mutex_destroy(&game.game_mutex);
    
    return 0;
}
