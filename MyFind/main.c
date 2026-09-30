/*
 * myfind.c
 *
 * Each filename is searched by its own child process. This allows the
 * searches for different filenames to run concurrently.
 *
 * To synchronize output, child processes do not write directly to stdout.
 * Instead, every child writes complete result lines to a pipe. The parent
 * process reads these complete lines from the pipe and writes them to stdout.
 * This ensures that output from different child processes cannot be
 * interleaved within a result line.
 */

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/syslimits.h>

#define BUFFER_SIZE (PATH_MAX + 256)

static int exit_with_usage_error(char *argv[]) {
    fprintf(stderr, "Usage: %s [-R] [-i] searchpath filename1 [filename2 ...]\n", argv[0]);
    return EXIT_FAILURE;
}

static void send_result(int pipe_fd, const char *filename, const char *path) {
    char absolute_directory_path[PATH_MAX];

    if (realpath(path, absolute_directory_path) == NULL) {
        perror("realpath");
        return;
    }

    char output[BUFFER_SIZE];
    int length = snprintf(output, sizeof(output), "%ld: %s: %s\n", (long)getpid(), filename, absolute_directory_path);

    if (length < 0 || (size_t) length >= sizeof(output)) {
        fprintf(stderr, "Result line is too long\n");
        return;
    }

    ssize_t written = write(pipe_fd, output, (size_t) length);

    if (written < 0) { perror("write"); }
}

static void search_directory(const char *directory_path, const char *filename, int recursive, int case_insensitive, int pipe_fd) {
    DIR *directory = opendir(directory_path);

    if (directory == NULL) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", directory_path, strerror(errno));
        return;
    }

    struct dirent *directory_entry;

    while ((directory_entry = readdir(directory)) != NULL) {
        if (strcmp(directory_entry->d_name, ".") == 0 || strcmp(directory_entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];

        const int length = snprintf(full_path, sizeof(full_path), "%s/%s", directory_path, directory_entry->d_name);

        if (length < 0 || (size_t) length >= sizeof(full_path)) {
            fprintf(stderr, "Path is too long: %s/%s\n", directory_path, directory_entry->d_name);
            continue;
        }

        int matches;

        if (case_insensitive) { matches = strcasecmp(directory_entry->d_name, filename) == 0; }
        else { matches = strcmp(directory_entry->d_name, filename) == 0; }

        if (matches) { send_result(pipe_fd, filename, full_path); }

        if (recursive) {
            struct stat directory_properties;

            if (lstat(full_path, &directory_properties) == -1) {
                fprintf(stderr, "Failed to inspect directory '%s' with error: %s\n", full_path, strerror(errno));
                continue;
            }

            if (S_ISDIR(directory_properties.st_mode)) {
                search_directory(full_path, filename, recursive, case_insensitive, pipe_fd);
            }
        }
    }

    if (closedir(directory) == -1) { perror("closedir"); }
}

static void child_search(const char *search_path, const char *filename, int recursive, int case_insensitive,
                         int pipe_fd) {
    struct stat directory_properties;

    if (lstat(search_path, &directory_properties) == -1) {
        fprintf(stderr, "Cannot access '%s': %s\n", search_path, strerror(errno));
        close(pipe_fd);
        exit(EXIT_FAILURE);
    }

    if (!S_ISDIR(directory_properties.st_mode)) {
        const char *basename = strrchr(search_path, '/');

        if (basename == NULL) { basename = search_path; }
        else { basename++; }

        int matches;

        if (case_insensitive) { matches = (strcasecmp(basename, filename) == 0); }
        else { matches = (strcmp(basename, filename) == 0); }

        if (matches) { send_result(pipe_fd, filename, search_path); }

        close(pipe_fd);
        exit(EXIT_SUCCESS);
    }

    search_directory(search_path, filename, recursive, case_insensitive, pipe_fd);

    close(pipe_fd);
    exit(EXIT_SUCCESS);
}

static void read_results(const int pipe_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;

        while (total_written < bytes_read) {
            const ssize_t written = write(STDOUT_FILENO, buffer + total_written, (size_t) (bytes_read - total_written));

            if (written < 0) {
                perror("write");
                return;
            }

            total_written += written;
        }
    }

    if (bytes_read < 0) { perror("read"); }
}

int main(const int argc, char *argv[]) {
    int recursive = 0;
    int case_insensitive = 0;

    int option;

    while ((option = getopt(argc, argv, "Ri")) != -1) {
        switch (option) {
            case 'R':
                recursive = 1;
                break;

            case 'i':
                case_insensitive = 1;
                break;

            default:
                exit_with_usage_error(argv);
        }
    }

    /*
     * At least one search path and one filename are required.
     */
    if (argc - optind < 2) { exit_with_usage_error(argv); }

    const char *search_path = argv[optind];

    /*
     * Everything after the search path is a filename.
     */
    const int filename_count = argc - optind - 1;

    /*
     * Create one pipe shared by the parent and all children.
     *
     * pipe_fd[0] = read end
     * pipe_fd[1] = write end
     */
    int pipe_fd[2];

    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    /*
     * Store the child PIDs so that the parent can wait for every child.
     */
    pid_t *child_pids = malloc((size_t) filename_count * sizeof(pid_t));

    if (child_pids == NULL) {
        perror("malloc");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXIT_FAILURE;
    }

    int children_created = 0;

    /*
     * Create one child process for every filename.
     */
    for (int i = 0; i < filename_count; i++) {
        const char *filename = argv[optind + 1 + i];
        const pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) {
            close(pipe_fd[0]);

            child_search(search_path, filename, recursive, case_insensitive, pipe_fd[1]);

            exit(EXIT_FAILURE);
        }

        /*
         * PARENT
         *
         * Store the PID so we can wait for this child later.
         */
        child_pids[children_created] = pid;
        children_created++;
    }

    close(pipe_fd[1]);

    read_results(pipe_fd[0]);
    close(pipe_fd[0]);

    /*
     * Wait for every child to prevent zombie processes.
     */
    for (int i = 0; i < children_created; i++) {
        int status;

        if (waitpid(child_pids[i], &status, 0) == -1) { perror("waitpid"); }
    }

    free(child_pids);

    return EXIT_SUCCESS;
}