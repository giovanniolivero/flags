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

/*
	enumerations
 */
enum cmd{START_ROUND = 0, END_ROUND = 1, START_MOVING = 3, CAUGTH = 4, FLAGS = 5};

/*
	structs
 */
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

/*
	function prototypes
 */
void fill_empty();
void print_board();
void end_simulation();
void new_round();
void ipc_rmv();
void handle_alarm(int signal);
int get_index(pid_t player_pid);

/*
	environment variables
 */
extern char **environ;

/*
	global variables
 */
struct piece *board;
Player *player;

char *args[] = {"./player", NULL};

int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G,
	SO_FLAG_MIN, SO_FLAG_MAX, SO_ROUND_SCORE, SO_MAX_TIME;

int shm_id, sem_board_id, msg_id;

struct sembuf sem_board;
struct msgbuf msg_queue;

int main(int argc, char * argv[], char** env){

	struct sigaction sa;
	sigset_t my_mask;


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
	SO_MAX_TIME = atoi(getenv("SO_MAX_TIME"));

	sa.sa_handler = &handle_alarm;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGALRM, &sa, NULL);

	player = (Player*)malloc(sizeof(Player)*SO_NUM_G);

	sem_board_id = semget(IPC_PRIVATE, 1 + SO_BASE*SO_ALTEZZA, 0600);
	semctl(sem_board_id, SO_BASE*SO_ALTEZZA, SETVAL, 0);
	sem_board.sem_num = SO_BASE*SO_ALTEZZA;
	sem_board.sem_flg = 0;

    shm_id = shmget(IPC_PRIVATE, sizeof(struct piece)*60*20,
					IPC_CREAT | IPC_EXCL | 0600);
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

	fill_empty();
    print_board();

	printf("[MASTER] Hi i'm Master PID: %d.\n", getpid());
	printf("------------------------------------------\n");
	/*
		generates players, saves players pid, init score
	 */
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

	/*
		lets players place their pawns of one by one
	 */
	rcv = (long) getpid();
	for(i = 0; i < SO_NUM_P*SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i%SO_NUM_G].pid);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
		sem_board.sem_op = 1;
		sem_board.sem_num= SO_BASE*SO_ALTEZZA;
		semop(sem_board_id, &sem_board, 1);
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
	}

	print_board();
    printf("\n");
	for(;;){
    	new_round();
	}
    return 0;
}

/**
 * Init a new round, generate #flags, place flags on the board
 * and communuìicate with players
 */
void new_round(){
	static int count = 1;
	int i, r, points, num_flags, cmd, player_pid, player_index;
	long rcv;
	char *split_msg;

	srand(time(NULL));
	num_flags = SO_FLAG_MIN + rand() % ((SO_FLAG_MAX+1) - SO_FLAG_MIN);
	points = SO_ROUND_SCORE;

	/*
		places flags with random points
	 */
	for(i = num_flags; i > 0; i--){
		do{
			r = rand() % (SO_BASE * SO_ALTEZZA);
		}while(board[r].type != 'e');
		board[r].type = 'f';
		if(points > i) board[r].value = 1 + rand()% (points-i+1);
		else if (points == i) board[r].value = 1;
		points = points - (board[r].value);
	}
	print_board();
  	printf("\n");

	alarm(SO_MAX_TIME);

	/*
		Sends all players START_ROUND msg and #flags
	 */
	printf("[MASTER] Round #%d started\n", count);
	printf("------------------------------------------\n");
	for(i = 0; i < SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i].pid);
		sprintf(msg_queue.mtext, "%d", START_ROUND);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);

		msg_queue.mtype = (long)(player[i].pid);
		sprintf(msg_queue.mtext, "%d", num_flags);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}

	/*
		waits for all players to give directions to their pawns
	 */
	rcv = (long) getpid();
	for(i = 0; i < SO_NUM_G; i++){
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
   	}

	/*
		sends all players START_MOVING msg
	 */
   	for(i = 0; i < SO_NUM_G; i++){
	   msg_queue.mtype = (long)(player[i].pid);
	   sprintf(msg_queue.mtext, "%d", START_MOVING);
	   msgsnd(msg_id, &msg_queue, LENGTH, 0);
   	}

	/*
		waits for all flags to be caught
	 */
	while(1){
		msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
		split_msg = strtok (msg_queue.mtext," ");
		cmd = atoi(split_msg);
		if(cmd == CAUGTH){
			num_flags--;
			split_msg = strtok (NULL, " ");
			player_pid =  atoi(split_msg);
			split_msg = strtok (NULL, " ");
			points =  atoi(split_msg);

			/*
			sends all player the number of the remaining flags
			 */
			for(i = 0; i < SO_NUM_G; i++){
				msg_queue.mtype = (long)(player[i].pid);
				sprintf(msg_queue.mtext, "%d %d", FLAGS, num_flags);
				msgsnd(msg_id, &msg_queue, LENGTH, 0);
			}

			player_index = get_index(player_pid);
			player[player_index].score += points;
			printf("[MASTER] Player %d caught a %d flag.\n", player_pid, points);
			printf("[MASTER] %d flags remaining.\n", num_flags);
			printf("------------------------------------------\n");
		}
		if (num_flags <= 0) break;
	}

	/*
		sends all players END_ROUND msg
	 */
	for(i = 0; i < SO_NUM_G; i++){
		msg_queue.mtype = (long)(player[i].pid);
		sprintf(msg_queue.mtext, "%d", END_ROUND);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}

	printf("[MASTER] Round #%d ended.\n", count);
	printf("------------------------------------------\n");
	printf("[MASTER] Board after round #%d.\n", count);
	printf("------------------------------------------\n");
	print_board();
	count++;
}

/**
 * sets all pieces of the board to empty
 */
void fill_empty(){
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
 * prints the board with different colors for pawns and flags
 */
void print_board(){
	int a,b;

	for(a=0;a<SO_ALTEZZA;a++){
        for(b=0;b<SO_BASE;b++){
        	if(board[a*SO_BASE + b].type == 'e'){
        		printf("#");
        	} else if(board[a*SO_BASE + b].type == 'p'){
				switch(get_index(board[a*SO_BASE + b].owner)){
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
						printf("[MASTER] Error player not in game\n");
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
	printf("------------------------------------------\n");
}

/**
 * Handles SIGALRM signal: ends the game, terminates players and show final score
 * @param signal SIGALRM signal
 */
void handle_alarm(int signal){
	int i, status;
	pid_t child_pid;

	printf("[MASTER] The game is ended\n");
	printf("------------------------------------------\n");
	print_board();
	for(i=0; i<SO_NUM_G; i++){
		printf("[MASTER] Player -> %d Score -> %d\n", player[i].pid, player[i].score);
		kill(player[i].pid, SIGTERM);
	}

	/*
		waits for all players to terminate their pawns
	 */
	while ((child_pid = wait(&status)) != -1) {
		printf("[MASTER] Terminated player process...\n");
	}
	if (errno != ECHILD) {
		fprintf(stderr, "Error #%d: %s\n", errno, strerror(errno));
		ipc_rmv();
		exit(EXIT_FAILURE);
	}
	ipc_rmv();
	printf("[MASTER] Bye bye.\n");
	printf("------------------------------------------\n");
	exit(EXIT_SUCCESS);
}

/**
 * gets the index of a player
 * @param  player_pid: player to find index
 * @return            index if playes in players[], -1 otherwise
 */
int get_index(pid_t player_pid){
	int i;

	for(i = 0; i < SO_NUM_G; i++){
		if (player[i].pid == player_pid) return i;
	}
	return -1;
}

/**
 * removes all ipcs, shared memory and free players
 */
void ipc_rmv(){
	shmctl(shm_id, IPC_RMID, NULL);
	semctl(sem_board_id, 0, IPC_RMID);
	msgctl(msg_id, IPC_RMID, NULL);
	free(player);
}
