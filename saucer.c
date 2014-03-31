/*
 * File:   saucer.c
 * Author: Artem Herasymchuk (herasymc)
 *
 * 
 * Portions of this software taken from kent.edu/grant macewan's
 * tanimate.c
 *
 * Portions of this software taken from CMPUT 379 material/examples
 * (used with permission):
 *
 * Copyright (c) 2008 Bob Beck <beck@obtuse.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 * 
 */

#include <curses.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "saucer.h"

#define ROCKET_INIT 10
#define ROCKET_REWARD 3
#define ROCKET_LIMIT 20
#define SAUCER_LINES 10
#define SAUCER_LIMIT 20
#define PLAYER_LIVES 30
#define	TUNIT 20000

typedef struct rocketset {
	/*
	 * Properties of rocket threads.
	 * Keeps track of whether the thread is running (alive),
	 * and its position.
	 */
	int running;	/* 0 - thread not running, 1 - thread running */
	int row;
	int col;
	int killed;
} rocketset;

typedef struct saucerset {
	/*
	 * Properties of saucer threads.
	 * Keeps track of whether the thread is running (alive),
	 * its position and its size.
	 * Size is defined by number of characters, i.e.
	 * <-> is size 3, <---> is size 5, etc. Minimum size is 3.
	 */
	int running;	/* 0 - thread not running, 1 - thread running */
	int row;
	int col;
	int size;
	int killed;
	int delay;
} saucerset;

/* Global variables, shared data, mutexes */
int g_score = 0;		/* player score */
int g_hscore;			/* player high score */
int g_esc = 0;			/* escaped saucers */
int g_rkt = ROCKET_INIT;	/* remaining rockets */
int g_arkt = 0;			/* alive rockets */
int g_asau = 0;			/* alive saucers */
int g_quit = 0;			/* quit flag for threads */
int g_level = 1;		/* current level */
rocketset set_rocket[ROCKET_LIMIT]; /* rocket properties */
saucerset set_saucer[SAUCER_LIMIT]; /* saucer properties */
pthread_mutex_t mx_g = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mx_c = PTHREAD_MUTEX_INITIALIZER; /* mutex for curses */

void setup() {
	/*
	 * Initial setup.
	 * Set up ncurses, seed the rng for picking rows/speeds at random,
	 * and display the game information for the first time.
	 * Fill the global variables with initial values.
	 */
	int i;

	srand(getpid());
	initscr();
	crmode();			/* ncurses setup */
	noecho();			/* ncurses setup */
	clear();			/* ncurses setup */
	draw_info();		

	pthread_mutex_lock(&mx_g);
	for (i = 0; i < ROCKET_LIMIT; ++i) {
		set_rocket[i].running = 0;
		set_rocket[i].row = 0;
		set_rocket[i].col = 0;
		set_rocket[i].killed = 0;
	}
	for (i = 0; i < SAUCER_LIMIT; ++i) {
		set_saucer[i].running = 0;
		set_saucer[i].row = 0;
		set_saucer[i].col = 0;
		set_saucer[i].size = 3;
		set_saucer[i].killed = 0;
	}
	pthread_mutex_unlock(&mx_g);
}

void kill_threads() {
	/*
	 * Kills all the currently running threads, except the main
	 * game threads (saucer launcher, keyboard, etc).
	 * Used to either restart a game or quit.
	 */
	int i;

	pthread_mutex_lock(&mx_g);
	for (i = 0; i < ROCKET_LIMIT; ++i) {
		set_rocket[i].killed = 1;
	}
	for (i = 0; i < SAUCER_LIMIT; ++i) {
		set_saucer[i].killed = 1;
	}
	pthread_mutex_unlock(&mx_g);
}

void restart() {
	/* Starts a new game without quitting. */
	kill_threads();
	pthread_mutex_lock(&mx_g);
	g_score = 0;
	g_esc = 0;
	g_rkt = ROCKET_INIT;
	g_arkt = 0;
	g_asau = 0;
	g_level = 1;
	pthread_mutex_unlock(&mx_g);
	pthread_mutex_lock(&mx_c);
	clear();
	pthread_mutex_unlock(&mx_c);
	draw_info();
}

void draw_obj(int type, int size, int row, int col) {
	/* Draws a (moving) object on the screen. */
	pthread_mutex_lock(&mx_c);
	if (type == 1) {
		/* rocket launcher */
		move(row, col - 1);
		addstr(" | ");
	} else if (type == 2) {
		/* rocket */
		move(row, col);
		addch('^');
		if (row != (LINES - 3)) {
			/*
			 * don't draw trailing whitespace on initial line
			 * as to not erase the launch site
			 */
			 move(row + 1, col);
			 addch(' ');
		}
	} else if (type == 3) {
		/* saucer */
		char s[10] = "<";
		int cur = col;
		int i;
		for (i = 1; i < (size - 1); ++i)
			strlcat(s, "-", sizeof(s));
		strlcat(s, ">", sizeof(s));
		if (col != 0) {
			move(row, col - 1);
			addch(' ');
		}
		i = 0;
		while (cur < COLS && i < size) {
			/* animate saucers flying off the screen piece by piece */
			move(row, cur);
			addch(s[i]);
			cur++;
			i++;
		}
	}
	move(LINES - 1, COLS - 1);
	refresh();
	pthread_mutex_unlock(&mx_c);
}

void draw_space(int type, int size, int row, int col) {
	/*
	 * Draws empty space on the screen (useful for dead objects).
	 * type indicates a horizontal length of white space (for saucers).
	 */
	pthread_mutex_lock(&mx_c);
	if (type) {
		move(row, col - 1);
		for (; size >= 0; --size) {
			addch(' ');
		}
	} else {
		move(row, col);
		addch(' ');
		move(row + 1, col);
		addch(' ');
	}
	move(LINES - 1, COLS - 1);
	refresh();
	pthread_mutex_unlock(&mx_c);
}

void draw_end() {
	/* Draws end game info. */
	pthread_mutex_lock(&mx_c);
	mvprintw((LINES / 2), (COLS / 2) - 5, "GAME OVER!");
	mvprintw((LINES / 2) + 1, (COLS / 2) - 4, "YOU LOSE");
	mvprintw((LINES / 2) + 4, (COLS / 2) - 17, "Press n for a new game or q to quit.");
	move(LINES - 1, COLS - 1);
	refresh();
	pthread_mutex_unlock(&mx_c);
}

void draw_info() {
	/* Draws useful info on the screen. */
	pthread_mutex_lock(&mx_c);
	mvprintw(LINES - 1, 0, "Score: %i | High Score: %i | Rockets Left: %i"
	    " | Lives Left: %i       ", g_score, g_hscore, g_rkt, PLAYER_LIVES - g_esc);
	move(LINES - 1, COLS - 1);
	refresh();
	pthread_mutex_unlock(&mx_c);
}

void reward(int s, int r) {
	/* Assigns rewards based on kill conditions. */
	pthread_mutex_lock(&mx_g);
	set_rocket[r].killed = 1;
	set_saucer[s].killed = 1;
	g_score++;
	if (g_score > g_hscore)
		g_hscore = g_score;
	/* increase rewards a bit with level and size */
	g_rkt += ROCKET_REWARD;
	g_rkt += (0.25 * g_level);
	if (set_saucer[s].size == 3) {
		/* 
		 * reward a kill of the smallest saucer with lives
		 * based on level
		 */
		g_esc -= g_level;
		if (g_esc < 0)
			g_esc = 0;
		g_rkt++;
	}
	if (set_saucer[s].size == 4)
		g_rkt++;
	pthread_mutex_unlock(&mx_g);
}

void check_cond() {
	/* check for collisions, level changes, etc... */
	int i, j, k;

	/* collision checks */
	for (i = 0; i < SAUCER_LIMIT; ++i) {
		/* loop through each saucer */
		for (j = 0; j < ROCKET_LIMIT; ++j) {
			/* check saucer against each rocket */
			if (set_rocket[j].running && set_saucer[i].running &&
			    !set_rocket[j].killed && !set_saucer[i].killed &&
			    set_rocket[j].row == set_saucer[i].row) {
			    	/* only check alive saucers/rockets for row */
				for (k = 0; k < set_saucer[i].size; ++k) {
					if (set_rocket[j].col ==
						(set_saucer[i].col + k)) {
						/*
						 * check collision (accouting
						 * for size), set kill flags,
						 * and the threads will exit
						 */
						reward(i, j);
						draw_info();
					}
				}
			}
		}
	}

	/* level increase check */
	if (((g_score / 10) + 1) > g_level) {
		pthread_mutex_lock(&mx_g);
		g_level++;
		pthread_mutex_unlock(&mx_g);
		draw_info();
	}

	/* endgame condition check */
	if ((g_rkt == 0 && g_arkt == 0) || g_esc >= PLAYER_LIVES) {
		draw_end();
		kill_threads();
	}

}

void *thr_saucer(void *arg) {
	saucerset *info = arg;

	while(1) {
		if (info->killed) {
			pthread_mutex_lock(&mx_g);
			g_asau--;
			info->running = 0;
			pthread_mutex_unlock(&mx_g);
			draw_space(1, info->size, info->row, info->col);
      			pthread_exit(NULL);
		}
		usleep((info->delay / g_level) * TUNIT);
		draw_obj(3, info->size, info->row, info->col);
		pthread_mutex_lock(&mx_g);
		info->col++;
		pthread_mutex_unlock(&mx_g);
		check_cond();
		if (info->col >= COLS) {
			draw_obj(3, info->size, info->row, info->col);
			pthread_mutex_lock(&mx_g);
			if (g_esc < PLAYER_LIVES)
				g_esc++;
			g_asau--;
			info->running = 0;
			pthread_mutex_unlock(&mx_g);
			check_cond();
			draw_info();
			pthread_exit(NULL);
		}
	}
}

void *thr_rocket(void *arg) {
	rocketset *info = arg;

	while(1) {
		if (info->killed) {
			pthread_mutex_lock(&mx_g);
			info->running = 0;
			g_arkt--;
			pthread_mutex_unlock(&mx_g);
			draw_space(0, 0, info->row, info->col);
      			pthread_exit(NULL);
		}
		usleep(TUNIT);
		draw_obj(2, 1, info->row, info->col);
		pthread_mutex_lock(&mx_g);
		info->row--;
		pthread_mutex_unlock(&mx_g);
		check_cond();
		if (info->row < 0) {
			draw_obj(2, 1, info->row, info->col);
			pthread_mutex_lock(&mx_g);
			info->running = 0;
			g_arkt--;
			pthread_mutex_unlock(&mx_g);
			check_cond();
			pthread_exit(NULL);
		}
	}
}

void *thr_launcher(void *arg) {
	pthread_t *saucers = (pthread_t*) arg;

	int i, rc;
	int csaucer = 0;

	while (1) {
		/* loop to launch new saucers at random intervals */
		usleep(((rand() % 10) * (400000 / g_level)));
		if (g_asau <= SAUCER_LIMIT) {
			pthread_mutex_lock(&mx_g);
			g_asau++;
			set_saucer[csaucer].running = 1;
			set_saucer[csaucer].killed = 0;
			set_saucer[csaucer].row = rand() % SAUCER_LINES;
			set_saucer[csaucer].col = 0;
			set_saucer[csaucer].size = 3 + (rand() % 5);
			set_saucer[csaucer].delay = 1 + (rand() % 15);
			pthread_mutex_unlock(&mx_g);
			rc = pthread_create(&saucers[csaucer], NULL, 
			    &thr_saucer, &set_saucer[csaucer]);
			if (rc) {
				fprintf(stderr,"error creating thread");
				kill_threads();
				endwin();
				exit(0);
			}
			csaucer = (csaucer + 1) % SAUCER_LIMIT;
			while (set_saucer[csaucer].running) {
				/*
				 * a saucer thread has ended but it was not
				 * the next order one, search for unused thread
				 */
				csaucer = (csaucer + 1) % SAUCER_LIMIT;
			}
		}
	}
}

void *thr_keyboard(void *arg) {
	pthread_t *rockets = (pthread_t*) arg;

	int c, i, rc;
	int lpos = COLS/2; /* start the launcher at the center */
	int crocket = 0;

	draw_obj(1, 1, LINES - 2, lpos);

	while (1) {
		/* main loop for keyboard thread */
		c = getch();
		if (c == 'Q' || c == 'q') {
			kill_threads();
			g_quit = 1;
		}
		if (c == ' ' && g_rkt > 0 && g_arkt <= ROCKET_LIMIT) {
			pthread_mutex_lock(&mx_g);
			g_rkt--;
			g_arkt++;
			set_rocket[crocket].running = 1;
			set_rocket[crocket].killed = 0;
			set_rocket[crocket].row = (LINES - 3);
			set_rocket[crocket].col = lpos;
			pthread_mutex_unlock(&mx_g);
			draw_info();
			rc = pthread_create(&rockets[crocket], NULL, 
			    &thr_rocket, &set_rocket[crocket]);
			if (rc) {
				fprintf(stderr,"error creating thread");
				kill_threads();
				endwin();
				exit(0);
			}
			crocket = (crocket + 1) % ROCKET_LIMIT;
			while (set_rocket[crocket].running) {
				/*
				 * a rocket thread has ended but it was not
				 * the next order one, search for unused thread
				 */
				crocket = (crocket + 1) % ROCKET_LIMIT;
			}
		}
		if (c == ',' && lpos > 0) {
			lpos--;
			draw_obj(1, 1, LINES - 2, lpos);
		}
		if (c == 'N' || c == 'n')
			restart();
			draw_obj(1, 1, LINES - 2, lpos);
		if (c == '.' && lpos < COLS-1) {
			lpos++;
			draw_obj(1, 1, LINES - 2, lpos);
		}
	}
}

int main() {
	pthread_t rockets[ROCKET_LIMIT];
	pthread_t saucers[SAUCER_LIMIT];
	pthread_t tk;
	pthread_t tl;
	FILE* f;
	int i, rc;

	/*
	 * read in high score. this code and the writing
	 * code ignores errors on purpose (see design)
	 */
	g_hscore = 0;
	f = fopen("score", "r");
	if (f != NULL) {
		fread(&g_hscore, sizeof(g_hscore), 1, f);
		fclose(f);
	}

	/* set up keyboard/saucer launcher threads to run the game */
	setup();
	rc = pthread_create(&tk, NULL, &thr_keyboard, &rockets);
	if (rc) {
		fprintf(stderr, "error creating thread");
		endwin();
		exit(0);
	}
	rc = pthread_create(&tl, NULL, &thr_launcher, &saucers);
	if (rc) {
		fprintf(stderr, "error creating thread");
		endwin();
		exit(0);
	}

	/* wait for quit condition */
	while (1) {
		if (g_quit) {
			/* clean up and exit */
			while (g_asau || g_arkt) {}; /* wait for thread cleanup */
			pthread_mutex_lock(&mx_g);
			pthread_mutex_lock(&mx_c);
   			pthread_cancel(tl);
			pthread_cancel(tk);
			f = fopen("score", "w");
			if (f != NULL) {
				fwrite(&g_hscore, sizeof(g_hscore), 1, f);
				fclose(f);
			}
			endwin();
			pthread_exit(NULL);
		}
	}
}
