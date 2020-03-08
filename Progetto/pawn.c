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
enum cmd{MOVE_TO = 2, START_MOVING = 3};
enum dir{LEFT = -1, DOWN = -1, RIGHT = -1, UP = 1, NONE = 0};

/*
	structs
 */
struct piece {
	char type;
	int value;
	int owner;
	int x,y;
};

struct Pair{
	int x, y;
};

/*
	function prototypes
 */
void handle_term(int signal);
void move_to(struct Pair move);
void step_x(int dir);
void step_y(int dir);
void path_to_flag(
	int start_x, int start_y,
	int delta_x, int delta_y,
	int dir_x, int dir_y
);
struct Pair delta_flag(int flag_x, int flag_y);

/*
	global variables
 */
struct piece *board;
struct sembuf sem_board;

int SO_BASE, SO_N_MOVES;
int sem_board_id, shm_id, msg_id;
int self_x, self_y, self_ind;

int main(int argc, char * argv[], char** envp){

	long rcv;
	int go_to;
	struct Pair move;

	struct msgbuf msg_queue;
	struct sigaction sa;
	sigset_t my_mask;

	key_t msg_key;

	FILE *shm_file;
	FILE *sem_board_file;
	FILE *msgid_file;

	char * split_msg;

	sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

	SO_BASE = atoi(getenv("SO_BASE"));
	SO_N_MOVES = atoi(getenv("SO_N_MOVES"));

	shm_file = fopen(FILENAME_SHM, "r");
	fscanf(shm_file, "%d", &shm_id);
	fclose(shm_file);

	board = (struct piece*) shmat(shm_id, NULL, 0);

	sem_board_file = fopen(FILENAME_SEM_BOARD, "r");
	fscanf(sem_board_file, "%d", &sem_board_id);
	fclose(sem_board_file);

	msgid_file = fopen(FILENAME_MSGID, "r");
	fscanf(msgid_file, "%d", &msg_key);
	fclose(msgid_file);

	if (msg_key == IPC_PRIVATE) {
			fprintf(stderr, "Chiave %d errata\n", IPC_PRIVATE);
			return(-1);
		}
	msg_id = msgget(msg_key, 0600);
	if (msg_id == -1) TEST_ERROR;

	rcv = (long) getpid();

	/*
		gets self position from msg
	 */
	if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
		split_msg = strtok (msg_queue.mtext," ");
		self_x = atoi(split_msg);
		split_msg = strtok (NULL, " ");
		self_y =  atoi(split_msg);
		self_ind = self_y * SO_BASE + self_x;
		board[self_ind].value = getpid();
	}

	/*
		waits for any message from the player
	 */
	for(;;){
		if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)>0) {
			switch (atoi(msg_queue.mtext)) {
				/*
					gets coordinates (x,y) of the target flag
				 */
				case MOVE_TO:
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					split_msg = strtok (msg_queue.mtext," ");
					move.x = atoi(split_msg);
					split_msg = strtok (NULL, " ");
					move.y =  atoi(split_msg);
					break;
				/*
					star moving to the target flag
				 */
				case START_MOVING:
					move_to(move);
					break;
				default:
					break;
			}
		}
	}
	exit(EXIT_SUCCESS);
}

/**
 * [handle_term description]
 * @param signal [description]
 */
void handle_term(int signal){
	exit(EXIT_SUCCESS);
}

/**
 * [move_to description]
 * @param move [description]
 */
void move_to(struct Pair move){
	struct Pair delta;
	int dist;

	srand(time(NULL));
	delta = delta_flag(move.x, move.y);
	dist = abs(delta.x) + abs(delta.y);
	if(dist <= SO_N_MOVES){
		printf("MOVING.... dist = %d SO_N_MOVES = %d\n", dist, SO_N_MOVES);
		if(delta.x == 0 && delta.y > 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, NONE, UP);
		}else if(delta.x > 0 && delta.y == 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, RIGHT, NONE);
		}else if(delta.x == 0 && delta.y < 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, NONE, DOWN);
		}else if(delta.x < 0 && delta.y == 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, LEFT, NONE);
		}else if(delta.x > 0 && delta.y > 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, RIGHT, UP);
		}else if(delta.x > 0 && delta.y < 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, RIGHT, DOWN);
		}else if(delta.x < 0 && delta.y > 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, LEFT, UP);
		}else if(delta.x < 0 && delta.y < 0){
			path_to_flag(self_x, self_y, delta.x, delta.y, LEFT, DOWN);
		}
	}else{
		printf("NOT MOVING.... dist = %d SO_N_MOVES = %d\n", dist, SO_N_MOVES);
	}
}

/**
 * [delta_flag description]
 * @param  flag_x [description]
 * @param  flag_y [description]
 * @return        [description]
 */
struct Pair delta_flag(int flag_x, int flag_y){
	int dist_x, dist_y;
	struct Pair delta;

	delta.x = flag_x - self_x;
	delta.y = flag_y - self_y;
	return delta;
}

/**
 * [path_to_flag description]
 * @param start_x [description]
 * @param start_y [description]
 * @param delta_x [description]
 * @param delta_y [description]
 * @param dir_y   [description]
 * @param dir_x   [description]
 */
void path_to_flag(int start_x, int start_y,
	int delta_x, int delta_y,
	int dir_y, int dir_x
){

}
/**
 * [step_x description]
 * @param dir [description]
 */
void step_x(int dir){

}

/** [step_y description] */
void step_y(int dir){

}
