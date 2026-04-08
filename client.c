#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <stdbool.h>
#include <termios.h>

#define BUFFER_SIZE 4096
#define INDENT "          "

#define RESET "\033[0m"
#define RED "\033[31;1m"
#define GREEN "\033[32;1m"
#define YELLOW "\033[33;1m"
#define BLUE "\033[34;1m"
#define MAGENTA "\033[35;1m"
#define CYAN "\033[36;1m"
#define WHITE "\033[37;1m"

int sock_fd;
char current_input[BUFFER_SIZE] = "";

pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

void get_timestamp(char *buffer)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buffer, 20, "%H:%M:%S", &t);
}

void print_message(const char *msg, bool type_next_line)
{
    pthread_mutex_lock(&print_mutex);

    printf("\r\33[K");   // Clear the current line
    printf("%s\n", msg); // Print the incoming message

    // Reprint the prompt AND whatever the user was typing
    if (type_next_line)
    {
        printf(WHITE INDENT " [Type]: " RESET "%s", current_input);
    }
    fflush(stdout);

    pthread_mutex_unlock(&print_mutex);
}

// Receive thread (matches server line protocol)
void *receive_from_server(void *arg)
{
    char read_buf[BUFFER_SIZE];
    char line[BUFFER_SIZE];
    int buf_len = 0; // Total number of bytes received but not yet processed into lines

    while (true)
    {
        if (buf_len >= BUFFER_SIZE - 1)
            buf_len = 0; // reset buffer safely
        int n = read(sock_fd, read_buf + buf_len, BUFFER_SIZE - buf_len - 1);
        if (n > 0)
        {
            buf_len += n;
            read_buf[buf_len] = '\0';

            // Process complete lines
            for (int i = 0; i < buf_len; i++)
            {
                if (read_buf[i] == '\n')
                {
                    int len = i;
                    if (len >= BUFFER_SIZE)
                        len = BUFFER_SIZE - 1;

                    memcpy(line, read_buf, len);
                    line[len] = '\0';

                    // Printing the message with timestamp and color based on message type:
                    printf("\a"); // Beep on new message
                    char formatted[BUFFER_SIZE + 100];
                    char timestamp[20];
                    get_timestamp(timestamp);

                    if (strncmp(line, "[Broadcast", 10) == 0)
                        sprintf(formatted, "%s[%s] %s%s", CYAN, timestamp, line, RESET);
                    else if (strncmp(line, "[Direct Message", 15) == 0)
                        sprintf(formatted, "%s[%s] %s%s", MAGENTA, timestamp, line, RESET);
                    else if (strncmp(line, "[Server", 7) == 0)
                        sprintf(formatted, "%s[%s] %s%s", RED, timestamp, line, RESET);
                    else if (strncmp(line, INDENT, strlen(INDENT)) == 0)
                        sprintf(formatted, "%s[%s] %s%s", BLUE, timestamp, line, RESET);
                    else
                        sprintf(formatted, "[%s] %s", timestamp, line);
                    print_message(formatted, true);

                    // Shift the remaining unprocessed data
                    memmove(read_buf, read_buf + i + 1, buf_len - i - 1);

                    buf_len -= (i + 1);
                    i = -1; // restart scan
                }
            }
        }
        else if (n == 0)
        {
            print_message(RED "The server disconnected gracefully.\n" RESET, false);
            pthread_exit(NULL);
        }
        else
        {
            if (errno == EINTR)
                continue;
            print_message(RED "The server disconnected abruptly.\n" RESET, false);
            pthread_exit(NULL);
        }
    }
    pthread_exit(NULL);
}

// Send thread (matches server expectations) by writing in lines where each line ends with '\n'
void *send_to_server(void *arg)
{
    (void)arg;
    char buffer[BUFFER_SIZE];

    // Put terminal in raw mode
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (true)
    {
        int pos = 0;
        current_input[0] = '\0';
        memset(buffer, 0, sizeof(buffer));

        // Read character-by-character directly from the OS, bypassing libc entirely
        while (true)
        {
            char c;
            int n = read(STDIN_FILENO, &c, 1);
            if (n <= 0)
                break; // EOF or error

            if (c == '\n')
            {
                pthread_mutex_lock(&print_mutex);
                current_input[pos++] = '\n';
                current_input[pos] = '\0';
                printf("\n");
                pthread_mutex_unlock(&print_mutex);
                break;
            }
            else if (c == 127 || c == '\b') // Backspace
            {
                if (pos > 0)
                {
                    pthread_mutex_lock(&print_mutex);
                    pos--;
                    current_input[pos] = '\0';
                    printf("\b \b");
                    fflush(stdout);
                    pthread_mutex_unlock(&print_mutex);
                }
            }
            else if (pos < BUFFER_SIZE - 2 && c >= 32 && c <= 126) // Normal typing
            {
                pthread_mutex_lock(&print_mutex);
                current_input[pos++] = c;
                current_input[pos] = '\0';
                printf("%c", c);
                fflush(stdout);
                pthread_mutex_unlock(&print_mutex);
            }
        }

        if (pos == 0)
            break; // EOF

        strcpy(buffer, current_input);
        current_input[0] = '\0'; // Clear immediately so incoming messages don't print old text

        if (strcmp(buffer, "/exit\n") == 0)
            break;

        if (strcmp(buffer, "/clear\n") == 0)
        {
            pthread_mutex_lock(&print_mutex);
            printf("\033[2J\033[H");
            printf(WHITE INDENT " [Type]: " RESET);
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        int total_bytes = strlen(buffer);
        int total_bytes_sent = 0;
        while (total_bytes_sent < total_bytes)
        {
            int bytes_left = total_bytes - total_bytes_sent;
            int n = write(sock_fd, buffer + total_bytes_sent, bytes_left);

            if (n > 0)
                total_bytes_sent += n;
            else if (n == -1)
            {
                if (errno == EINTR)
                    continue;
                if (errno == ECONNRESET)
                    print_message(RED "The server disconnected abruptly.\n" RESET, false);
                if (errno == EPIPE)
                    print_message(RED "The server disconnected gracefully.\n" RESET, false);

                tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore terminal state on fail
                pthread_exit(NULL);
            }
        }

        buffer[strcspn(buffer, "\n")] = '\0';
        char timestamp[20];
        get_timestamp(timestamp);
        char formatted[BUFFER_SIZE + 200];

        if (strncmp(buffer, "/all ", 5) == 0)
            snprintf(formatted, sizeof(formatted), GREEN "[%s] [Your Broadcast]: %s" RESET, timestamp, buffer + 5);
        else if (strncmp(buffer, "/dm ", 4) == 0)
        {
            char temp[BUFFER_SIZE];
            strcpy(temp, buffer + 4);

            char *user = strtok(temp, " ");
            char *msg = strtok(NULL, "");

            if (user && msg)
                snprintf(formatted, sizeof(formatted), GREEN "[%s] [Your Direct Message to %s]: %s" RESET, timestamp, user, msg);
            else
                snprintf(formatted, sizeof(formatted), RED "[Invalid DM format]" RESET);
        }
        else
            snprintf(formatted, sizeof(formatted), GREEN "[%s] [You]: %s" RESET, timestamp, buffer);

        print_message(formatted, true);
    }

    // Restore terminal state before exiting gracefully
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    pthread_exit(NULL);
}
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "usage %s hostname port\n", argv[0]);
        exit(1);
    }

    // Socket creation:
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
        error("ERROR opening socket");

    // Resolve hostname:
    struct hostent *server = gethostbyname(argv[1]);
    if (server == NULL)
        error("ERROR, no such host");

    // Set up server address struct:
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    serv_addr.sin_port = htons(atoi(argv[2]));

    // Connect to server:
    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    printf(RED "Connected to the server %s:%s.\n" RESET, argv[1], argv[2]); // Does NOT need mutex as this is before threads start

    // Create threads for sending and receiving (since both can happen concurrently):
    pthread_t send_thread, recv_thread;
    pthread_create(&recv_thread, NULL, receive_from_server, NULL);
    pthread_create(&send_thread, NULL, send_to_server, NULL);

    // Terminating:
    pthread_join(send_thread, NULL);
    shutdown(sock_fd, SHUT_RDWR); // We reach here only when sending is done (either by "/exit" or error), so we shutdown the socket to unblock the receive thread if it's waiting on read()
    pthread_join(recv_thread, NULL);

    /*
        Reason behind shutdown() before joining the receive thread:
        When the user types "/exit", the send thread will exit, but the receive thread might still be blocked on read()
        waiting for messages from the server. By calling shutdown(sock_fd, SHUT_RDWR), we signal to the server
        that we won't send or receive any more data. This will cause the server to close its end of the connection,
        which in turn will cause the receive thread's read() to return 0,
        allowing it to exit. If we didn't call shutdown(), the receive thread might remain blocked
        indefinitely, preventing the program from terminating properly.
    */
    return 0;
}