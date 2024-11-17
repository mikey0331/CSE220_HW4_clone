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
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}}, // I
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, // O
    {{0, 1}, {1, 0}, {1, 1}, {1, 2}}, // T
    {{0, 0}, {1, 0}, {2, 0}, {2, 1}}, // J
    {{0, 0}, {1, 0}, {2, 0}, {2, -1}}, // L
    {{0, 0}, {0, 1}, {1, -1}, {1, 0}}, // S
    {{0, -1}, {0, 0}, {1, 0}, {1, 1}} // Z
};

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

// Build query response for consistency
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

// Updated helper function: rotate_point (unchanged logic)
void rotate_point(int *row, int *col, int rotation) {
    int temp;
    for (int i = 0; i < rotation; i++) {
        temp = *row;
        *row = -*col;
        *col = temp;
    }
}

// Helper for consistent response formatting
void send_packet(int socket, const char *format, ...) {
    char response[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(response, BUFFER_SIZE, format, args);
    va_end(args);
    write(socket, response, strlen(response));
}

// Updated process_packet to align responses
void process_packet(GameState *game, char *packet, int is_p1) {
    Player *current = is_p1 ? &game->p1 : &game->p2;
    Player *other = is_p1 ? &game->p2 : &game->p1;

    if (packet[0] == 'F') {
        send_halt(current->socket, 0);
        send_halt(other->socket, 1);
        game->phase = 3; // Game over
        return;
    }

    switch (game->phase) {
        case 0: // Setup phase
            if (packet[0] != 'B') {
                send_error(current->socket, is_p1 ? 200 : 100);
                return;
            }
            // Parse and validate board dimensions
            int width, height;
            if (is_p1) {
                if (sscanf(packet + 1, "%d %d", &width, &height) != 2 || width < 10 || height < 10) {
                    send_error(current->socket, 200);
                    return;
                }
                game->width = width;
                game->height = height;
            }
            send_ack(current->socket);
            current->ready = 1;
            if (game->p1.ready && game->p2.ready) game->phase = 1; // Transition to ship placement
            break;

        case 1: // Ship placement phase
            if (packet[0] != 'I') {
                send_error(current->socket, 101);
                return;
            }
            if (!validate_ship_placement(game, packet + 1, current)) {
                send_error(current->socket, 302);
                return;
            }
            send_ack(current->socket);
            current->ready = 2;
            if (game->p1.ready == 2 && game->p2.ready == 2) game->phase = 2; // Transition to gameplay
            break;

        case 2: // Gameplay phase
            if (packet[0] == 'Q') {
                char response[BUFFER_SIZE];
                build_query_response(game, current, other, response);
                write(current->socket, response, strlen(response));
                return;
            } else if (packet[0] == 'S') {
                int col, row;
                if (sscanf(packet + 1, "%d %d", &col, &row) != 2 ||
                    col < 0 || col >= game->width || row < 0 || row >= game->height) {
                    send_error(current->socket, 304);
                    return;
                }
                if (current->shots[row][col]) {
                    send_error(current->socket, 305);
                    return;
                }
                // Process shot
                current->shots[row][col] = 1;
                if (other->board[row][col]) {
                    other->ships_remaining--;
                    send_shot_response(current->socket, other->ships_remaining, 'H');
                    if (other->ships_remaining == 0) game->phase = 4; // Game won
                } else {
                    send_shot_response(current->socket, other->ships_remaining, 'M');
                }
            } else {
                send_error(current->socket, 304); // Invalid action
            }
            break;

        default:
            send_error(current->socket, 400); // Unexpected phase
    }
}

// Validate ship placement logic
int validate_ship_placement(GameState *game, char *packet, Player *player) {
    int temp_board[MAX_BOARD][MAX_BOARD] = {0};
    char *tokens[MAX_SHIPS * 4];
    int num_tokens = parse_tokens(packet, tokens);

    if (num_tokens != MAX_SHIPS * 4) return 0; // Incorrect number of parameters

    for (int i = 0; i < MAX_SHIPS; i++) {
        int type = atoi(tokens[i * 4]);
        int rotation = atoi(tokens[i * 4 + 1]);
        int col = atoi(tokens[i * 4 + 2]);
        int row = atoi(tokens[i * 4 + 3]);

        if (!is_valid_ship_position(game, temp_board, type, rotation, row, col)) {
            free_tokens(tokens, num_tokens);
            return 0;
        }
    }
    memcpy(player->board, temp_board, sizeof(temp_board));
    player->ships_remaining = MAX_SHIPS * 4;
    free_tokens(tokens, num_tokens);
    return 1;
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
