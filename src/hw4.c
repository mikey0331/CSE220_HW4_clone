#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <pthread.h>
#include "tetris_pieces.h"

#define PLAYER1_PORT 2201
#define PLAYER2_PORT 2202
#define MIN_BOARD_SIZE 10
#define MAX_SHIPS 5
#define BUFFER_SIZE 1024
#define MAX_BOARD_SIZE 50

// Game board structure
typedef struct {
    int width;
    int height;
    bool **ships;  // 2D array for ship positions
    bool **hits;   // 2D array for hit positions
} GameBoard;

// Game state structure
typedef struct {
    GameBoard *player1_board;
    GameBoard *player2_board;
    int current_player;
    bool game_over;
    int player1_socket;
    int player2_socket;
    pthread_mutex_t game_mutex;
    int current_turn; // New field to track current turn
} GameState;

// Client handler arguments structure
typedef struct {
    int socket;
    int player_num;
    GameState *game;
} ClientArgs;

// Function prototypes
GameBoard* create_board(int width, int height);
void free_board(GameBoard *board);
bool is_valid_shot(GameBoard *board, int row, int col);
bool process_shot(GameBoard *board, int row, int col);
int count_remaining_ships(GameBoard *board);
void send_response(int socket, const char *response);
void *handle_client(void *args);
bool handle_begin_packet(GameState *game, char *data, int player, int socket);
bool handle_initialize_packet(GameState *game, char *data, int player, int socket);
void handle_shoot_packet(GameState *game, char *data, int player, int socket);
void handle_query_packet(GameState *game, int player, int socket);
void handle_forfeit_packet(GameState *game, int player, int socket);

// Create a new game board
GameBoard* create_board(int width, int height) {
    GameBoard *board = (GameBoard*)malloc(sizeof(GameBoard));
    if (!board) return NULL;

    board->width = width;
    board->height = height;

    // Allocate ships array
    board->ships = (bool**)malloc(height * sizeof(bool*));
    board->hits = (bool**)malloc(height * sizeof(bool*));
    if (!board->ships || !board->hits) {
        free(board);
        return NULL;
    }

    for (int i = 0; i < height; i++) {
        board->ships[i] = (bool*)calloc(width, sizeof(bool));
        board->hits[i] = (bool*)calloc(width, sizeof(bool));
        if (!board->ships[i] || !board->hits[i]) {
            free_board(board);
            return NULL;
        }
    }

    return board;
}

// Free a game board
void free_board(GameBoard *board) {
    if (!board) return;

    if (board->ships) {
        for (int i = 0; i < board->height; i++) {
            free(board->ships[i]);
        }
        free(board->ships);
    }

    if (board->hits) {
        for (int i = 0; i < board->height; i++) {
            free(board->hits[i]);
        }
        free(board->hits);
    }

    free(board);
}

// Send response to client
void send_response(int socket, const char *response) {
    send(socket, response, strlen(response), 0);
}

// Handle Begin packet
bool handle_begin_packet(GameState *game, char *data, int player, int socket) {
    int width, height;
    
    if (player == 1) {
        if (sscanf(data, "B %d %d", &width, &height) != 2) {
            send_response(socket, "E 200");
            return false;
        }
        
        if (width < MIN_BOARD_SIZE || height < MIN_BOARD_SIZE || 
            width > MAX_BOARD_SIZE || height > MAX_BOARD_SIZE) {
            send_response(socket, "E 200");
            return false;
        }

        pthread_mutex_lock(&game->game_mutex);
        game->player1_board = create_board(width, height);
        game->player2_board = create_board(width, height);
        pthread_mutex_unlock(&game->game_mutex);

        if (!game->player1_board || !game->player2_board) {
            send_response(socket, "E 200");
            return false;
        }
    } else {
        // For player 2, just send "B" with no parameters
        if (strlen(data) > 1) { // Just "B" is allowed
            send_response(socket, "E 100");
            return false;
        }
    }

    send_response(socket, "A");
    return true;
}

// Client handler function
void *handle_client(void *args) {
    ClientArgs *client_args = (ClientArgs *)args;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while (!client_args->game->game_over) {
        bytes_received = recv(client_args->socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            break;
        }

        buffer[bytes_received] = '\0';
        printf("Received from Player %d: %s\n", client_args->player_num, buffer);

        // Handle different packet types
        switch (buffer[0]) {
            case 'B':
                handle_begin_packet(client_args->game, buffer, client_args->player_num, client_args->socket);
                break;
            case 'I':
                handle_initialize_packet(client_args->game, buffer, client_args->player_num, client_args->socket);
                break;
            case 'S':
                if(client_args->game->current_player == client_args->player_num) {
                    handle_shoot_packet(client_args->game, buffer, client_args->player_num, client_args->socket);
                } else {
                    send_response(client_args->socket, "E 102");
                }
                break;
            case 'Q':
                handle_query_packet(client_args->game, client_args->player_num, client_args->socket);
                break;
            case 'F':
                handle_forfeit_packet(client_args->game, client_args->player_num, client_args->socket);
                break;
            default:
                send_response(client_args->socket, "E 100");
        }
    }

    return NULL;
}

// Handle Initialize packet
bool handle_initialize_packet(GameState *game, char *data, int player, int socket) {
    char *token;
    int piece_type, rotation, col, row;
    TetrisPiece pieces[MAX_SHIPS];
    int piece_count = 0;
    GameBoard *board = (player == 1) ? game->player1_board : game->player2_board;
    
    // Skip the 'I' character
    token = strtok(data + 1, " ");

    while (token != NULL && piece_count < MAX_SHIPS) {
        // Read piece parameters
        piece_type = atoi(token);
        token = strtok(NULL, " ");
        if (!token) break;
        rotation = atoi(token);
        token = strtok(NULL, " ");
        if (!token) break;
        col = atoi(token);
        token = strtok(NULL, " ");
        if (!token) break;
        row = atoi(token);
        token = strtok(NULL, " ");

        // Validate piece parameters
        if (!is_valid_piece_type(piece_type)) {
            send_response(socket, "E 300");
            return false;
        }
        if (!is_valid_rotation(rotation)) {
            send_response(socket, "E 301");
            return false;
        }

        // Create piece
        pieces[piece_count].piece_type = piece_type;
        pieces[piece_count].rotation = rotation;
        pieces[piece_count].anchor.col = col;
        pieces[piece_count].anchor.row = row;
        
        // Get piece cells
        if (!get_piece_cells(piece_type, rotation, pieces[piece_count].cells)) {
            send_response(socket, "E 300");
            return false;
        }

        // Check if piece fits on board
        if (!is_valid_placement(&pieces[piece_count], board->width, board->height)) {
            send_response(socket, "E 302");
            return false;
        }

        // Check for overlap with previous pieces
        for (int i = 0; i < piece_count; i++) {
            if (pieces_overlap(&pieces[i], &pieces[piece_count])) {
                send_response(socket, "E 303");
                return false;
            }
        }

        piece_count++;
    }

    if (piece_count != MAX_SHIPS) {
        send_response(socket, "E 201");
        return false;
    }

    // Place pieces on board
    pthread_mutex_lock(&game->game_mutex);
    for (int i = 0; i < MAX_SHIPS; i++) {
        Point abs_coords[PIECE_SIZE];
        get_absolute_coordinates(&pieces[i], abs_coords);
        for (int j = 0; j < PIECE_SIZE; j++) {
            board->ships[abs_coords[j].row][abs_coords[j].col] = true;
        }
    }
    pthread_mutex_unlock(&game->game_mutex);

    send_response(socket, "A");
    return true;
}

// Handle Shoot packet
void handle_shoot_packet(GameState *game, char *data, int player, int socket) {
    int row, col;
    if (sscanf(data, "S %d %d", &row, &col) != 2) {
        send_response(socket, "E 202");
        return;
    }

    GameBoard *target_board = (player == 1) ? game->player2_board : game->player1_board;
    
    // Check if shot is within board boundaries
    if (row < 0 || row >= target_board->height || col < 0 || col >= target_board->width) {
        send_response(socket, "E 400");
        return;
    }

    // Check if cell was already shot
    if (target_board->hits[row][col]) {
        send_response(socket, "E 401");
        return;
    }

    pthread_mutex_lock(&game->game_mutex);
    
    // Mark the hit
    target_board->hits[row][col] = true;
    bool is_hit = target_board->ships[row][col];
    
    // Count remaining ships
    int ships_remaining = 0;
    for (int i = 0; i < target_board->height; i++) {
        for (int j = 0; j < target_board->width; j++) {
            if (target_board->ships[i][j] && !target_board->hits[i][j]) {
                ships_remaining++;
            }
        }
    }

    // Switch current player
    game->current_player = (game->current_player == 1) ? 2 : 1;

    pthread_mutex_unlock(&game->game_mutex);

    // Send response
    char response[32];
    snprintf(response, sizeof(response), "R %d %c", ships_remaining, is_hit ? 'H' : 'M');
    send_response(socket, response);

    // If no ships remaining, send halt messages
    if (ships_remaining == 0) {
        game->game_over = true;
        send_response(socket, "H 1");  // Winner
        send_response(player == 1 ? game->player2_socket : game->player1_socket, "H 0");  // Loser
    }
}

// Handle Query packet
void handle_query_packet(GameState *game, int player, int socket) {
    if (player != game->current_player) {
        send_response(socket, "E 102");
        return;
    }

    GameBoard *target_board = (player == 1) ? game->player2_board : game->player1_board;
    
    // Count remaining ships
    int ships_remaining = 0;
    for (int i = 0; i < target_board->height; i++) {
        for (int j = 0; j < target_board->width; j++) {
            if (target_board->ships[i][j] && !target_board->hits[i][j]) {
                ships_remaining++;
            }
        }
    }

    // Build response string
    char response[BUFFER_SIZE];
    int offset = snprintf(response, sizeof(response), "G %d", ships_remaining);

    // Add all hits and misses
    for (int row = 0; row < target_board->height; row++) {
        for (int col = 0; col < target_board->width; col++) {
            if (target_board->hits[row][col]) {
                offset += snprintf(response + offset, sizeof(response) - offset,
                                 " %c %d %d",
                                 target_board->ships[row][col] ? 'H' : 'M',
                                 row, col);
            }
        }
    }

    send_response(socket, response);
}

// Handle Forfeit packet
void handle_forfeit_packet(GameState *game, int player, int socket) {
    if (player != game->current_player) {
        send_response(socket, "E 102");
        return;
    }

    game->game_over = true;
    send_response(socket, "H 0");  // Forfeiting player loses
    send_response(player == 1 ? game->player2_socket : game->player1_socket, "H 1");  // Other player wins
}

// Main function
int main() {
    int server_socket1, server_socket2;
    struct sockaddr_in address1, address2;
    int opt = 1;
    int addrlen = sizeof(address1);
    GameState game = {0};
    pthread_t thread1, thread2;

    // Initialize game state
    game.current_player = 1;
    game.game_over = false;
    game.current_turn = 0; // Initialize current turn
    pthread_mutex_init(&game.game_mutex, NULL);

    // Create socket file descriptors
    if ((server_socket1 = socket(AF_INET, SOCK_STREAM, 0)) == 0 ||
        (server_socket2 = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options
    if (setsockopt(server_socket1, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ||
        setsockopt(server_socket2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Configure addresses
    address1.sin_family = AF_INET;
    address1.sin_addr.s_addr = INADDR_ANY;
    address1.sin_port = htons(PLAYER1_PORT);

    address2.sin_family = AF_INET;
    address2.sin_addr.s_addr = INADDR_ANY;
    address2.sin_port = htons(PLAYER2_PORT);

    // Bind sockets
    if (bind(server_socket1, (struct sockaddr *)&address1, sizeof(address1)) < 0 ||
        bind(server_socket2, (struct sockaddr *)&address2, sizeof(address2))) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_socket1, 1) < 0 || listen(server_socket2, 1) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on ports %d and %d...\n", PLAYER1_PORT, PLAYER2_PORT);

    // Accept connections and create threads
    game.player1_socket = accept(server_socket1, (struct sockaddr *)&address1, (socklen_t*)&addrlen);
    if (game.player1_socket < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    game.player2_socket = accept(server_socket2, (struct sockaddr *)&address2, (socklen_t*)&addrlen);
    if (game.player2_socket < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    // Create thread arguments
    ClientArgs *args1 = malloc(sizeof(ClientArgs));
    ClientArgs *args2 = malloc(sizeof(ClientArgs));
    
    args1->socket = game.player1_socket;
    args1->player_num = 1;
    args1->game = &game;

    args2->socket = game.player2_socket;
    args2->player_num = 2;
    args2->game = &game;

    // Create threads
    pthread_create(&thread1, NULL, handle_client, (void*)args1);
    pthread_create(&thread2, NULL, handle_client, (void*)args2);

    // Wait for threads to finish
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    // Cleanup
    close(server_socket1);
    close(server_socket2);
    pthread_mutex_destroy(&game.game_mutex);
    free(args1);
    free(args2);
    free_board(game.player1_board);
    free_board(game.player2_board);

    return 0;
}
