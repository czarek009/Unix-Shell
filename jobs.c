#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
  while ((pid = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED | WCONTINUED)) >
         0) {
    for (int i = 0; i < njobmax; i++) {
      for (int j = 0; j < jobs[i].nproc; j++) {

        if (pid == jobs[i].proc[j].pid) {
          if (WIFEXITED(status)) {
            jobs[i].proc[j].exitcode = WEXITSTATUS(status);
            jobs[i].proc[j].state = FINISHED;
          } else if (WIFSIGNALED(status)) {
            jobs[i].proc[j].exitcode = WTERMSIG(status) + 128;
            jobs[i].proc[j].state = FINISHED;
          } else if (WIFSTOPPED(status)) {
            jobs[i].proc[j].exitcode = WSTOPSIG(status) + 128;
            jobs[i].proc[j].state = STOPPED;
          } else if (WIFCONTINUED(status)) {
            jobs[i].proc[j].state = RUNNING;
            jobs[i].state = RUNNING;
          }

          // jeżeli wszystkie procesy są FINISHED, job też jest FINISHED
          // jeżeli wszystkie procesy są STOPPED, job też jest STOPPED
          // jeżeli choć jeden proces jest RUNNING, job też jest RUNNING
          bool finished = true;
          bool stopped = true;
          for (int k = 0; k < jobs[i].nproc; k++) {
            if (jobs[i].proc[k].state == RUNNING) {
              jobs[i].state = RUNNING;
              finished = false;
              stopped = false;
              break;
            }
            if (jobs[i].proc[k].state == STOPPED) {
              finished = false;
            }
          }
          if (finished) {
            jobs[i].state = FINISHED;
          } else if (stopped) {
            jobs[i].state = STOPPED;
          }
          break;
        }
      }
    }
  }

  /* TODO END */
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
  if (state == FINISHED) {
    *statusp = exitcode(&jobs[j]);
    deljob(&jobs[j]);
  }
  /* TODO END*/

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  /* TODO: Continue stopped job. Possibly move job to foreground slot. */
  msg("%s[%d] continue '%s'\n%s", GREEN, j, jobcmd(j), DEFAULT);
  Kill(-jobs[j].pgid, SIGCONT);
  if (!bg) {
    movejob(j, FG);
    monitorjob(mask);
  }
  /* TODO END */

  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
  if (jobs[j].pgid == 0) {
    return false;
  }
  Kill(-jobs[j].pgid, SIGTERM);
  Kill(-jobs[j].pgid, SIGCONT);
  jobs[j].proc[jobs[j].nproc - 1].exitcode = 128 + SIGTERM;
  /* TODO END */

  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

    /* TODO: Report job number, state, command and exit code or signal. */
    int state = jobs[j].state;
    int extcd = exitcode(&jobs[j]);
    char *cmd = jobcmd(j);

    if (state == FINISHED) {
      if (extcd >= 128) {
        msg("%s[%d] killed '%s' by signal %d\n%s", RED, j, cmd, extcd - 128, DEFAULT);
      } else {
        msg("%s[%d] exited '%s', status=%d\n%s", RED, j, cmd, extcd, DEFAULT);
      }
      deljob(&jobs[j]);
      continue;
    }

    if (state == STOPPED && (which == -1 || which == STOPPED)) {
      msg("%s[%d] suspended '%s'\n%s", YELLOW, j, cmd, DEFAULT);
      continue;
    }

    if (state == RUNNING && (which == -1 || which == RUNNING)) {
      msg("%s[%d] running '%s'\n%s", GREEN, j, jobcmd(j), DEFAULT);
      continue;
    }
    /* TODO END */
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
  Tcsetpgrp(tty_fd, jobs[0].pgid);

  while ((state = jobstate(0, &exitcode)) == RUNNING) {
    Sigsuspend(mask);
  }
  if (state == STOPPED) {
    Tcgetattr(tty_fd, &jobs[0].tmodes);
    int new_slot = addjob(0, BG);
    movejob(0, new_slot);
    msg("%s[%d] suspended '%s'\n%s", YELLOW, new_slot, jobcmd(new_slot), DEFAULT);
  }
  if (state == STOPPED || state == FINISHED) {
    Tcsetattr(tty_fd, 0, &shell_tmodes);
    Tcsetpgrp(tty_fd, getpgrp());
  }
  /* TODO END */

  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  Signal(SIGCHLD, sigchld_handler);
  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
  for (int j = 0; j < njobmax; j++) {
    if (jobs[j].state != FINISHED && jobs[j].pgid != 0) {
      if (!killjob(j)) {
        kill(-jobs[j].pgid, SIGKILL);
        jobs[j].proc[jobs[j].nproc - 1].exitcode = SIGKILL + 128;
      }
      jobs[j].state = FINISHED;
    }
  }
  /* TODO END */

  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}
