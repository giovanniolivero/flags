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
#include <sys/stat.h>
#include <fcntl.h>

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
#define LENGTH 500
#define BUF_SIZE 100

/*
	enumerations
 */
enum cmd{MOVE_TO = 2, START_MOVING = 3, CAUGHT = 4, POSITION = 6};
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
int sem_board_id, shm_id, msg_id;
int self_x, self_y, self_ind, caught_points;
short send_flg;

int main(int argc, char * argv[], char** envp){

	long rcv;
	char * split_msg;

	struct msgbuf msg_queue;
	struct sigaction sa;
	sigset_t my_mask;

	key_t msg_key;

	FILE * shm_file;
	FILE * sem_board_file;
	FILE * msgid_file;

	int fifo_fd, str_len;
	char * my_msg;
	char * my_fifo;

	SO_BASE = atoi(getenv("SO_BASE"));
	SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_N_MOVES = atoi(getenv("SO_N_MOVES"));
	SO_MIN_HOLD_NSEC = atoi(getenv("SO_MIN_HOLD_NSEC"));

	sa.sa_handler = &handle_term;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGTERM, &sa, NULL);

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
					send_flg = 0;
					caught_points = move_to(target);
					if(caught_points > 0){
						msg_queue.mtype = (long) getppid();
						sprintf(msg_queue.mtext, "%d %d", CAUGHT, caught_points);
						send_flg = msgsnd(msg_id, &msg_queue, LENGTH, 0);
						caught_points = 0;

					}
					break;
				/*
					sends self coordinates (x,y)
				 */
				case POSITION:

					my_msg = malloc(sizeof(char) * BUF_SIZE);
					my_fifo = malloc(sizeof(char) * BUF_SIZE);

					sprintf(my_fifo,"fifo_%d\n",getppid());

					/* Open FIFO in write mode*/
					fifo_fd = open(my_fifo, O_WRONLY);

					/* Assemble the message*/
					str_len = sprintf(my_msg,"%d\n",self_x);

					/* Write message to FIFO*/
					write(fifo_fd, my_msg, str_len);

					str_len = sprintf(my_msg,"%d\n",self_y);
					write(fifo_fd, my_msg, str_len);

					close(fifo_fd);

					free(my_msg);
					free(my_fifo);

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
	int dist_x, dist_y;
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
		if(board[target_index].type != 'f') break;

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
		nanosleep(SO_MIN_HOLD_NSEC);
	}
	return 0;
}

/**
 * moves one step on y if possible
 * @param dir direction
 */
int step_y(int dir){
	int ret_val, index;

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
		nanosleep(SO_MIN_HOLD_NSEC);
	}
	return 0;
}

void handle_term(int signal){
	struct msgbuf msg_queue;

	if(send_flg == 0 && caught_points > 0){
		msg_queue.mtype = (long) getppid();
		sprintf(msg_queue.mtext, "%d %d", CAUGHT, caught_points);
		msgsnd(msg_id, &msg_queue, LENGTH, 0);
	}

	exit(SO_N_MOVES);
}
