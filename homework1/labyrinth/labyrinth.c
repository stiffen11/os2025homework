#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>
#include "testkit.h"
#include "labyrinth.h"

#define VERSION "Labyrinth Game"

// 全局变量记录命令行参数
static char *map_file = NULL;
static char player_id = -1;
static char *direction = NULL;
static bool version_flag = false;

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    return playerId >= '0' && playerId <= '9';
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open map file '%s'\n", filename);
        return false;
    }

    char line[MAX_COLS + 2]; // +2 for newline and null terminator
    labyrinth->rows = 0;
    
    while (fgets(line, sizeof(line), file) && labyrinth->rows < MAX_ROWS) {
        size_t len = strlen(line);
        // Remove newline character
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        
        if (len == 0) continue; // Skip empty lines
        
        // Set columns based on first non-empty line
        if (labyrinth->rows == 0) {
            labyrinth->cols = len;
        } else if (len != labyrinth->cols) {
            fprintf(stderr, "Error: Inconsistent line length at row %d\n", labyrinth->rows);
            fclose(file);
            return false;
        }
        
        if (len > MAX_COLS) {
            fprintf(stderr, "Error: Map too wide (max %d columns)\n", MAX_COLS);
            fclose(file);
            return false;
        }
        
        // Copy line to map
        for (int col = 0; col < labyrinth->cols; col++) {
            char ch = line[col];
            if (ch == '#' || ch == '.' || (ch >= '0' && ch <= '9')) {
                labyrinth->map[labyrinth->rows][col] = ch;
            } else {
                fprintf(stderr, "Error: Invalid character '%c' at (%d,%d)\n", 
                        ch, labyrinth->rows, col);
                fclose(file);
                return false;
            }
        }
        
        labyrinth->rows++;
        if (labyrinth->rows > MAX_ROWS) {
            fprintf(stderr, "Error: Map too tall (max %d rows)\n", MAX_ROWS);
            fclose(file);
            return false;
        }
    }
    
    fclose(file);
    
    if (labyrinth->rows == 0) {
        fprintf(stderr, "Error: Empty map file\n");
        return false;
    }
    
    return true;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    Position pos = {-1, -1};
    
    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            if (labyrinth->map[row][col] == playerId) {
                pos.row = row;
                pos.col = col;
                return pos;
            }
        }
    }
    
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    Position pos = {-1, -1};
    
    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            if (labyrinth->map[row][col] == '.') {
                pos.row = row;
                pos.col = col;
                return pos;
            }
        }
    }
    
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return false;
    }
    
    char ch = labyrinth->map[row][col];
    return ch == '.';
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    Position current = findPlayer(labyrinth, playerId);
    
    // If player not found, place at first empty space
    if (current.row == -1) {
        current = findFirstEmptySpace(labyrinth);
        if (current.row == -1) {
            fprintf(stderr, "Error: No empty space to place player %c\n", playerId);
            return false;
        }
        labyrinth->map[current.row][current.col] = playerId;
    }
    
    Position target = current;
    
    // Calculate target position based on direction
    if (strcmp(direction, "up") == 0) {
        target.row--;
    } else if (strcmp(direction, "down") == 0) {
        target.row++;
    } else if (strcmp(direction, "left") == 0) {
        target.col--;
    } else if (strcmp(direction, "right") == 0) {
        target.col++;
    } else {
        fprintf(stderr, "Error: Invalid direction '%s'\n", direction);
        return false;
    }
    
    // Check if target is valid
    if (target.row < 0 || target.row >= labyrinth->rows || 
        target.col < 0 || target.col >= labyrinth->cols) {
        fprintf(stderr, "Error: Cannot move outside map boundaries\n");
        return false;
    }
    
    char target_cell = labyrinth->map[target.row][target.col];
    if (target_cell == '#') {
        fprintf(stderr, "Error: Cannot move into wall\n");
        return false;
    }
    
    if (target_cell >= '0' && target_cell <= '9') {
        fprintf(stderr, "Error: Cannot move into another player's position\n");
        return false;
    }
    
    // Perform move
    labyrinth->map[current.row][current.col] = '.';
    labyrinth->map[target.row][target.col] = playerId;
    
    return true;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    FILE *file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "Error: Cannot save map to '%s'\n", filename);
        return false;
    }
    
    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            fputc(labyrinth->map[row][col], file);
        }
        fputc('\n', file);
    }
    
    fclose(file);
    return true;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return;
    }
    
    if (visited[row][col]) {
        return;
    }
    
    char ch = labyrinth->map[row][col];
    if (ch == '#') {
        return;
    }
    
    visited[row][col] = true;
    
    // Check all four directions
    dfs(labyrinth, row - 1, col, visited); // up
    dfs(labyrinth, row + 1, col, visited); // down
    dfs(labyrinth, row, col - 1, visited); // left
    dfs(labyrinth, row, col + 1, visited); // right
}

bool isConnected(Labyrinth *labyrinth) {
    if (labyrinth->rows == 0 || labyrinth->cols == 0) {
        return false;
    }
    
    bool visited[MAX_ROWS][MAX_COLS] = {false};
    
    // Find first empty space or player position to start DFS
    bool found_start = false;
    int start_row = -1, start_col = -1;
    
    for (int row = 0; row < labyrinth->rows && !found_start; row++) {
        for (int col = 0; col < labyrinth->cols && !found_start; col++) {
            char ch = labyrinth->map[row][col];
            if (ch == '.' || (ch >= '0' && ch <= '9')) {
                start_row = row;
                start_col = col;
                found_start = true;
            }
        }
    }
    
    if (!found_start) {
        return false; // No empty spaces or players
    }
    
    dfs(labyrinth, start_row, start_col, visited);
    
    // Check if all empty spaces and player positions are visited
    for (int row = 0; row < labyrinth->rows; row++) {
        for (int col = 0; col < labyrinth->cols; col++) {
            char ch = labyrinth->map[row][col];
            if ((ch == '.' || (ch >= '0' && ch <= '9')) && !visited[row][col]) {
                return false;
            }
        }
    }
    
    return true;
}

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"map", required_argument, 0, 'm'},
        {"player", required_argument, 0, 'p'},
        {"move", required_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    bool has_map = false;
    bool has_player = false;
    
    // Parse command line arguments
    while ((opt = getopt_long(argc, argv, "m:p:d:v", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'm':
                map_file = optarg;
                has_map = true;
                break;
            case 'p':
                if (strlen(optarg) != 1 || !isValidPlayer(optarg[0])) {
                    fprintf(stderr, "Error: Invalid player ID '%s'. Must be 0-9\n", optarg);
                    return EXIT_FAILURE;
                }
                player_id = optarg[0];
                has_player = true;
                break;
            case 'd':
                direction = optarg;
                break;
            case 'v':
                version_flag = true;
                break;
            case '?':
            default:
                printUsage();
                return EXIT_FAILURE;
        }
    }
    
    // Check for extra arguments
    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected arguments\n");
        printUsage();
        return EXIT_FAILURE;
    }
    
    // Handle --version flag
    if (version_flag) {
        if (has_map || has_player || direction) {
            fprintf(stderr, "Error: --version cannot be used with other options\n");
            return EXIT_FAILURE;
        }
        printf("%s\n", VERSION);
        return EXIT_SUCCESS;
    }
    
    // Check required arguments
    if (!has_map || !has_player) {
        fprintf(stderr, "Error: Missing required arguments\n");
        printUsage();
        return EXIT_FAILURE;
    }
    
    // Load the map
    Labyrinth labyrinth;
    if (!loadMap(&labyrinth, map_file)) {
        return EXIT_FAILURE;
    }
    
    // Check maze connectivity
    if (!isConnected(&labyrinth)) {
        fprintf(stderr, "Error: Maze is not fully connected\n");
        return EXIT_FAILURE;
    }
    
    // Handle movement
    if (direction) {
        if (!movePlayer(&labyrinth, player_id, direction)) {
            return EXIT_FAILURE;
        }
        // Save updated map
        if (!saveMap(&labyrinth, map_file)) {
            return EXIT_FAILURE;
        }
    }
    
    // Print the map
    for (int row = 0; row < labyrinth.rows; row++) {
        for (int col = 0; col < labyrinth.cols; col++) {
            putchar(labyrinth.map[row][col]);
        }
        putchar('\n');
    }
    
    return EXIT_SUCCESS;
}
