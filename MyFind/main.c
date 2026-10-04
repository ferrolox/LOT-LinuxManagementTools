#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h> // Only used on Linux
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/syslimits.h> // Only used on MacOS

// Buffer size for pipe reads and output formatting.
#define BUFFER_SIZE (PATH_MAX + 256)

// Standard error output for usage errors.
static int exit_with_usage_error(char *argv[]) {
    fprintf(stderr, "Usage: %s [-R] [-i] searchpath filename1 [filename2 ...]\n", argv[0]);
    return EXIT_FAILURE;
}

static void send_result(int pipe_fd, const char *filename, const char *path) {
    char absolute_directory_path[PATH_MAX];

    // Resolve the absolute path of the directory entry.
    if (realpath(path, absolute_directory_path) == NULL) {
        perror("realpath");
        return;
    }

    // Format the output string with process ID, filename, and absolute path.
    char output[BUFFER_SIZE];
    int length = snprintf(output, sizeof(output), "%ld: %s: %s\n", (long)getpid(), filename, absolute_directory_path);

    // Check if the formatted output length exceeds the buffer size.
    if (length < 0 || (size_t) length >= sizeof(output)) {
        fprintf(stderr, "Result line is too long\n");
        return;
    }

    // Write the formatted output to the pipe.
    ssize_t written = write(pipe_fd, output, (size_t) length);

    if (written < 0) { perror("write"); }
}

static void search_directory(const char *directory_path, const char *filename, int recursive, int case_insensitive, int pipe_fd) {
    DIR *directory = opendir(directory_path);

    // Check if the directory can be accessed.
    if (directory == NULL) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", directory_path, strerror(errno));
        return;
    }

    struct dirent *directory_entry;

    // Iterate through the directory entries.
    while ((directory_entry = readdir(directory)) != NULL) {

        // Skip the current and parent directory entries to avoid infinite recursion.
        if (strcmp(directory_entry->d_name, ".") == 0 || strcmp(directory_entry->d_name, "..") == 0) {
            continue;
        }

        // Construct the full path of the directory entry.
        char full_path[PATH_MAX];
        const int length = snprintf(full_path, sizeof(full_path), "%s/%s", directory_path, directory_entry->d_name);

        // Check if the constructed path length exceeds the buffer size.
        if (length < 0 || (size_t) length >= sizeof(full_path)) {
            fprintf(stderr, "Path is too long: %s/%s\n", directory_path, directory_entry->d_name);
            continue;
        }

        int matches;

        // Compare the directory entry name with the target filename, considering case sensitivity.
        if (case_insensitive) { matches = strcasecmp(directory_entry->d_name, filename) == 0; }
        else { matches = strcmp(directory_entry->d_name, filename) == 0; }

        // If a match is found, send the result through the pipe.
        if (matches) { send_result(pipe_fd, filename, full_path); }

        // If recursive search is enabled, check if the directory entry is a directory and recursively search it.
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

static void prepare_search(const char *search_path, const char *filename, int recursive, int case_insensitive, int pipe_fd) {
    struct stat directory_properties;

    // Check if the search path is accessible.
    if (lstat(search_path, &directory_properties) == -1) {
        fprintf(stderr, "Cannot access '%s': %s\n", search_path, strerror(errno));
        close(pipe_fd);
        exit(EXIT_FAILURE);
    }

    // Check if the search path is a valid directory. If it's not, check if the search path is a file and if it matches the filename.
    if (!S_ISDIR(directory_properties.st_mode)) {
        const char *basename = strrchr(search_path, '/');

        if (basename == NULL) { basename = search_path; }
        else { basename++; }

        int matches;

        if (case_insensitive) { matches = strcasecmp(basename, filename) == 0; }
        else { matches = strcmp(basename, filename) == 0; }

        if (matches) { send_result(pipe_fd, filename, search_path); }

        close(pipe_fd);
        exit(EXIT_SUCCESS);
    }

    // Execute the directory search.
    search_directory(search_path, filename, recursive, case_insensitive, pipe_fd);

    close(pipe_fd);
    exit(EXIT_SUCCESS);
}

static void read_results(const int pipe_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    // Read from the pipe and write to stdout until there is no more data.
    while ((bytes_read = read(pipe_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;

        // Ensure that all bytes read from the pipe are written to stdout, handling partial writes.
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

    // Extract command-line options using getopt.
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

    // Check if there are at least two non-option arguments (search path and at least one filename)
    if (argc - optind < 2) { exit_with_usage_error(argv); }

    // Get search path and the number of filenames to search for.
    const char *search_path = argv[optind];
    const int filename_count = argc - optind - 1;

    int pipe_fd[2];

    // Create pipe with read and write file descriptors.
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // Allocate memory for child process IDs.
    pid_t *child_pids = malloc((size_t) filename_count * sizeof(pid_t));

    // Exit gracefully if memory allocation fails.
    if (child_pids == NULL) {
        perror("malloc");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXIT_FAILURE;
    }

    int children_created = 0;

    // Fork a child process for each filename and prepare search.
    for (int i = 0; i < filename_count; i++) {
        const char *filename = argv[optind + 1 + i];
        const pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) {
            close(pipe_fd[0]); // Close the read end of the pipe in the child process.

            prepare_search(search_path, filename, recursive, case_insensitive, pipe_fd[1]);

            exit(EXIT_FAILURE);
        }

        // Store child process ID for termination and cleanup (zoombie prevention).
        child_pids[children_created] = pid;
        children_created++;
    }

    // Close the write end of the pipe in the parent.
    close(pipe_fd[1]);

    // Read child process results from the pipe and output them to stdout, then close the read end.
    read_results(pipe_fd[0]);
    close(pipe_fd[0]);

    // Wait for all child processes to terminate and free memory allocated for child process IDs.
    for (int i = 0; i < children_created; i++) {
        int status;

        if (waitpid(child_pids[i], &status, 0) == -1) { perror("waitpid"); }
    }

    free(child_pids);

    return EXIT_SUCCESS;
}