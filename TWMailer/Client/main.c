#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define USER_MAX 8
#define SUBJECT_MAX 80

static int read_input(const char *prompt, char **buffer, size_t *capacity) {
    printf("%s", prompt);
    fflush(stdout);

    ssize_t length = getline(buffer, capacity, stdin);

    if (length < 0) return feof(stdin) ? 0 : -1;
    if (length > 0 && (*buffer)[length - 1] == '\n') (*buffer)[--length] = '\0';
    if (length > 0 && (*buffer)[length - 1] == '\r') (*buffer)[--length] = '\0';

    return 1;
}

static int valid_user(const char *username) {
    size_t length = strlen(username);

    if (length == 0 || length > USER_MAX) return 0;

    for (size_t i = 0; i < length; i++) {
        if (!((username[i] >= 'a' && username[i] <= 'z') || (username[i] >= '0' && username[i] <= '9'))) return 0;
    }

    return 1;
}

static int read_username(const char *prompt, char **username, size_t *capacity) {
    int result = read_input(prompt, username, capacity);

    if (result != 1) return result;
    if (!valid_user(*username)) {
        fprintf(stderr, "Invalid username. Use 1-8 lowercase letters or digits.\n");
        return -1;
    }

    return 1;
}

static int read_message_number(int *number) {
    char *input = NULL;
    size_t capacity = 0;

    int result = read_input("Message number: ", &input, &capacity);

    if (result != 1) {
        free(input);
        return result;
    }

    errno = 0;
    char *end;
    long value = strtol(input, &end, 10);

    if (errno != 0 || end == input || *end != '\0' || value <= 0 || value > INT_MAX) {
        fprintf(stderr, "Invalid message number.\n");
        free(input);
        return -1;
    }

    *number = (int)value;
    free(input);

    return 1;
}

/* TCP may transmit only part of a buffer in one send() call. */
static int send_all(int socket_fd, const char *buffer, size_t length) {
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(socket_fd, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);

        if (sent < 0 && errno == EINTR) continue;

        if (sent <= 0) {
            perror("send");
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

static int send_line(int socket_fd, const char *line) {
    if (send_all(socket_fd, line, strlen(line)) == -1) return -1;

    return send_all(socket_fd, "\n", 1);
}

/* Read one byte at a time so that a line read cannot consume bytes belonging to the next protocol field. */
static int read_line(int socket_fd, char **buffer, size_t *capacity) {
    size_t length = 0;

    if (*buffer == NULL || *capacity == 0) {
        *capacity = 128;
        *buffer = malloc(*capacity);

        if (*buffer == NULL) {
            perror("malloc");
            return -1;
        }
    }

    while (1) {
        char character;
        ssize_t received = recv(socket_fd, &character, 1, 0);

        if (received < 0 && errno == EINTR) continue;
        if (received < 0) {
            perror("recv");
            return -1;
        }

        if (received == 0) {
            if (length == 0) return 0;

            fprintf(stderr, "Incomplete response from server.\n");
            return -1;
        }

        if (character == '\n') {
            (*buffer)[length] = '\0';

            if (length > 0 && (*buffer)[length - 1] == '\r') (*buffer)[--length] = '\0';

            return 1;
        }

        if (length + 1 >= *capacity) {
            if (*capacity > SIZE_MAX / 2) {
                fprintf(stderr, "Response line is too large.\n");
                return -1;
            }

            size_t new_capacity = *capacity * 2;
            char *new_buffer = realloc(*buffer, new_capacity);

            if (new_buffer == NULL) {
                perror("realloc");
                return -1;
            }

            *buffer = new_buffer;
            *capacity = new_capacity;
        }

        (*buffer)[length++] = character;
    }
}

static int receive_line(int socket_fd, char **buffer, size_t *capacity) {
    int result = read_line(socket_fd, buffer, capacity);

    if (result == 0) {
        fprintf(stderr, "Server disconnected.\n");
        return -1;
    }

    return result;
}

static int receive_status(int socket_fd) {
    char *response = NULL;
    size_t capacity = 0;

    int result = receive_line(socket_fd, &response, &capacity);

    if (result == 1) {
        if (strcmp(response, "OK") == 0) printf("Operation successful.\n");
        else if (strcmp(response, "ERR") == 0) printf("Operation failed.\n");
        else {
            fprintf(stderr, "Unexpected server response: %s\n", response);
            result = -1;
        }
    }

    free(response);
    return result;
}

static int handle_send(int socket_fd) {
    char *sender = NULL;
    char *receiver = NULL;
    char *subject = NULL;

    size_t sender_capacity = 0;
    size_t receiver_capacity = 0;
    size_t subject_capacity = 0;

    int result;

    result = read_username("Sender: ", &sender, &sender_capacity);
    if (result != 1) goto cleanup;

    result = read_username("Receiver: ", &receiver, &receiver_capacity);
    if (result != 1) goto cleanup;

    result = read_input("Subject: ", &subject, &subject_capacity);
    if (result != 1) goto cleanup;

    if (strlen(subject) > SUBJECT_MAX) {
        fprintf(stderr, "Subject must not exceed 80 characters.\n");
        result = -1;
        goto cleanup;
    }

    if (send_line(socket_fd, "SEND") == -1 || send_line(socket_fd, sender) == -1 ||
        send_line(socket_fd, receiver) == -1 || send_line(socket_fd, subject) == -1) {
        result = -1;
        goto cleanup;
    }

    printf("Enter your message. Enter a single '.' to finish:\n");

    char *line = NULL;
    size_t line_capacity = 0;

    while (1) {
        result = read_input("", &line, &line_capacity);

        if (result != 1) {
            free(line);
            goto cleanup;
        }

        if (strcmp(line, ".") == 0) break;

        if (send_line(socket_fd, line) == -1) {
            free(line);
            result = -1;
            goto cleanup;
        }
    }

    free(line);

    if (send_line(socket_fd, ".") == -1) {
        result = -1;
        goto cleanup;
    }

    result = receive_status(socket_fd);

    cleanup:
    free(sender);
    free(receiver);
    free(subject);

    return result;
}

static int handle_list(int socket_fd) {
    char *username = NULL;
    size_t capacity = 0;

    int result = read_username("Username: ", &username, &capacity);
    if (result != 1) {
        free(username);
        return result;
    }

    if (send_line(socket_fd, "LIST") == -1 || send_line(socket_fd, username) == -1) {
        free(username);
        return -1;
    }

    free(username);

    char *response = NULL;
    size_t response_capacity = 0;

    result = receive_line(socket_fd, &response, &response_capacity);
    if (result != 1) {
        free(response);
        return -1;
    }

    errno = 0;
    char *end;
    long count = strtol(response, &end, 10);

    if (errno != 0 || end == response || *end != '\0' || count < 0) {
        fprintf(stderr, "Invalid LIST response from server.\n");
        free(response);
        return -1;
    }

    free(response);

    if (count == 0) {
        printf("Inbox is empty or user does not exist.\n");
        return 0;
    }

    printf("\nInbox (%ld messages):\n\n", count);

    for (long i = 1; i <= count; i++) {
        result = receive_line(socket_fd, &response, &response_capacity);

        if (result != 1) {
            free(response);
            return -1;
        }

        printf("%ld. %s\n", i, response);
    }

    printf("\n");

    free(response);
    return 0;
}

static int handle_read(int socket_fd) {
    char *username = NULL;
    size_t capacity = 0;

    int result = read_username("Username: ", &username, &capacity);
    if (result != 1) {
        free(username);
        return result;
    }

    int message_number;
    result = read_message_number(&message_number);

    if (result != 1) {
        free(username);
        return result;
    }

    char number_buffer[32];
    snprintf(number_buffer, sizeof(number_buffer), "%d", message_number);

    if (send_line(socket_fd, "READ") == -1 || send_line(socket_fd, username) == -1 ||
        send_line(socket_fd, number_buffer) == -1) {
        free(username);
        return -1;
    }

    free(username);

    char *response = NULL;
    size_t response_capacity = 0;

    result = receive_line(socket_fd, &response, &response_capacity);
    if (result != 1) {
        free(response);
        return -1;
    }

    if (strcmp(response, "ERR") == 0) {
        printf("Message could not be found.\n");
        free(response);
        return 0;
    }

    if (strcmp(response, "OK") != 0) {
        fprintf(stderr, "Unexpected server response: %s\n", response);
        free(response);
        return -1;
    }

    free(response);

    printf("\n---------- MESSAGE ----------\n");

    /* The stored message ends with the dot line written by the server. */
    while (1) {
        result = receive_line(socket_fd, &response, &response_capacity);

        if (result != 1) {
            free(response);
            return -1;
        }

        if (strcmp(response, ".") == 0) break;

        printf("%s\n", response);
    }

    printf("-----------------------------\n");

    free(response);
    return 0;
}

static int handle_delete(int socket_fd) {
    char *username = NULL;
    size_t capacity = 0;

    int result = read_username("Username: ", &username, &capacity);
    if (result != 1) {
        free(username);
        return result;
    }

    int message_number;
    result = read_message_number(&message_number);

    if (result != 1) {
        free(username);
        return result;
    }

    char number_buffer[32];
    snprintf(number_buffer, sizeof(number_buffer), "%d", message_number);

    if (send_line(socket_fd, "DEL") == -1 || send_line(socket_fd, username) == -1 ||
        send_line(socket_fd, number_buffer) == -1) {
        free(username);
        return -1;
    }

    free(username);

    return receive_status(socket_fd);
}

static int connect_to_server(const char *ip, const char *port) {
    struct addrinfo hints;
    struct addrinfo *server_addresses;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int result = getaddrinfo(ip, port, &hints, &server_addresses);

    if (result != 0) {
        fprintf(stderr, "Invalid server address: %s\n", gai_strerror(result));
        return -1;
    }

    int socket_fd = -1;

    for (struct addrinfo *address = server_addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);

        if (socket_fd == -1) continue;
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;

        close(socket_fd);
        socket_fd = -1;
    }

    freeaddrinfo(server_addresses);

    if (socket_fd == -1) {
        fprintf(stderr, "Could not connect to server.\n");
        return -1;
    }

    return socket_fd;
}

static void command_loop(int socket_fd) {
    char *command = NULL;
    size_t capacity = 0;

    printf("\nTW-Mailer Client\n");
    printf("----------------\n");
    printf("Available commands:\n");
    printf("  SEND\n");
    printf("  LIST\n");
    printf("  READ\n");
    printf("  DEL\n");
    printf("  QUIT\n\n");

    while (1) {
        int result = read_input("Command: ", &command, &capacity);

        if (result == 0) {
            printf("\nInput closed. Disconnecting.\n");
            break;
        }

        if (result == -1) {
            perror("getline");
            break;
        }

        for (size_t i = 0; command[i] != '\0'; i++) command[i] = (char)toupper((unsigned char)command[i]);

        printf("\n");

        if (strcmp(command, "SEND") == 0) result = handle_send(socket_fd);
        else if (strcmp(command, "LIST") == 0) result = handle_list(socket_fd);
        else if (strcmp(command, "READ") == 0) result = handle_read(socket_fd);
        else if (strcmp(command, "DEL") == 0) result = handle_delete(socket_fd);
        else if (strcmp(command, "QUIT") == 0) {
            if (send_line(socket_fd, "QUIT") == -1) fprintf(stderr, "Could not send QUIT command.\n");

            printf("Disconnected.\n");
            break;
        } else {
            printf("Unknown command. Please enter SEND, LIST, READ, DEL or QUIT.\n");
            continue;
        }

        if (result < 0) {
            fprintf(stderr, "Communication error. Disconnecting.\n");
            break;
        }

        printf("\n");
    }

    free(command);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    errno = 0;
    char *end;
    long port = strtol(argv[2], &end, 10);

    if (errno != 0 || end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port number.\n");
        return EXIT_FAILURE;
    }

    int socket_fd = connect_to_server(argv[1], argv[2]);

    if (socket_fd == -1) return EXIT_FAILURE;

    printf("Connected to %s:%ld\n", argv[1], port);

    command_loop(socket_fd);

    close(socket_fd);

    return EXIT_SUCCESS;
}