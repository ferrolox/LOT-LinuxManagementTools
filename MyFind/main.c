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
    fprintf(stderr,"Usage: %s [-R] [-i] searchpath filename1 [filename2 ...]\n", argv[0]);
    return EXIT_FAILURE;
}

/*
 * Sends one complete result line to the parent through the pipe.
 */
static void send_result(int pipe_fd, const char *filename, const char *path)
{
    char absolute_path[PATH_MAX];
    char output[BUFFER_SIZE];

    if (realpath(path, absolute_path) == NULL) {
        perror("realpath");
        return;
    }

    int length = snprintf(output, sizeof(output), "%ld: %s: %s\n",
                          (long)getpid(), filename, absolute_path);

    if (length < 0 || (size_t)length >= sizeof(output)) {
        fprintf(stderr, "Result line is too long\n");
        return;
    }

    /*
     * A single write keeps the complete result together.
     */
    ssize_t written = write(pipe_fd, output, (size_t)length);

    if (written < 0) { perror("write"); }
}


/*
 * Recursively searches a directory for one filename.
 */
static void search_directory(const char *directory_path, const char *filename,
                             int recursive, int case_insensitive, int pipe_fd) {
    DIR *directory = opendir(directory_path);

    if (directory == NULL) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", directory_path, strerror(errno));
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {

        /*
         * "." refers to the current directory and ".." to its parent.
         * They must be skipped to prevent infinite recursion.
         */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];

        const int length = snprintf(full_path, sizeof(full_path), "%s/%s", directory_path, entry->d_name);

        if (length < 0 || (size_t)length >= sizeof(full_path)) {
            fprintf(stderr, "Path is too long: %s/%s\n", directory_path, entry->d_name);
            continue;
        }

        /*
         * Check whether this entry has the filename we're looking for.
         */
        int matches;

        if (case_insensitive) { matches = strcasecmp(entry->d_name, filename) == 0; }
        else { matches = strcmp(entry->d_name, filename) == 0; }

        if (matches) { send_result(pipe_fd, filename, full_path); }

        /*
         * If recursive mode is enabled, search inside directories.
         */
        if (recursive) {
            struct stat information;

            if (lstat(full_path, &information) == -1) {
                fprintf(stderr, "Cannot inspect '%s': %s\n", full_path, strerror(errno));
                continue;
            }

            if (S_ISDIR(information.st_mode)) {
                search_directory(full_path, filename, recursive,case_insensitive, pipe_fd);
            }
        }
    }

    if (closedir(directory) == -1) { perror("closedir"); }
}

/*
 * Searches for one filename.
 *
 * This function is executed by exactly one child process for each
 * filename supplied by the user.
 */
static void child_search(const char *search_path, const char *filename,
                         int recursive, int case_insensitive, int pipe_fd) {
    /*
     * If the search path itself is a file, compare it directly.
     * Normally the assignment expects a directory, but handling this
     * case makes the program more robust.
     */
    struct stat information;

    if (lstat(search_path, &information) == -1) {
        fprintf(stderr, "Cannot access '%s': %s\n", search_path, strerror(errno));
        close(pipe_fd);
        exit(EXIT_FAILURE);
    }

    if (!S_ISDIR(information.st_mode)) {
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


/*
 * Reads all data from the pipe and writes it to stdout.
 */
static void read_results(const int pipe_fd)
{
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd, buffer, sizeof(buffer))) > 0) {

        ssize_t total_written = 0;

        while (total_written < bytes_read) {
            const ssize_t written = write(STDOUT_FILENO, buffer + total_written, (size_t)(bytes_read - total_written));

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
    pid_t *child_pids = malloc((size_t)filename_count * sizeof(pid_t));

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

        if (waitpid(child_pids[i], &status, 0) == -1) {
            perror("waitpid");
        }
    }

    free(child_pids);

    return EXIT_SUCCESS;
}