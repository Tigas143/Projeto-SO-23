#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h> // Include for waitpid
#include "constants.h"
#include "operations.h"
#include "parser.h"
#include <pthread.h>

struct ThreadArgs {
    int input_file;
    int fd;
};

char *strremove(char *str, const char *sub) {
    size_t len = strlen(sub);
    if (len > 0) {
        char *p = str;
        while ((p = strstr(p, sub)) != NULL) {
            memmove(p, p + len, strlen(p + len) + 1);
        }
    }
    return str;
}

void *thread_function(void *args) {
    struct ThreadArgs *thread_args = (struct ThreadArgs *)args;
    int input_file = thread_args->input_file;
    int fd = thread_args->fd;

    unsigned int event_id, delay;
    size_t num_rows, num_columns, num_coords;
    size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];
    fflush(stdout);
    enum Command command_type;
    while ((command_type = get_next(input_file)) != EOC) {
      switch (command_type) {
        case CMD_CREATE:
          if (parse_create(input_file, &event_id, &num_rows, &num_columns) != 0) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
          }
          if (ems_create(event_id, num_rows, num_columns)) {
            fprintf(stderr, "Failed to create event\n");
          }

          break;

        case CMD_RESERVE:
          num_coords = parse_reserve(input_file, MAX_RESERVATION_SIZE, &event_id, xs, ys);

          if (num_coords == 0) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
          }

          if (ems_reserve(event_id, num_coords, xs, ys)) {
            fprintf(stderr, "Failed to reserve seats\n");
          }

          break;

        case CMD_SHOW:
          if (parse_show(input_file, &event_id) != 0) {
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
          }

          if (ems_show(event_id, fd)) {
            fprintf(stderr, "Failed to show event\n");
          }

          break;

        case CMD_LIST_EVENTS:
          if (ems_list_events(fd)) {
            fprintf(stderr, "Failed to list events\n");
          }

          break;
        case CMD_WAIT:
                    if (parse_wait(input_file, &delay, NULL) == -1) {  // thread_id is not implemented
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            continue;
          }

          if (delay > 0) {
            printf("Waiting...\n");
            ems_wait(delay);
          }
          break;

        case CMD_INVALID:
          fprintf(stderr, "SSSSInvalid command. See HELP for usage\n");
          break;

        case CMD_HELP:
          printf(
              "Available commands:\n"
              "  CREATE <event_id> <num_rows> <num_columns>\n"
              "  RESERVE <event_id> [(<x1>,<y1>) (<x2>,<y2>) ...]\n"
              "  SHOW <event_id>\n"
              "  LIST\n"
              "  WAIT <delay_ms> [thread_id]\n"  // thread_id is not implemented
              "  BARRIER\n"                      // Not implemented
              "  HELP\n");

          break;

        case CMD_BARRIER:  // Not implemented
        case CMD_EMPTY:
          break;

        case EOC:
          ems_terminate();
          return NULL;
      }
    }

    return NULL;
}

void process_job_file(const char *jobs_directory, const char *filename, int max_threads) {
    char file_path[4096];
    snprintf(file_path, 4096, "%s/%s", jobs_directory, filename);
    int input_file = open(file_path, O_RDONLY);
    if (input_file == -1) {
        perror("Error opening command file");
        return;
    }

    int fd = 0;
    if (strstr(filename, ".jobs") != NULL) {
        char nome[4096];
        sprintf(nome, "%s.out", filename);
        strremove(nome, ".jobs");
        fd = open(nome, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            fprintf(stderr, "open error: %s\n", strerror(errno));
            close(input_file);
            return;
        }
    } else {
        close(input_file);
        return;
    }

    // Create an array to store thread IDs
    pthread_t threads[max_threads];
    struct ThreadArgs *thread_args_array = malloc((size_t)max_threads * sizeof(struct ThreadArgs));

   for (int i = 0; i < max_threads; ++i) {
        thread_args_array[i].input_file = input_file;
        thread_args_array[i].fd = fd;

        if (pthread_create(&threads[i], NULL, thread_function, (void *)&thread_args_array[i]) != 0) {
            perror("Error creating thread");
            break;
        }
    }
    // Wait for all threads to finish
    for (int i = 0; i < max_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    // Close the output file
    close(fd);
    free(thread_args_array);
}


int main(int argc, char *argv[]) {
    unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
    const char *jobs_directory;
    jobs_directory = argv[1];
    if (argc >= 4) {

        // Check if there's an optional delay argument
        char *endptr;
        unsigned long int delay = strtoul(argv[4], &endptr, 10);

        if (*endptr == '\0' && delay <= UINT_MAX) {
            state_access_delay_ms = (unsigned int)delay;
        } else {
            fprintf(stderr, "Invalid delay value or value too large\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Usage: %s <jobs_directory> <max_processes> <max_threads> [delay]\n", argv[0]);
        return 1;
    }
    int max_threads = atoi(argv[3]);
    if (max_threads <= 0) {
        fprintf(stderr, "Invalid value for maximum processes or threads\n");
        return 1;
    }
    int max_processes = atoi(argv[2]);
    if (max_processes <= 0) {
        fprintf(stderr, "Invalid value for maximum processes\n");
        return 1;
    }
    // No need to redeclare max_processes here

    if (ems_init(state_access_delay_ms)) {
        fprintf(stderr, "Failed to initialize EMS\n");
        return 1;
    }

   DIR *dir = opendir(jobs_directory);
    if (!dir) {
        perror("Error opening JOBS directory");
        return 1;
    }

    struct dirent *entry;
    int active_processes = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (active_processes >= max_processes) {
            // Wait for any child process to finish
            wait(NULL);
            active_processes--;
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("Error forking process");
            return 1;
        } else if (pid == 0) {
            // Child process
            process_job_file(jobs_directory,entry->d_name, max_threads);
            exit(0);
        } else {
            // Parent process
            active_processes++;
        }
    }

    // Wait for all remaining child processes to finish
    while (active_processes > 0) {
        wait(NULL);
        active_processes--;
    }

    closedir(dir);
    ems_terminate();
    return 0;
}