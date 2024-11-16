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
    pthread_mutex_init(&game->game_mutex, NULL);
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
    
    if (player == &game->player1 && game->board_width == 0) {
        int width, height;
        if (sscanf(msg, "B %d %d", &width, &height) == 2) {
            if (width > 0 && width <= BOARD_MAX_SIZE && 
                height > 0 && height <= BOARD_MAX_SIZE) {
                game->board_width = width;
                game->board_height = height;
                pthread_mutex_unlock(&game->game_mutex);
                return 1;
            }
        }
    } else if (player == &game->player2 && game->board_width > 0) {
        pthread_mutex_unlock(&game->game_mutex);
        return 1;
    }
    
    pthread_mutex_unlock(&game->game_mutex);
    return 0;
}

int handle_initialize(GameState *game, Player *player, char *msg) {
    pthread_mutex_lock(&game->game_mutex);
    
    if (player->num_pieces >= MAX_PIECES) {
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    Piece piece;
    if (sscanf(msg, "I %d %d %d %d", &piece.type, &piece.rotation, 
               &piece.col, &piece.row) != 4) {
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    if (!is_valid_placement(game, player, &piece)) {
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    place_piece(player, &piece);
    player->pieces[player->num_pieces++] = piece;
    
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
    
    if (!attacker->is_turn) {
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    int row, col;
    if (sscanf(msg, "S %d %d", &row, &col) != 2) {
        pthread_mutex_unlock(&game->game_mutex);
        return -1;
    }
    
    if (row < 0 || row >= game->board_height || 
        col < 0 || col >= game->board_width) {
        pthread_mutex_unlock(&game->game_mutex);
        return 0;
    }
    
    if (defender->board[row][col] == 1) {
        defender->board[row][col] = 2;
        attacker->shot_board[row][col] = 1;
        check_ship_sunk(defender, row, col);
        
        attacker->is_turn = 0;
        defender->is_turn = 1;
        
        pthread_mutex_unlock(&game->game_mutex);
        return 1;
    }
    
    attacker->shot_board[row][col] = 2;
    attacker->is_turn = 0;
    defender->is_turn = 1;
    
    pthread_mutex_unlock(&game->game_mutex);
    return 0;
}

void handle_query(GameState *game, Player *player) {
    char response[BUFFER_SIZE];
    int offset = 0;
    
    offset += sprintf(response + offset, "Q %d ", player == &game->player1 ? 
                     game->player2.ships_remaining : game->player1.ships_remaining);
    
    for (int i = 0; i < game->board_height; i++) {
        for (int j = 0; j < game->board_width; j++) {
            if (player->shot_board[i][j] > 0) {
                offset += sprintf(response + offset, "%d %d %d ", 
                                i, j, player->shot_board[i][j]);
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
                if (handle_begin(game, player, buffer)) {
                    send(player->socket, "A", 1, 0);
                } else {
                    send(player->socket, "E", 1, 0);
                }
                break;
                
            case 'I':
                if (handle_initialize(game, player, buffer)) {
                    send(player->socket, "A", 1, 0);
                } else {
                    send(player->socket, "E", 1, 0);
                }
                break;
                
            case 'S':
                if (player->is_turn) {
                    int result = handle_shoot(game, player, opponent, buffer);
                    if (result == 1) {
                        if (opponent->ships_remaining == 0) {
                            send(player->socket, "H 1", 3, 0);
                            send(opponent->socket, "H 0", 3, 0);
                            game->game_active = 0;
                        } else {
                            send(player->socket, "H", 1, 0);
                        }
                    } else if (result == 0) {
                        send(player->socket, "M", 1, 0);
                    } else {
                        send(player->socket, "E", 1, 0);
                    }
                } else {
                    send(player->socket, "E", 1, 0);
                }
                break;
                
            case 'Q':
                handle_query(game, player);
                break;
                
            case 'F':
                handle_forfeit(game, player);
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
    
    if ((server_fd1 = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket 1 creation failed");
        exit(EXIT_FAILURE);
    }
    
    if ((server_fd2 = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket 2 creation failed");
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
    
    if (bind(server_fd1, (struct sockaddr *)&address1, sizeof(address1)) < 0) {
        perror("Bind 1 failed");
        exit(EXIT_FAILURE);
    }
    
    if (bind(server_fd2, (struct sockaddr *)&address2, sizeof(address2)) < 0) {
        perror("Bind 2 failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd1, 1) < 0) {
        perror("Listen 1 failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd2, 1) < 0) {
        perror("Listen 2 failed");
        exit(EXIT_FAILURE);
    }
    
    game.game_active = 1;
    
    game.player1.socket = accept(server_fd1, (struct sockaddr *)&address1, 
                               (socklen_t*)&addrlen);
    if (game.player1.socket < 0) {
        perror("Accept 1 failed");
        exit(EXIT_FAILURE);
    }
    
    game.player2.socket = accept(server_fd2, (struct sockaddr *)&address2, 
                               (socklen_t*)&addrlen);
    if (game.player2.socket < 0) {
        perror("Accept 2 failed");
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


