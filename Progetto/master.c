#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>

#define TEST_ERROR    if (errno) {fprintf(stderr, \
					   "%s:%d: PID=%5d: Error %d (%s)\n",\
					   __FILE__,\
					   __LINE__,\
					   getpid(),\
					   errno,\
					   strerror(errno));}

#define FILENAME_SHM  "shm_file.txt"
#define FILENAME_SEM_BOARD "sem_board_file.txt"
#define FILENAME_MSGID  "msgid_file.txt"
#define LENGTH 120

enum cmd{START_ROUND = 0, END_ROUND = 1};

struct piece {
	char type;
	int value;
	pid_t owner;
	int x,y;
	};

typedef struct Player {
	pid_t pid;
	int score;
	}Player;

void fillEmpty();
void printBoard();
void endSimulation();
void new_round();
int getIndex(pid_t player_pid);

extern char **environ;

struct piece *board;
Player *player;

char *args[] = {"./player", NULL};

int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G, SO_FLAG_MIN, SO_FLAG_MAX, SO_ROUND_SCORE;

int shm_id, sem_board_id, msg_id;

struct sembuf sem_board;
struct msgbuf msg_queue;

int main(int argc, char * argv[], char** env){

	FILE * shm_file;
	FILE * sem_board_file;
	FILE * msgid_file;

	key_t msg_key;

	pid_t child_pid;
	int i;
	long rcv;

	SO_FLAG_MIN = atoi(getenv("SO_FLAG_MIN"));
	SO_FLAG_MAX = atoi(getenv("SO_FLAG_MAX"));
	SO_ROUND_SCORE = atoi(getenv("SO_ROUND_SCORE"));
	SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_BASE = atoi(getenv("SO_BASE"));
	SO_NUM_P = atoi(getenv("SO_NUM_P"));
	SO_NUM_G = atoi(getenv("SO_NUM_G"));

	player = (Player *) malloc(sizeof(Player)*SO_NUM_G);

	sem_board_id = semget(IPC_PRIVATE, 1 + SO_BASE*SO_ALTEZZA, 0600);
	semctl(sem_board_id, SO_BASE*SO_ALTEZZA, SETVAL, 0);
	sem_board.sem_num = SO_BASE*SO_ALTEZZA;
	sem_board.sem_flg = 0;

    shm_id = shmget(IPC_PRIVATE, sizeof(struct piece)*60*20, IPC_CREAT | IPC_EXCL | 0600);
    board = (struct piece*) shmat(shm_id, NULL, 0);

    shm_file = fopen(FILENAME_SHM, "w");
	fprintf(shm_file, "%d\n", shm_id);
	fclose(shm_file);

	sem_board_file = fopen(FILENAME_SEM_BOARD, "w");
	fprintf(sem_board_file, "%d\n", sem_board_id);
	fclose(sem_board_file);

	for (msg_key = IPC_PRIVATE+1; msg_key != IPC_PRIVATE; msg_key++) {

		msg_id = msgget(msg_key, 0600 | IPC_CREAT | IPC_EXCL);
		if (msg_id == -1)  {
			if (errno == EEXIST) {
				fprintf(stderr,	"La chiave %d è gia esistente\n",
					msg_key);
				continue;
			}
			if (errno == EACCES) {
				continue;
			}
			if (errno == ENOMEM || errno == ENOSPC) {
				fprintf(stderr,	"%s\n", strerror(errno));
				return(-1);
			}
		}

		msgid_file = fopen(FILENAME_MSGID, "w");
		fprintf(msgid_file, "%d\n", msg_key);
		fclose(msgid_file);
		printf("Nuova coda di messaggi %d, scritta in \"%s\"\n",
			msg_key, FILENAME_MSGID);
		break;
	}
	if (msg_key == IPC_PRIVATE) {
		fprintf(stderr,	"Nessuna chiave disponibile!\n");
		return(-1);
	}

	fillEmpty();
    printBoard();

	printf("hey sono il master %d.\n", getpid());

	for(i=0; i<SO_NUM_G; i++){
		switch(child_pid = fork()){
			case -1:
				TEST_ERROR;
				exit(EXIT_FAILURE);
			case 0:
				execve(args[0], args, env);
				break;
			default:
				break;
		}

		player[i].pid = child_pid;
		player[i].score = 0;
	}

	rcv = (long) getpid();
	for(i = 0; i < SO_NUM_P*SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i%SO_NUM_G].pid);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
		sem_board.sem_op = 1;
		sem_board.sem_num= SO_BASE*SO_ALTEZZA;
		semop(sem_board_id, &sem_board, 1);
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
	}

	printBoard();
    printf("\n");
    new_round();
    endSimulation();

    return 0;
}

/**
 * [new_round description]
 */
void new_round(){
	int i, r, points, num_flags;

	srand(time(NULL));
	num_flags = SO_FLAG_MIN + rand() % ((SO_FLAG_MAX+1) - SO_FLAG_MIN);
	points = SO_ROUND_SCORE;

	for(i = num_flags; i > 0; i--){
		do{
			r = rand() % (SO_BASE * SO_ALTEZZA);
		}while(board[r].type != 'e');
		board[r].type = 'f';
		if(points > i) board[r].value = 1 + rand()% (points-i+1);
		else if (points == i) board[r].value = 1;
		points = points - (board[r].value);
	}
	printBoard();
  	printf("\n");


	printf("%d\n", START_ROUND);

	/*START_ROUND*/
	for(i = 0; i < SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i].pid);
		sprintf(msg_queue.mtext, "%d", START_ROUND);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}

	sleep(5);

	/*END_ROUND*/
	for(i = 0; i < SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i].pid);
		sprintf(msg_queue.mtext, "%d", END_ROUND);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}
}

/**
 * [fillEmpty description]
 */
void fillEmpty(){
	int i,j;

	for(i=0;i<SO_ALTEZZA;i++){
        for(j=0;j<SO_BASE;j++){
            board[i*SO_BASE + j].type = 'e';
            board[i*SO_BASE + j].value = 0;
			board[i*SO_BASE + j].owner = 0;
            board[i*SO_BASE + j].x = j;
            board[i*SO_BASE + j].y = i;
            semctl(sem_board_id, i*SO_BASE + j, SETVAL, 1);
        }
    }
}

/**
 * [printBoard description]
 */
void printBoard(){
	int a,b;

	for(a=0;a<SO_ALTEZZA;a++){
        for(b=0;b<SO_BASE;b++){
        	if(board[a*SO_BASE + b].type == 'e'){
        		printf("#");
        	} else if(board[a*SO_BASE + b].type == 'p'){
				switch(getIndex(board[a*SO_BASE + b].owner)){
					case 0:
						printf("\033[1;31m");
						printf("X");
						printf("\033[0m");
						break;
					case 1:
						printf("\033[1;32m");
						printf("X");
						printf("\033[0m");
						break;
					case 2:
						printf("\033[1;33m");
						printf("X");
						printf("\033[0m");
						break;
					case 3:
						printf("\033[1;34m");
						printf("X");
						printf("\033[0m");
						break;
					default:
						printf("Error player not in game\n");
						break;
				}
        	} else if(board[a*SO_BASE + b].type == 'f'){
				printf("\033[1;36m");
				printf("4");
				printf("\033[0m");
			}
        }
        printf("\n");
    }
}

/**
 * [endSimulation description]
 */
void endSimulation(){
	int i;

	printf("END_GAME\n");
	for(i=0; i<SO_NUM_G; i++){
		printf("player -> %d score -> %d\n", player[i].pid, player[i].score);
		kill(player[i].pid, SIGTERM);
	}
	shmctl(shm_id, IPC_RMID, NULL);
	semctl(sem_board_id, 0, IPC_RMID);
	msgctl(msg_id, IPC_RMID, NULL);
	free(player);
}

/**
 * [getIndex description]
 * @param  player_pid [description]
 * @return            [description]
 */
int getIndex(pid_t player_pid){
	int i;

	for(i = 0; i < SO_NUM_G; i++){
		if (player[i].pid == player_pid) return i;
	}
	return -1;
}
