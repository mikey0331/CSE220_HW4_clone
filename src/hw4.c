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
    int ready; // 0: Not Ready, 1: Board Sent, 2: Ships Sent
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
    int phase; // 0: Setup, 1: Ship Placement, 2: Battle, 3: Game Over
    int current_turn; // 1: P1's turn, 2: P2's turn
} GameState;

// Constants for Tetris Pieces (shared between players)
const int TETRIS_PIECES[7][4][2] = {
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}}, // I
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, // O
    {{0, 1}, {1, 0}, {1, 1}, {1, 2}}, // T
    {{0, 0}, {1, 0}, {2, 0}, {2, 1}}, // J
    {{0, 0}, {1, 0}, {2, 0}, {2, -1}}, // L
    {{0, 0}, {0, 1}, {1, -1}, {1, 0}}, // S
    {{0, -1}, {0, 0}, {1, 0}, {1, 1}} // Z
};

// Utility Functions for Sending Responses
void send_error(int socket, int code) {
    char response[16];
    sprintf(response, "E %d", code); // Added a space after "E"
    write(socket, response, strlen(response));
}

void send_ack(int socket) {
    write(socket, "A", 1);
}

void send_halt(int socket, int is_winner) {
    char response[16];
    sprintf(response, "H %d", is_winner); // Added a space after "H"
    write(socket, response, strlen(response));
}

void send_shot_response(int socket, int ships_remaining, char result) {
    char response[32];
    sprintf(response, "R %d %c", ships_remaining, result); // Added spaces between components
    write(socket, response, strlen(response));
}

void build_query_response(GameState *game, Player *player, Player *opponent, char *response) {
    sprintf(response, "G %d", opponent->ships_remaining); // Added space after "G"
    for (int i = 0; i < game->height; i++) {
        for (int j = 0; j < game->width; j++) {
            if (player->shots[i][j]) {
                char temp[32];
                sprintf(temp, " %c %d %d", opponent->board[i][j] ? 'H' : 'M', i, j); // Spaces between elements
                strcat(response, temp);
            }
        }
    }
}

// Utility to Rotate Points (Used for Ship Placement)
void rotate_point(int *row, int *col, int rotation) {
    int temp;
    for (int i = 0; i < rotation; i++) {
        temp = *row;
        *row = -*col;
        *col = temp;
    }
}

// GameState Methods
void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    if (packet[0] == 'F') {
        send_halt(current->socket, 0);
        send_halt(other->socket, 1);
        game->phase = 3; // End game
        return;
    }

    // Phase 0: Board Setup
    if (game->phase == 0) {
        if (packet[0] != 'B') {
            send_error(current->socket, is_p1 ? 200 : 100);
            return;
        }

        if (is_p1) {
            int w = 0, h = 0;
            if (sscanf(packet + 1, "%d %d", &w, &h) != 2 || w < 10 || h < 10) {
                send_error(current->socket, 200);
                return;
            }
            game->width = w;
            game->height = h;
        } else if (strlen(packet) > 1) {
            send_error(current->socket, 100);
            return;
        }

        send_ack(current->socket);
        current->ready = 1;
        if (game->p1.ready && game->p2.ready) {
            game->phase = 1;
        }
        return;
    }

    // Phase 1: Ship Placement
    if (game->phase == 1) {
        if (packet[0] != 'I') {
            send_error(current->socket, 101);
            return;
        }

        // Parse Ship Placement
        char *temp_packet = strdup(packet);
        char *token = strtok(temp_packet + 1, " ");
        int param_count = 0;
        char *tokens[MAX_SHIPS * 4];
        while (token && param_count < MAX_SHIPS * 4) {
            tokens[param_count++] = strdup(token);
            token = strtok(NULL, " ");
        }
        free(temp_packet);

        if (param_count != MAX_SHIPS * 4) {
            send_error(current->socket, 301);
            return;
        }

        // Validate Ship Parameters
        int temp_board[MAX_BOARD][MAX_BOARD] = {0};
        for (int i = 0; i < MAX_SHIPS; i++) {
            int type = atoi(tokens[i * 4]);
            int rotation = atoi(tokens[i * 4 + 1]);
            int col = atoi(tokens[i * 4 + 2]);
            int row = atoi(tokens[i * 4 + 3]);

            if (type < 1 || type > 7) {
                send_error(current->socket, 300);
                return;
            }
            if (rotation < 0 || rotation > 3) {
                send_error(current->socket, 301);
                return;
            }

            // Check Ship Bounds
            int piece_idx = type - 1;
            for (int j = 0; j < 4; j++) {
                int new_row = TETRIS_PIECES[piece_idx][j][0];
                int new_col = TETRIS_PIECES[piece_idx][j][1];

                rotate_point(&new_row, &new_col, rotation);
                new_row += row;
                new_col += col;

                if (new_row < 0 || new_row >= game->height || 
                    new_col < 0 || new_col >= game->width || 
                    temp_board[new_row][new_col]) {
                    send_error(current->socket, 302);
                    return;
                }
                temp_board[new_row][new_col] = 1;
            }
        }

        // Update Player State
        memcpy(current->board, temp_board, sizeof(temp_board));
        current->ships_remaining = MAX_SHIPS * 4;
        send_ack(current->socket);
        current->ready = 2;
        if (game->p1.ready == 2 && game->p2.ready == 2) {
            game->phase = 2;
        }
        return;
    }
if (game->phase == 2) {
        if (packet[0] == 'S') { // Shoot Command
            int col = 0, row = 0;
            if (sscanf(packet + 1, "%d %d", &col, &row) != 2 ||
                col < 0 || col >= game->width || row < 0 || row >= game->height) {
                send_error(current->socket, 201); // Invalid coordinates
                return;
            }

            if (current->shots[row][col]) {
                send_error(current->socket, 202); // Already shot
                return;
            }

            current->shots[row][col] = 1; // Mark the shot
            char result = 'M'; // Default result is Miss

            if (other->board[row][col]) { // Hit
                result = 'H';
                other->board[row][col] = 0; // Mark the hit position
                other->ships_remaining--;

                // Check if all ships are destroyed
                if (other->ships_remaining == 0) {
                    send_halt(current->socket, 1); // Current player wins
                    send_halt(other->socket, 0);  // Other player loses
                    game->phase = 3;             // Game Over
                    return;
                }
            }

            send_shot_response(current->socket, other->ships_remaining, result);

            // Update the turn
            game->current_turn = (game->current_turn == 1) ? 2 : 1;
            return;
        }

        if (packet[0] == 'Q') { // Query Game State
            char response[BUFFER_SIZE] = {0};
            build_query_response(game, current, other, response);
            write(current->socket, response, strlen(response));
            return;
        }

        send_error(current->socket, 203); // Invalid Command in Battle Phase
        return;
    }

    // Invalid Packet for Current Phase
    send_error(current->socket, 204);
}int main() {
    GameState game = {0};

    // Create Sockets for Players
    int server_fd1 = socket(AF_INET, SOCK_STREAM, 0);
    int server_fd2 = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd1 < 0 || server_fd2 < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address1 = {0};
    struct sockaddr_in address2 = {0};

    address1.sin_family = AF_INET;
    address1.sin_addr.s_addr = INADDR_ANY;
    address1.sin_port = htons(PORT1);

    address2.sin_family = AF_INET;
    address2.sin_addr.s_addr = INADDR_ANY;
    address2.sin_port = htons(PORT2);

    // Bind Sockets
    if (bind(server_fd1, (struct sockaddr *)&address1, sizeof(address1)) < 0 ||
        bind(server_fd2, (struct sockaddr *)&address2, sizeof(address2)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for Players
    if (listen(server_fd1, 1) < 0 || listen(server_fd2, 1) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Waiting for players...\n");

    // Accept Player Connections
    int addrlen1 = sizeof(address1);
    int addrlen2 = sizeof(address2);

    game.p1.socket = accept(server_fd1, (struct sockaddr *)&address1, (socklen_t *)&addrlen1);
    game.p2.socket = accept(server_fd2, (struct sockaddr *)&address2, (socklen_t *)&addrlen2);

    if (game.p1.socket < 0 || game.p2.socket < 0) {
        perror("Accept failed");
        exit(EXIT_FAILURE);
    }

    printf("Both players connected. Starting game...\n");

    game.phase = 0;       // Start with setup phase
    game.current_turn = 1; // Player 1 starts

    char buffer[BUFFER_SIZE] = {0};

    while (game.phase != 3) { // While game is not over
        Player *current = (game.current_turn == 1) ? &game.p1 : &game.p2;
        int bytes_read = read(current->socket, buffer, BUFFER_SIZE - 1);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            process_packet(&game, buffer, game.current_turn == 1);
        } else if (bytes_read == 0) {
            printf("Player %d disconnected. Ending game.\n", game.current_turn);
            break;
        } else if (errno != EINTR) {
            perror("Read error");
            break;
        }
    }

    // Cleanup
    close(game.p1.socket);
    close(game.p2.socket);
    close(server_fd1);
    close(server_fd2);

    return 0;
}
