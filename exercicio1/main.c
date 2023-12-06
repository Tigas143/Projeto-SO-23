#include <limits.h>

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "constants.h"
#include "operations.h"
#include "parser.h"
#define O_RDONLY 00

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


int main(int argc, char *argv[]) {
  const char *jobs_directory;
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;

  if (argc >= 2) {
    jobs_directory = argv[1];
    
    // Check if there's an optional delay argument
  if (argc > 2) {
    char *endptr;
    unsigned long int delay = strtoul(argv[2], &endptr, 10);

    if (*endptr == '\0' && delay <= UINT_MAX) {
      state_access_delay_ms = (unsigned int)delay;
    } else {
      fprintf(stderr, "Invalid delay value or value too large\n");
      return 1;
    }
  }
  } else {
    fprintf(stderr, "Usage: %s <jobs_directory> [delay]\n", argv[0]);
    return 1;
  }

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

  while ((entry = readdir(dir)) != NULL) {
    char file_path[4096];
    snprintf(file_path, 4096, "%s/%s", jobs_directory, entry->d_name);
    int input_file = open(file_path, O_RDONLY);
    if (input_file == -1) {
      perror("Error opening command file");
      continue;  // Or you can exit the program, depending on requirements
    }
    int fd = 0;
    if(strstr(entry->d_name, ".jobs") != NULL){
      char nome[4096];
      sprintf(nome, "%s.out", entry->d_name);
      strremove(nome, ".jobs");
      fd = open(nome, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
      if (fd < 0){
        fprintf(stderr, "open error: %s\n", strerror(errno));
        return -1;
      }
    }
    else{
      continue;
    }
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
        if (ems_list_events()) {
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
        return 0;
    }
    }
    close(input_file);
    close(fd);
  }
  
  closedir(dir);
}