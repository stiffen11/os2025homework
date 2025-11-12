#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <testkit.h>
#include<getopt.h>
#include "labyrinth.h"

int main(int argc, char *argv[]) {
    // TODO: Implement this function
    const char *short_opts="m:p:";
    char *map_file=NULL;
    int version_flag=0;
    int mov_flag=0;
    int player_id=0;
    struct option long_opts[]={
	    {"version",no_argument,&version_flag,1},
	    {"map",required_argument,0,"m"},
	    {"player",required_argument,0,"p"},
	    {"move",required_argument,&mov_flag,1},
	    {0,0,0,0}
    };
int opt=0;
    while((opt=getopt_long(argc,argv,short_opts,long_opts))!=-1){
            switch(opt){
		    case 'm':    
		    	map_file=optarg;
		    	break;
		    case 'p':
		    	player_id=optarg;
		    	break;
		default:
			printUsage();
			break;

   }
        if(map_file!=NULL){
    FILE *file=fopen("../maps/filename","r");
    if(file==NULL){
	    perror("fail");
	    return;
    }
    char buffer[256];
    while(fgets(buffer,sizeof(buffer),file)!=NULL){
	    printf("%s",buffer);
    }
    fclose(file);
	}



    if(version_flag==1){
	    printf("version 1.0");
    }
    if(mov_flag==1){
	  printf("up\ndown\nleft\nright\n");
    }

    return 0;
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    // TODO: Implement this function
    return false;
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    // TODO: Implement this function

    return false;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    // TODO: Implement this function
    Position pos = {-1, -1};
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // TODO: Implement this function
    return false;
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // TODO: Implement this function
    return false;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    // TODO: Implement this function
    return false;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    // TODO: Implement this function
}

bool isConnected(Labyrinth *labyrinth) {
    // TODO: Implement this function
    return false;
}
