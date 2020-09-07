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
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>

#define TEST_ERROR    if (errno) {fprintf(stderr, \
					   "[PAWN_ERROR] %s:%d: PID=%5d: Error %d (%s)\n",\
					   __FILE__,\
					   __LINE__,\
					   getpid(),\
					   errno,\
					   strerror(errno));}

#define FILENAME_INI  "./ipcs.ini"
#define LENGTH 500
#define BUF_SIZE 500

/*
	enumerations
 */
enum cmd{END_ROUND = 1, MOVE_TO = 2, START_MOVING = 3, CAUGHT = 4, POSITION = 6};
enum dir{LEFT = -1, DOWN = -1, RIGHT = 1, UP = 1, NONE = 0};

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
int move_to(struct Pair target);
int step_x(int dir);
int step_y(int dir);
int path_to_flag(int delta_x, int delta_y, int dir_x, int dir_y);
struct Pair delta_flag(int flag_x, int flag_y);

/*
	global variables
 */
struct piece *board;
struct sembuf sem_board;
struct Pair target;

int SO_BASE, SO_ALTEZZA, SO_N_MOVES, SO_MIN_HOLD_NSEC;
int sem_board_id, shm_id, msg_id, prt_msg_id;
int self_x, self_y, self_ind, caught_points;

int main(int argc, char * argv[], char** env){
    long rcv;

	char *line; /* line to write in ipcs.ini */
	char *split_msg; /* msg to split in rcv() */

	size_t len = 0;
    ssize_t read;

    struct msgbuf msg_queue;
	struct msgbuf prt_msg_queue;

    struct sigaction sa;
	sigset_t my_mask;

    key_t msg_key;
	key_t prt_msg_key;

	FILE *ini_file;

    SO_BASE = atoi(getenv("SO_BASE"));
    SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
    SO_N_MOVES = atoi(getenv("SO_N_MOVES"));
    SO_MIN_HOLD_NSEC = atoi(getenv("SO_MIN_HOLD_NSEC"));

	line = malloc(sizeof(char)*100);
	bzero(line, 100);

	/*
		sigaction init
	*/
    sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

	/*
		starts reading from ipcs.ini
	*/
	ini_file = fopen(FILENAME_INI, "r");

	if(ini_file == NULL) {
    	perror("Error opening file");
    	return(-1);
   	}

	/*
		gets semaphore id in ipcs.ini
	*/
   	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	sem_board_id = atoi(line);
		bzero(line, 100);
   	}

	/*
		gets shared memory id in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	shm_id = atoi(line);
		bzero(line, 100);
   	}

	board = (struct piece*) shmat(shm_id, NULL, 0);

	/*
		gets primary message key in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	msg_key = atoi(line);
		bzero(line, 100);
   	}

	if (msg_key == IPC_PRIVATE) {
		fprintf(stderr, "[PAWN_ERROR] PAWN %d wrong key %d\n", getpid(), IPC_PRIVATE);
		return(-1);
	}
	msg_id = msgget(msg_key, 0600);
	if (msg_id == -1) TEST_ERROR;

	/*
		gets priority message key in ipcs.ini
	*/
	if( (read = getline(&line, &len, ini_file)) != -1 ) {
    	prt_msg_key = atoi(line);
   	}

	if (prt_msg_key == IPC_PRIVATE) {
		fprintf(stderr, "[PAWN_ERROR] PAWN %d wrong key %d\n", getpid(), IPC_PRIVATE);
		return(-1);
	}
	prt_msg_id = msgget(prt_msg_key, 0600);
	if (prt_msg_id == -1) TEST_ERROR;

    /*
        Confirm INIT
    */
    sem_board.sem_op = 1;
	sem_board.sem_num = SO_BASE*SO_ALTEZZA + 1;
	semop(sem_board_id, &sem_board, 1);

	/*
		gets self position from msg queue
	 */
    rcv = (long) getpid();
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
			split_msg = strtok (msg_queue.mtext," ");
			switch (atoi(split_msg)) {

				/*
					gets coordinates (x,y) of the target flag
				 */
				case MOVE_TO:
					msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
					split_msg = strtok (msg_queue.mtext," ");
					target.x = atoi(split_msg);
					split_msg = strtok (NULL, " ");
					target.y =  atoi(split_msg);
					break;

				/*
					starts moving to the target flag
				 */
				case START_MOVING:
					caught_points = move_to(target);

					/*
						Tells the player that a flag has been caught
						if points are not zero
					*/
					if(caught_points > 0){
						msg_queue.mtype = (long) getppid();
						sprintf(msg_queue.mtext, "%d %d %d", CAUGHT, caught_points, self_ind);
						msgsnd(msg_id, &msg_queue, LENGTH, 0);
					}
					break;

				/*
					sends self coordinates (x,y)
				 */
				case POSITION:
					prt_msg_queue.mtype = (long) getppid();
					sprintf(prt_msg_queue.mtext, "%d %d", self_x, self_y);
					msgsnd(prt_msg_id, &prt_msg_queue, LENGTH, 0);
					break;

				/*
					sends moves left
				*/
				case END_ROUND:
					prt_msg_queue.mtype = (long) getppid();
					sprintf(prt_msg_queue.mtext, "%d", SO_N_MOVES);
					msgsnd(prt_msg_id, &prt_msg_queue, LENGTH, 0);
					break;
				default:
					break;
			}
		}
	}
    exit(EXIT_FAILURE);
}

/**
 * determinates if the pawn can move to the target flag if can moves it
 * @param  target target flag
 * @return      points of the target flag
 */
int move_to(struct Pair target){
	struct Pair delta;
	int dist, points;

	srand(time(NULL));
	delta = delta_flag(target.x, target.y);
	dist = abs(delta.x) + abs(delta.y);
	/*
		moves only if it can
	 */
	if(dist <= SO_N_MOVES){
		if(delta.x == 0 && delta.y > 0){
			points = path_to_flag(delta.x, delta.y, NONE, UP);
		}else if(delta.x > 0 && delta.y == 0){
			points = path_to_flag(delta.x, delta.y, RIGHT, NONE);
		}else if(delta.x == 0 && delta.y < 0){
			points = path_to_flag(delta.x, delta.y, NONE, DOWN);
		}else if(delta.x < 0 && delta.y == 0){
			points = path_to_flag(delta.x, delta.y, LEFT, NONE);
		}else if(delta.x > 0 && delta.y > 0){
			points = path_to_flag(delta.x, delta.y, RIGHT, UP);
		}else if(delta.x > 0 && delta.y < 0){
			points = path_to_flag(delta.x, delta.y, RIGHT, DOWN);
		}else if(delta.x < 0 && delta.y > 0){
			points = path_to_flag(delta.x, delta.y, LEFT, UP);
		}else if(delta.x < 0 && delta.y < 0){
			points = path_to_flag(delta.x, delta.y, LEFT, DOWN);
		}
	}
	return points;
}

/**
 * calculate the distance on x and on y
 * @param  flag_x flag's coordinate x
 * @param  flag_y flag's coordinate y
 * @return        a Pair (delta_x, delta_y)
 */
struct Pair delta_flag(int flag_x, int flag_y){
	struct Pair delta;

	delta.x = flag_x - self_x;
	delta.y = flag_y - self_y;
	return delta;
}

/**
 * moves the pawn to the targeg flag
 * @param delta_x y component of movement
 * @param delta_y y component of movement
 * @param dir_x   direction on x
 * @param dir_y   direction on y
 */
int path_to_flag(int delta_x, int delta_y, int dir_x, int dir_y){
	int target_index, points, random, ret = -1;

	target_index = target.y * SO_BASE + target.x;
	points = board[target_index].value;
	delta_x = abs(delta_x);
	delta_y = abs(delta_y);
	srand(time(NULL));

	for(; delta_x > 0 || delta_y > 0;){
		/*
			check if target flag exists
		 */
		if(board[target_index].type != 'f') return -1;

		if(delta_x > 0 && delta_y > 0){
			/*
				randomly moves on x or y
			 */
			random = rand() % 2;
			switch(random){
				case 0:
					ret = step_x(dir_x);
					if (ret == 0) delta_x--;
					break;
				case 1:
					ret = step_y(dir_y);
					if (ret == 0) delta_y--;
					break;
				default:
					break;
			}
		}else if(delta_x > 0 && delta_y == 0){
			/*
				only moves on x
			 */
			ret = step_x(dir_x);
			if (ret == 0) delta_x--;

		}else if(delta_x == 0 && delta_y > 0){
			/*
				only moves on y
			 */
			ret = step_y(dir_y);
 			if (ret == 0) delta_y--;
		}
	}

	if(target_index == self_ind){
		return points;
	}

	return -1;
}

/**
 * moves one step on x if possible
 * @param dir direction
 */
int step_x(int dir){
	int ret_val, index;
	int milisec = SO_MIN_HOLD_NSEC;

	/*
		struct needed in nanosleep(...)
	*/
	struct timespec req = {0};
	req.tv_sec = 0;
	req.tv_nsec = milisec * 1000000L;

	index = self_y * SO_BASE + self_x + dir;
	sem_board.sem_op = -1;
	sem_board.sem_num= index;
	sem_board.sem_flg = IPC_NOWAIT;
	ret_val = semop(sem_board_id, &sem_board, 1);
	if(ret_val == -1){
		if (errno == EAGAIN) return -1;
		else TEST_ERROR;
	}else{
		board[index].type = 'p';
		board[index].owner = getppid();
		board[index].value = getpid();
		board[self_ind].type = 'e';
		board[self_ind].value = 0;
		board[self_ind].owner = 0;
		sem_board.sem_op = 1;
		sem_board.sem_num= self_ind;
		semop(sem_board_id, &sem_board, 1);
		self_ind = index;
		self_x += dir;
		SO_N_MOVES -= 1;
		nanosleep(&req, (struct timespec *)NULL);
	}
	return 0;
}

/**
 * moves one step on y if possible
 * @param dir direction
 */
int step_y(int dir){
	int ret_val, index;
	int milisec = SO_MIN_HOLD_NSEC;

	/*
		struct needed in nanosleep(...)
	*/
	struct timespec req = {0};
	req.tv_sec = 0;
	req.tv_nsec = milisec * 1000000L;

	index = (self_y + dir) * SO_BASE + self_x;
	sem_board.sem_op = -1;
	sem_board.sem_num= index;
	sem_board.sem_flg = IPC_NOWAIT;
	ret_val = semop(sem_board_id, &sem_board, 1);
	if(ret_val == -1){
		if (errno == EAGAIN) return -1;
		else TEST_ERROR;
	}else{
		board[index].type = 'p';
		board[index].owner = getppid();
		board[index].value = getpid();
		board[self_ind].type = 'e';
		board[self_ind].value = 0;
		board[self_ind].owner = 0;
		sem_board.sem_op = 1;
		sem_board.sem_num= self_ind;
		semop(sem_board_id, &sem_board, 1);
		self_ind = index;
		self_y += dir;
		SO_N_MOVES -= 1;
		nanosleep(&req, (struct timespec *)NULL);
	}
	return 0;
}

/**
 * handles SIGTERM signal, exits with left moves
 * @param signal SIGTERM
 */
void handle_term(int signal){

	if (SO_N_MOVES < 0) {
			fprintf(stderr, "[PAWN_ERROR] PAWN %d something goes wrong.\n", getpid());
			_exit(EXIT_FAILURE);
	}

	_exit(SO_N_MOVES);
}
