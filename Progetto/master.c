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
#include <time.h>

#define TEST_ERROR    if (errno) {fprintf(stderr, \
					   "[MASTER_ERROR] %s:%d: PID=%5d: Error %d (%s)\n",\
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
enum cmd{START_ROUND = 0, END_ROUND = 1, START_MOVING = 3, CAUGHT = 4,
	ALRM = 5 };

/*
	structs
 */
struct piece {
	char type;
	int value  ;
	pid_t owner;
	int x,y;
};

typedef struct Player {
	pid_t pid;
	int score, moves;
}Player;

typedef struct Flag {
	int position, points;
	char available;
}Flag;

/*
	function prototypes
 */
void fill_empty();
void print_board();
void print_status();
void print_statistics();
void ipcrm();
void update_flag(int flag_position);
void handle_alarm(int signal);
int get_player_index(pid_t player_pid);

/*
	environment variables
 */
extern char **environ;

/*
	global variables
 */
struct sembuf sem_board;
struct piece *board;

Player *player;
Flag *self_flags;

int num_flags;
int shm_id, sem_board_id, msg_id, prt_msg_id;
int SO_BASE, SO_ALTEZZA, SO_NUM_P, SO_NUM_G,
	SO_FLAG_MIN, SO_FLAG_MAX, SO_ROUND_SCORE,
	SO_MAX_TIME, SO_N_MOVES;

static int num_round = 1, tot_moves;

char *args[] = {"./player", NULL};

time_t time_start, time_end;

int main(int argc, char * argv[], char** env){

	int i, j, r, points, cmd, caught_pos;
    long rcv;
    pid_t child_pid, player_pid;

	char *line; /* line to write in ipcs.ini */
	char *split_msg; /* msg to split in rcv() */

    struct sigaction sa;
	sigset_t my_mask;

    struct msgbuf msg_queue;
	struct msgbuf prt_msg_queue;

	FILE * ini_file;

	key_t msg_key;
	key_t prt_msg_key;

    SO_FLAG_MIN = atoi(getenv("SO_FLAG_MIN"));
	SO_FLAG_MAX = atoi(getenv("SO_FLAG_MAX"));
	SO_ROUND_SCORE = atoi(getenv("SO_ROUND_SCORE"));
	SO_ALTEZZA = atoi(getenv("SO_ALTEZZA"));
	SO_BASE = atoi(getenv("SO_BASE"));
	SO_NUM_P = atoi(getenv("SO_NUM_P"));
	SO_NUM_G = atoi(getenv("SO_NUM_G"));
	SO_MAX_TIME = atoi(getenv("SO_MAX_TIME"));
	SO_N_MOVES = atoi(getenv("SO_N_MOVES"));

    tot_moves = SO_NUM_P * SO_N_MOVES;
    player = (Player*)malloc(sizeof(Player)*SO_NUM_G);
	line = malloc(sizeof(char)*100);

	/*
		sigaction init
	*/
    sa.sa_handler = &handle_alarm;
	sa.sa_flags = 0;
	sigemptyset(&my_mask);
	sa.sa_mask = my_mask;
	sigaction(SIGALRM, &sa, NULL);

	/*
		semaphore init
	*/
    printf("[MASTER] Setting semaphore...\n");
    sem_board_id = semget(IPC_PRIVATE, 1 + SO_BASE*SO_ALTEZZA + 1, 0600);
	semctl(sem_board_id, SO_BASE*SO_ALTEZZA, SETVAL, 0);
	semctl(sem_board_id, SO_BASE*SO_ALTEZZA + 1, SETVAL, 0);
	sem_board.sem_num = SO_BASE*SO_ALTEZZA;
	sem_board.sem_flg = 0;

	/*
		writes semaphore id in ipcs.ini
	*/
	ini_file = fopen(FILENAME_INI, "w");
	sprintf(line, "%d\n", sem_board_id);
	fputs(line, ini_file);

	/*
		shared memory init
	*/
    printf("[MASTER] Setting shared memory...\n");
    shm_id = shmget(IPC_PRIVATE, sizeof(struct piece)*SO_BASE*SO_ALTEZZA,
					IPC_CREAT | IPC_EXCL | 0600);
    board = (struct piece*) shmat(shm_id, NULL, 0);

	/*
		writes shared memory id in ipcs.ini
	*/
	sprintf(line, "%d\n", shm_id);
	fputs(line, ini_file);

	/*
		primary message queue init
	*/
    printf("[MASTER] Setting messages queues...\n");
    for (msg_key = IPC_PRIVATE+1; msg_key != IPC_PRIVATE; msg_key++) {

		msg_id = msgget(msg_key, 0600 | IPC_CREAT | IPC_EXCL);
		if (msg_id == -1)  {
			if (errno == EEXIST) {
				fprintf(stderr,	"[MASTER_ERROR] Key %d already exsists.\n",
					msg_key);
				continue;
			}
			if (errno == EACCES) {
				continue;
			}
			if (errno == ENOMEM || errno == ENOSPC) {
				fprintf(stderr,	"[MASTER_ERROR] %s\n", strerror(errno));
				return(-1);
			}
		}

		/*
			writes primary message key in ipcs.ini
		*/
		sprintf(line, "%d\n",msg_key);
		fputs(line, ini_file);
		break;
	}
	if (msg_key == IPC_PRIVATE) {
		fprintf(stderr,	"[MASTER_ERROR] Any free keys!\n");
		return(-1);
	}

	/*
		priority message queue init
	*/
	for (prt_msg_key = IPC_PRIVATE+1; prt_msg_key != IPC_PRIVATE; prt_msg_key++) {

		prt_msg_id = msgget(prt_msg_key, 0600 | IPC_CREAT | IPC_EXCL);
		if (prt_msg_id == -1)  {
			if (errno == EEXIST) {
				fprintf(stderr,	"[MASTER_ERROR] Key %d already exsists.\n",
					prt_msg_key);
				continue;
			}
			if (errno == EACCES) {
				continue;
			}
			if (errno == ENOMEM || errno == ENOSPC) {
				fprintf(stderr,	"[MASTER_ERROR] %s\n", strerror(errno));
				return(-1);
			}
		}

		/*
			writes priority message key in ipcs.ini
		*/
		sprintf(line, "%d\n", prt_msg_key);
		fputs(line, ini_file);
		break;
	}
	if (prt_msg_key == IPC_PRIVATE) {
		fprintf(stderr,	"[MASTER_ERROR] Any free keys!\n");
		return(-1);
	}

	/*
		Closes ini_file and frees line allocation
	*/
	fclose(ini_file);
	free(line);

    fill_empty();
	printf("[MASTER] Hi i'm Master PID: %d.\n", getpid());
	printf("[MASTER] That's the board.\n");
	printf("------------------------------------------\n");
	print_board();

	time_start = time(NULL);

    /*
		generates players, saves players pid, init score
	 */
    for(i = 0; i < SO_NUM_G; i++){
		switch(child_pid = fork()){
			case -1:
				exit(EXIT_FAILURE);
			case 0:
				execve(args[0], args, env);
				break;
			default:
				break;
		}
		player[i].pid = child_pid;
        player[i].score = 0;
		player[i].moves = tot_moves;
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
	printf("------------------------------------------\n");
	printf("[MASTER] Players placed all their pawns.\n");
	printf("[MASTER] Board updated.\n");
	printf("------------------------------------------\n");
	print_board();

	/*
		Waits every process init
	*/
    sem_board.sem_op = -SO_NUM_G*SO_NUM_P;
	sem_board.sem_num = SO_BASE*SO_ALTEZZA + 1;
	semop(sem_board_id, &sem_board, 1);

	/*
		Round
	*/
    for(;;){

		/*
			Init flags
		*/
		srand(getpid());
		num_flags = SO_FLAG_MIN + rand() % ((SO_FLAG_MAX+1) - SO_FLAG_MIN);
		points = SO_ROUND_SCORE;

		self_flags = (Flag*)malloc(sizeof(Flag)*num_flags);

		/*
			places flags with random points and saves all flags position
		 */
		for(i = num_flags, j = 0; i > 0; i--, j++){
			do{
				r = rand() % (SO_BASE * SO_ALTEZZA);
			}while(board[r].type != 'e');
			board[r].type = 'f';
			self_flags[j].position = r;
			self_flags[j].available = 'Y';
			if(points > i) board[r].value = 1 + rand()% (points-i+1);
			else if (points == i) board[r].value = 1;
			self_flags[j].points = board[r].value;
			points -= (board[r].value);
		}
		printf("[MASTER] %d flags have been placed.\n", num_flags);
		printf("[MASTER] Board updated.\n");
		printf("------------------------------------------\n");
		print_status();
	  	printf("\n");

		alarm(SO_MAX_TIME);

		/*
			Sends all players START_ROUND msg and #flags
		 */
		printf("[MASTER] Round #%d started\n", num_round);
		printf("------------------------------------------\n");
		for(i = 0; i < SO_NUM_G; i++){
			/*
				tells the player the round is started
			*/
			msg_queue.mtype = (long)(player[i].pid);
			sprintf(msg_queue.mtext, "%d", START_ROUND);
			msgsnd(msg_id, &msg_queue, LENGTH, 0);

			/*
				tells the player # flags
			*/
			msg_queue.mtype = (long)(player[i].pid);
			sprintf(msg_queue.mtext, "%d", num_flags);
			msgsnd(msg_id, &msg_queue, LENGTH, 0);

			/*
				tells the player all flags' position
			*/
			for(j = 0; j < num_flags; j++){
				msg_queue.mtype = (long)(player[i].pid);
				sprintf(msg_queue.mtext, "%d", self_flags[j].position);
				msgsnd(msg_id, &msg_queue, LENGTH, 0);
			}
		}

		/*
			waits for all players to give directions to their pawns
		 */
		rcv = (long) getpid();
		for(i = 0; i < SO_NUM_G; i++){
			msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0);
	   	}

		printf("[MASTER] All players have given instruction.\n");

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
		j = num_flags;
		while(j > 0){
			if(msgrcv(msg_id, &msg_queue, LENGTH, rcv, 0)){
				split_msg = strtok (msg_queue.mtext," ");
				cmd = atoi(split_msg);
				if(cmd == CAUGHT){
					j--;
					split_msg = strtok (NULL, " ");
					player_pid = (pid_t) atoi(split_msg);
					split_msg = strtok (NULL, " ");
					points =  atoi(split_msg);
					split_msg = strtok (NULL, " ");
					caught_pos =  atoi(split_msg);

					/*
						updates the leaderboard
					*/
					player[get_player_index(player_pid)].score += points;

					update_flag(caught_pos);

					printf("[MASTER] Player %d caught a %d points flag with position %d.\n", player_pid, points, caught_pos);
					printf("------------------------------------------\n");

					/*
						sends all player a caught flag notification
					 */
					 for(i = 0; i < SO_NUM_G; i++){
						msg_queue.mtype = (long)(player[i].pid);
	 					sprintf(msg_queue.mtext, "%d", ALRM);
	 					msgsnd(msg_id, &msg_queue, LENGTH, 0);
	 			   	}
				}
			}
		}

		/*
			sends all players END_ROUND msg
		 */
		for(i = 0; i < SO_NUM_G; i++){
			msg_queue.mtype = (long)(player[i].pid);
			sprintf(msg_queue.mtext, "%d", END_ROUND);
			msgsnd(msg_id, &msg_queue, LENGTH, 0);
		}

		/*
			Gets current moves left from players and updates the leaderboard
		*/
		for(i = 0; i < SO_NUM_G; i++){
			msgrcv(prt_msg_id, &prt_msg_queue, LENGTH, rcv, 0);
			split_msg = strtok (prt_msg_queue.mtext," ");
			player_pid = (pid_t) atoi(split_msg);
			split_msg = strtok (NULL, " ");
			player[get_player_index(player_pid)].moves = atoi(split_msg);
		}

		printf("[MASTER] Round #%d ended.\n", num_round);
		printf("[MASTER] Board updated.\n");
		printf("------------------------------------------\n");
		print_status();

		/*
			frees current self_flags allocation
		*/
		free(self_flags);
		self_flags = NULL;

		num_round++;
	}
	/*
		in case for loop breaks removes ipcs and exit failure
	*/
	ipcrm();
    exit(EXIT_FAILURE);
}

/**
 * sets all pieces of the board to empty
 */
void fill_empty(){
	int i,j;

	for(i = 0; i < SO_ALTEZZA; i++){
        for(j = 0; j < SO_BASE; j++){
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

	for(a = 0; a < SO_ALTEZZA; a++){
        for(b = 0; b < SO_BASE; b++){
        	if(board[a*SO_BASE + b].type == 'e'){
        		printf("#");
        	} else if(board[a*SO_BASE + b].type == 'p'){
				switch(get_player_index(board[a*SO_BASE + b].owner)){
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
    pid_t child_pid, player_pid;
    int i, status, position, points;

    time_end = time(NULL);

	/*
		sends SIGTERM to all players
	*/
    for(i = 0; i < SO_NUM_G; i++){
		kill(player[i].pid, SIGTERM);
	}

	/*
		Catches unsended flags and updates the leaderboad
	*/
	if(self_flags != NULL){
		for(i = 0; i < num_flags; i++){
			position = self_flags[i].position;
			if(self_flags[i].available == 'Y' && board[position].type != 'f'){
				player_pid = board[position].owner;
				points = self_flags[i].points;
				player[get_player_index(player_pid)].score += points;
				printf("[MASTER] Player %d caught a %d points flag.\n", player_pid, points);
				printf("------------------------------------------\n");

			}
		}
	}

    /*
		waits for all players to terminate their pawns
	 */
    i = 0;
    while ((child_pid = waitpid(player[i].pid, &status, 0)) != -1) {
		if (WIFEXITED(status)) {
			/*
				gets player left moves from exit status
			*/
			player[i].moves = WEXITSTATUS(status);
            i++;
 		}else{
            printf("[MASTER] player exit by signal %d\n", 128 + WTERMSIG(status));
        }
	}
	if (errno != ECHILD) {
		fprintf(stderr, "[MASTER_ERROR] Error #%d: %s\n", errno, strerror(errno));
		ipcrm();
		exit(EXIT_FAILURE);
	}

	print_status();
	printf("[MASTER] All players logged out.\n");
	print_statistics();
    ipcrm();

	/*
		frees player allocation and self_flags if it can
	*/
    free(player);
	if(self_flags != NULL) free(self_flags);

    printf("[MASTER] Bye bye.\n");
	printf("------------------------------------------\n");
	_exit(EXIT_SUCCESS);
}

/**
 * gets the index of a player
 * @param  player_pid: player to find index
 * @return            index if playes in players[], -1 otherwise
 */
int get_player_index(pid_t player_pid){
	int i;

	for(i = 0; i < SO_NUM_G; i++){
		if(player[i].pid == player_pid) return i;
	}
	return -1;
}

/**
 * updates a flag status
 * @param  flag_position: flag to find index
 * @return            index if playes in players[], -1 otherwise
 */
void update_flag(int flag_position){
	int i;

	for(i = 0; i < num_flags; i++){
		if(self_flags[i].position == flag_position)  self_flags[i].available = 'N';
	}
}

/**
 * prints the game's status
 */
void print_status(){
	int i;

	print_board();
	for(i=0; i<SO_NUM_G; i++){
		printf("[MASTER] Player -> %d Score -> %d Moves -> %d\n", player[i].pid, player[i].score, player[i].moves);
	}
	printf("------------------------------------------\n");
}

/**
 * prints the final statistics at the end of the simulation
 */
void print_statistics(){
	int i, used_moves, tot_points = 0, time_played;

	printf("[MASTER] Statistics:\n");
	printf("---\n");
	printf("[MASTER] #%d round played.\n", num_round);
	printf("---\n");
	for(i = 0; i < SO_NUM_G; i++){
		used_moves = tot_moves - player[i].moves;
		printf("[MASTER] Player -> %d:\n", player[i].pid);
		printf("\t-player used moves %d;\n", used_moves);
		printf("\t-total moves %d;\n", tot_moves);
		printf("\t-used_moves/tot_moves %f.\n", used_moves/(float)tot_moves);
	}
	printf("---\n");
	for(i = 0; i < SO_NUM_G; i++){
		used_moves = tot_moves - player[i].moves;
		tot_points += player[i].score;
		printf("[MASTER] Player -> %d:\n", player[i].pid);
		printf("\t-gained points %d;\n", player[i].score);
		printf("\t-player used moves %d;\n", used_moves);
		if(used_moves) printf("\t-points/used_moves %f.\n", player[i].score/(float)used_moves);
		else printf("\t- can't print points/used_moves cause used_moves is Zero.\n");
	}
	printf("---\n");
	printf("[MASTER] Last stat:\n");
	printf("\t-players total points %d;\n", tot_points);
	time_played = time_end - time_start;
	printf("\t-time played %ds.\n", time_played);
	printf("\t-tot_points/time_played %f.\n", tot_points/(float)time_played);
	printf("---\n");
}

/**
 * removes all ipcs
 */
void ipcrm(){

	printf("[MASTER] Removing semaphore...\n");
    semctl(sem_board_id, 0, IPC_RMID);

    printf("[MASTER] Removing shared memory...\n");
    shmctl(shm_id, IPC_RMID, NULL);

    printf("[MASTER] Removing messages queues...\n");
    msgctl(msg_id, IPC_RMID, NULL);
    msgctl(prt_msg_id, IPC_RMID, NULL);
}
