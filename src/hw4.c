#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <pthread.h>

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
        if (strlen(data) > 2) { // More than just "B\n"
            send_response(socket, "E 200");
            return false;
        }
    }

    send_response(socket, "A");
    return true;
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