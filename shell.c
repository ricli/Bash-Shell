#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int num_bprocesses;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);
int exec_func(struct tokens *tokens, char* redirect_check, int run_in_fg);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "prints current working directory"},
  {cmd_cd, "cd", "changes current working directory"},
  {cmd_wait, "wait", "waits for all processes in background to finish"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* prints current working directory contents */
int cmd_pwd(unused struct tokens *tokens) {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        fprintf(stdout, "%s\n", buf);
    } else {
        fprintf(stdout, "getcwd error: %s\n", strerror(errno));
    }
    return 1;
}

/* changes current working directory to specified path */
int cmd_cd(struct tokens *tokens) {
    size_t line_size = tokens_get_length(tokens);
    if (line_size == 1) {
        fprintf(stdout, "cd error: no arguments\n");
        return 0;
    }
    char* path = tokens_get_token(tokens, 1);
    if (chdir(path) == -1) {
        fprintf(stdout, "chdir error: %s\n", strerror(errno));
    }
    return 1;
}

/*waits for a proccesses in the background to finish*/
int cmd_wait(unused struct tokens *tokens) {
    while (num_bprocesses > 0) {
        wait(NULL);
        num_bprocesses--;
    }
    return 1;
}

/*executes the program specified in the command line with supplied arguments*/
int exec_func(struct tokens *tokens, char* redirect_check, int run_in_fg) {
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stdout, "fork error\n");
        return 0;
    }
    else if (pid == 0) {
        pid_t g_pid = getpid();
        setpgid(g_pid, g_pid);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGKILL, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCONT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        size_t arg_len = tokens_get_length(tokens);
        if (redirect_check != NULL && (strcmp(redirect_check, ">") == 0 || strcmp(redirect_check, "<") == 0)) {
            arg_len -= 2;
        }
        char* path = tokens_get_token(tokens, 0);
        char *argv[arg_len + 1];
        for (int i = 0; i < arg_len; i++) {
            char* arg = tokens_get_token(tokens, i);
            argv[i] = arg;
        }
        argv[arg_len] = NULL;

        char temp[2];
        strncpy(temp, path, 1);
        temp[1] = '\0';
        if (strcmp(temp, "/") == 0) {
            if (execv(path, argv) == -1) {
                fprintf(stdout, "execv error: %s\n", strerror(errno));
            }
        } else {
            char* env_path = getenv("PATH");
            char* tokens;
            tokens = strtok(env_path, ":");
            while (tokens != NULL) {
                char token_cpy[strlen(tokens)];
                strcpy(token_cpy, tokens);
                char temp2[strlen(token_cpy) + 1];
                strcpy(temp2, token_cpy);
                strcat(temp2, "/");
                char full_path[strlen(temp2) + strlen(path)];
                strcpy(full_path, temp2);
                strcat(full_path, path);
                execv(full_path, argv);
                tokens = strtok(NULL, ":");
            }
            fprintf(stdout, "execv error: %s\n", strerror(errno));
        }
    } else {
        if (run_in_fg) {
            tcsetpgrp(0, pid);
            wait(NULL);
            tcsetpgrp(0, getpid());
        } else {
            num_bprocesses++;
        }
    }
    return 1;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }

  num_bprocesses = 0;
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGKILL, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGCONT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    size_t line_size = tokens_get_length(tokens);
    char* redirect_check = tokens_get_token(tokens, line_size - 2);
    char* background_check = tokens_get_token(tokens, line_size - 1);
    int run_in_fg = strcmp(background_check, "&");
    int std_out = 1;
    int std_in = 0;
    if (redirect_check != NULL && strcmp(redirect_check, ">") == 0) {
        char* file_name = tokens_get_token(tokens, line_size - 1);
        FILE* file = fopen(file_name, "w");
        std_out = dup(1);
        dup2(fileno(file), 1);
    }
    if (redirect_check != NULL && strcmp(redirect_check, "<") == 0) {
        char* file_name = tokens_get_token(tokens, line_size - 1);
        FILE* file = fopen(file_name, "r");
        std_in = dup(0);
        dup2(fileno(file), 0);
    }

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      //fprintf(stdout, "This shell doesn't know how to run programs.\n");
      exec_func(tokens, redirect_check, run_in_fg);
    }

    dup2(std_out, 1);
    dup2(std_in, 0);

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
