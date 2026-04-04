#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define MAX_CLIENTS 100
#define QUEUE_SIZE 100
#define BUFFER_SIZE 4096
#define MAX_USERNAME_LEN 50

// Note: 1 write == 1 read is NOT true necessarily.
// read() may read only partial data and move on to the next line.

// write() slows when kernel buffer is full due to slow network, but read() slows because it waits for data to arrive from the network.
// read() blocks (sleeps) the current thread till data is unavailable, the other socket is NOT closed and NO error.

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

typedef struct
{
    bool active;
    int client_fd;
    char username[MAX_USERNAME_LEN];
    time_t join_time;
    int msg_count;
    char read_buf[BUFFER_SIZE];
    int buf_len;
} Client;

Client clients[MAX_CLIENTS];
time_t server_start_time;
pthread_mutex_t lock; // For synchronizing access to the clients[] array

// Must be called with the lock held
void reset_slot(Client *client)
{
    client->active = false;
    client->client_fd = -1;
    client->username[0] = '\0';
    client->buf_len = 0;
    client->join_time = 0;
    client->msg_count = 0;
    memset(client->read_buf, 0, sizeof(client->read_buf));
}

// Passes the entire (not partial) message
int send_to_client(int client_fd, const char *fmt, ...)
{
    char msg[BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int total_bytes_sent = 0, total_bytes = strlen(msg);
    while (total_bytes_sent < total_bytes)
    {
        int bytes_left = total_bytes - total_bytes_sent;
        int n = write(client_fd, msg + total_bytes_sent, bytes_left);

        if (n > 0)
            total_bytes_sent += n;
        else if (n == -1)
        {
            if (errno == EINTR) // interrupted (retry)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) // buffer full (rare in blocking mode)
                continue;
            if (errno == EPIPE || errno == ECONNRESET) // client gone (abrupt or graceful)
                return -1;
            return -1;
        }
    }
    return total_bytes_sent;
}

// Reads in lines where each line ends with '\n'
int receive_from_client(Client *client, char *out, int max_len)
{
    while (true)
    {
        // Check if we already have a complete line (ending with '\n') in the buffer:
        for (int i = 0; i < client->buf_len; i++)
        {
            if (client->read_buf[i] == '\n')
            {
                int len = i;
                if (len >= max_len)
                    len = max_len - 1;

                memcpy(out, client->read_buf, len);
                out[len] = '\0';

                // Shift remaining data
                memmove(client->read_buf, client->read_buf + i + 1, client->buf_len - i - 1);

                client->buf_len -= (i + 1);
                return len;
            }
        }

        if (client->buf_len >= BUFFER_SIZE - 1)
        {
            client->buf_len = 0;
            return -1;
        }

        // We don't have a full line yet, so we need to read more data from the socket:
        int n = read(client->client_fd, client->read_buf + client->buf_len, BUFFER_SIZE - client->buf_len - 1);
        if (n > 0)
        {
            client->buf_len += n;
            client->read_buf[client->buf_len] = '\0';
        }
        else if (n == 0) // client closed
            return 0;
        else
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue; // retry or temporary unavailability
            return -1;    // forceful disconnection or other error
        }
    }
}

void broadcast(int exclude_sock, const char *fmt, ...)
{
    char msg[BUFFER_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int fds[MAX_CLIENTS];

    // Copying client_fds to a local array to minimize lock holding time:
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active && clients[i].client_fd != exclude_sock)
            fds[i] = clients[i].client_fd;
        else
            fds[i] = -1;
    }
    pthread_mutex_unlock(&lock);

    // Send without lock:
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (fds[i] == -1)
            continue;

        int n = send_to_client(fds[i], "%s", msg);
        if (n < 0)
        {
            pthread_mutex_lock(&lock);
            if (clients[i].active && clients[i].client_fd == fds[i])
            {
                close(clients[i].client_fd);
                // DO NOT reset the slot here, because the receive_from_client() in the client thread may still be working on this slot,
                // thus causing race conditions.
            }
            pthread_mutex_unlock(&lock);
        }
    }
}

// Must hold lock before calling this function
int find_client_by_name(const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active && strcmp(clients[i].username, name) == 0)
            return i;
    }
    return -1;
}

void list_users(int client_fd)
{
    char buffer[BUFFER_SIZE];
    strcpy(buffer, "[Server]: Online users (case-sensitive):\n");

    /* Snapshot active usernames while holding the lock, then build
       the printable buffer after unlocking to avoid doing string ops
       or I/O while the mutex is held. */
    char names[MAX_CLIENTS][MAX_USERNAME_LEN];
    int count = 0;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENTS && count < MAX_CLIENTS; i++)
    {
        if (clients[i].active)
        {
            strncpy(names[count], clients[i].username, sizeof(names[count]));
            names[count][sizeof(names[count]) - 1] = '\0';
            count++;
        }
    }
    pthread_mutex_unlock(&lock);

    for (int i = 0; i < count; i++)
    {
        size_t len = strlen(buffer);
        time_t now = time(NULL);
        int seconds = now - clients[i].join_time;
        snprintf(buffer + len, BUFFER_SIZE - len, "          %s (online for %ds)\n", names[i], seconds);
    }

    send_to_client(client_fd, "%s", buffer);
}

bool valid_username(const char *username)
{
    if (username == NULL)
        return false;

    int len = strlen(username);

    if (len == 0 || len >= MAX_USERNAME_LEN)
        return false;

    for (int i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)username[i];
        // Allow only: a-z, A-Z, 0-9, _
        if (!isalnum(c) && c != '_')
            return false;
    }
    return true;
}

void *handle_client(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME_LEN];

    // Ask the username:
    // Since client hasn't registered yet, it does NOT have a client_index, so we use temporary client for buffering the username input.
    Client temp_client;
    memset(&temp_client, 0, sizeof(temp_client));
    temp_client.client_fd = client_fd;
    while (true)
    {
        int n = send_to_client(client_fd, "[Server]: Enter your case-sensitive username (only alphabets, digits and underscores allowed)\n");
        if (n < 0)
        {
            close(client_fd);
            pthread_exit(NULL);
        }

        n = receive_from_client(&temp_client, username, 50);
        if (n <= 0)
        {
            close(client_fd);
            pthread_exit(NULL);
        }

        if (!valid_username(username))
        {
            send_to_client(client_fd, "[Server]: Invalid username! Try another username with only alphabets, digits and underscores.\n");
            continue;
        }

        // If no other client has the same username.
        pthread_mutex_lock(&lock);
        int existing_index = find_client_by_name(username);
        pthread_mutex_unlock(&lock);
        if (existing_index == -1)
            break;

        send_to_client(client_fd, "[Server]: Username already taken! Try another username.\n");
        list_users(client_fd);
    }

    // Server capacity check and client registration:
    pthread_mutex_lock(&lock);
    int client_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            client_index = i;
            break;
        }
    }

    if (client_index == -1)
    {
        pthread_mutex_unlock(&lock);
        send_to_client(client_fd, "[Server]: The Server is currently full. Try again later!\n");
        close(client_fd);
        pthread_exit(NULL);
    }

    clients[client_index].active = true;
    clients[client_index].client_fd = client_fd;
    strncpy(clients[client_index].username, username, MAX_USERNAME_LEN);
    clients[client_index].username[MAX_USERNAME_LEN - 1] = '\0';
    clients[client_index].join_time = time(NULL);
    pthread_mutex_unlock(&lock);

    // Joining the chat:
    send_to_client(client_fd, "[Server]: Welcome to the chat server!\n");
    broadcast(client_fd, "[Server]: %s joined the chat\n", username);

    // Messages handling loop:
    // Note: You don't need to check for return value of send_to_client() and broadcast() in this loop,
    // because if the client disconnects, the next read() will return 0 or -1 and break the loop.
    while (true)
    {
        // Receive from the client:
        int n = receive_from_client(&clients[client_index], buffer, BUFFER_SIZE);
        if (n == 0)
        {
            broadcast(client_fd, "[Server]: %s disconnected\n", username);
            break;
        }
        else if (n < 0)
        {
            broadcast(client_fd, "[Server]: Error receiving message from %s\n", username);
            break;
        }
        clients[client_index].msg_count++;

        // Commands handling:
        if (strcmp(buffer, "/exit") == 0)
        {
            broadcast(client_fd, "[Server]: %s exited from the chat\n", username);
            break;
        }
        else if (strcmp(buffer, "/whoami") == 0)
        {
            send_to_client(client_fd, "[Server]: You are %s\n", username);
        }
        else if (strcmp(buffer, "/uptime") == 0)
        {
            time_t current_time = time(NULL);
            int elapsed = (int)(current_time - server_start_time);
            int days = elapsed / 86400;
            elapsed %= 86400;
            int hours = elapsed / 3600;
            elapsed %= 3600;
            int minutes = elapsed / 60;
            int seconds = elapsed % 60;
            if (days > 0)
                send_to_client(client_fd, "[Server]: The server's uptime is %d days %d hours %d minutes %d seconds\n", days, hours, minutes, seconds);
            else if (hours > 0)
                send_to_client(client_fd, "[Server]: The server's uptime is %d hours %d minutes %d seconds\n", hours, minutes, seconds);
            else if (minutes > 0)
                send_to_client(client_fd, "[Server]: The server's uptime is %d minutes %d seconds\n", minutes, seconds);
            else
                send_to_client(client_fd, "[Server]: The server's uptime is %d seconds\n", seconds);
        }
        else if (strcmp(buffer, "/time") == 0)
        {
            time_t now = time(NULL);
            char t[26];
            ctime_r(&now, t);
            t[strcspn(t, "\n")] = '\0';
            send_to_client(client_fd, "[Server]: Current server time is %s\n", t);
        }
        else if (strcmp(buffer, "/since") == 0)
        {
            time_t now = time(NULL);
            int seconds = now - clients[client_index].join_time;
            send_to_client(client_fd, "[Server]: You joined %d seconds ago\n", seconds);
        }
        else if (strcmp(buffer, "/users") == 0)
        {
            list_users(client_fd);
        }
        else if (strcmp(buffer, "/stats") == 0)
        {
            int count = clients[client_index].msg_count;
            send_to_client(client_fd, "[Server]: You sent %d messages\n", count);
        }
        else if (strncmp(buffer, "/help", 5) == 0)
        {
            send_to_client(client_fd, "[Server]: Available Commands:\n"
                                      "          /help                         : display this help message\n"
                                      "          /users                        : show all the online users\n"
                                      "          /dm <username> <message>      : private message\n"
                                      "          /all <message>                : broadcast\n"
                                      "          /whoami                       : display your username\n"
                                      "          /rename <new_username>        : change your username\n"
                                      "          /stats                        : display your message statistics\n"
                                      "          /uptime                       : display server uptime\n"
                                      "          /time                         : display current server time\n"
                                      "          /since                        : display how long you have been connected\n"
                                      "          /clear                        : clear the screen (client-side only)\n"
                                      "          /exit                         : exit\n");
        }
        else if (strncmp(buffer, "/rename ", 8) == 0)
        {
            char new_name[MAX_USERNAME_LEN];
            sscanf(buffer + 8, "%49s", new_name);

            if (!valid_username(new_name))
            {
                send_to_client(client_fd, "[Server]: Invalid username! (Only alphabets, digits and underscores are allowed)\n");
                continue;
            }

            pthread_mutex_lock(&lock);
            int existing_index = find_client_by_name(new_name);
            if (existing_index != -1)
            {
                pthread_mutex_unlock(&lock);
                send_to_client(client_fd, "[Server]: Username already taken!\n");
                continue;
            }

            char old_name[MAX_USERNAME_LEN];
            strcpy(old_name, clients[client_index].username);
            strcpy(clients[client_index].username, new_name);
            strcpy(username, new_name); // Update local variable for /whoami, /exit, other errors, etc.
            pthread_mutex_unlock(&lock);

            broadcast(client_fd, "[Server]: %s renamed to %s\n", old_name, new_name);
        }
        else if (strcmp(buffer, "/all") == 0)
        {
            send_to_client(client_fd, "[Server]: Usage: /all <message>\n");
        }
        else if (strncmp(buffer, "/all ", 5) == 0)
        {
            broadcast(client_fd, "[Broadcast from %s]: %s\n", username, buffer + 5);
        }
        else if (strcmp(buffer, "/dm") == 0)
        {
            send_to_client(client_fd, "[Server]: Usage: /dm <username> <message>\n");
        }
        else if (strncmp(buffer, "/dm ", 4) == 0)
        {
            char target_username[MAX_USERNAME_LEN], msg[BUFFER_SIZE];
            int args = sscanf(buffer + 4, "%49s %4095[^\n]", target_username, msg);
            if (args != 2)
            {
                send_to_client(client_fd, "[Server]: Usage: /dm <username> <message>\n");
                continue;
            }
            if (strcmp(target_username, username) == 0)
            {
                send_to_client(client_fd, "[Server]: You cannot send a direct message to yourself!\n");
                continue;
            }

            // Find the target client and send the message:
            pthread_mutex_lock(&lock);
            int target_index = find_client_by_name(target_username);
            if (target_index != -1)
            {
                int target_fd = clients[target_index].client_fd;
                pthread_mutex_unlock(&lock);
                send_to_client(target_fd, "[Direct Message from %s]: %s\n", username, msg);
            }
            else
            {
                pthread_mutex_unlock(&lock);
                send_to_client(client_fd, "[Server]: User %s not found\n", target_username);
            }
        }
        else
            send_to_client(client_fd, "[Server]: Invalid command!\n");
    }

    // Remove  the client:
    pthread_mutex_lock(&lock);
    reset_slot(&clients[client_index]);
    pthread_mutex_unlock(&lock);

    // Closing the client socket and exiting the thread:
    close(client_fd);
    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }

    // Initializations:
    server_start_time = time(NULL);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent server crash when writing to a closed socket
    pthread_mutex_init(&lock, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++)
        reset_slot(&clients[i]);
    struct sockaddr_in serv_addr, cli_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(atoi(argv[1]));

    // Listening Socket Creation:
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        error("ERROR opening socket");
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Binding the Listening Socket to the Port and all Interfaces:
    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    // Setting up the Listening Socket:
    printf("Server started. Listening on port %d...\n", atoi(argv[1]));
    listen(listen_fd, QUEUE_SIZE);

    // Loop of accept() and creating new threads:
    while (true)
    {
        socklen_t clilen = sizeof(cli_addr);
        int *client_fd = malloc(sizeof(int));
        if (!client_fd)
            error("malloc failed");
        *client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &clilen);

        if (*client_fd < 0)
            error("ERROR on accept");

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, client_fd);
        pthread_detach(tid);
    }

    // Closing the Listening Socket:
    close(listen_fd);
    return 0;
}