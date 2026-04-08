# Multi-Threaded TCP Server and Client

## Author
Anurag Mishra ([@anuragmishra-creates](https://github.com/anuragmishra-creates))

--- 
## Project Overview
The system follows a thread-per-client architecture where each connection is handled independently while sharing synchronized access to global client state.

This project implements a robust, concurrent Client-Server chat application using TCP sockets in C. It utilizes POSIX threads (`pthreads`) to handle multiple simultaneous client connections and non-blocking I/O paradigms. The architecture supports real-time broadcasting, private messaging, dynamic user management, and seamless terminal-based interaction.

---

## 🛠️ Instructions to Compile and Run

### Prerequisites
* A Linux/POSIX-compliant environment.
* GCC compiler installed.

### Compilation
Open your terminal and compile the server and client codes.

```bash
gcc server.c -o server
gcc client.c -o client
```

### Execution

#### 1. Start the Server

Run the server executable and provide a port number of your choice (e.g., `40000`):

```bash
    ./server 40000
```

#### 2. Start the Clients

Open a new terminal window (or a different VM) for each client.  
Run the client executable with the server's IP address and port number.

```bash
    ./client localhost 40000
```

---

## 🌟 Core Functionalities (As per Lab Requirements)

- **Concurrent Client Handling:**  
  The server utilizes `pthread_create` to spawn a detached thread for every incoming client connection, allowing multiple clients to interact simultaneously without blocking the main server process.

- **Dynamic Join/Disconnect:**  
  Clients can join the chat dynamically. The server manages a dynamic array of active clients (up to 100) and automatically reclaims slots when clients disconnect (gracefully or abruptly).

- **Private Messaging:**  
  Two clients can chat privately via the server using private messaging logic.

- **Network Broadcasting:**  
  Clients can broadcast messages to all currently active users on the server using the `/all` command.

--- 

## 🚀 Advanced & Additional Functionalities

We went above and beyond the standard requirements to create a highly polished, production-like terminal chat experience.

### 1. Seamless Concurrent Terminal I/O (Bypassing Canonical Mode)

#### The Problem
By default, terminals operate in Canonical Mode, which buffers user input until the 'Enter' key is pressed. This creates severe UI conflicts in a multithreaded chat environment where incoming messages can asynchronously interrupt a user while they are typing.

Example: Suppose you are typing "/all Hello, world!". Before you can finish, and are only at the "Hello" stage, a message arrives from the server. Because the incoming message and your keyboard input share the same terminal screen (stdout), the output becomes garbled:

``` bash
          [Type]: /all Hello[10:45:02] [Direct Message from Eren_Jaeger]: This is my message!
          [Type]: 
```

Now no matter what we type, the past message "/all Hello" is STILL in the stdin buffer (though unknown to the client process) and will be sent to the server along with whatever we type now.

Case I:
Suppose we type "/all Hello, world!" again and press enter, but now the entire "/all Hello/all Hello, world!" is sent to the server.
This causes the message "Hello/all Hello, world!" to be broadcasted (the first "/all" is taken as command)!
Even if though we could just continue from what is fixed to be sent "/all Hello" by typing ", world!", the UI is broken and case II is even a bigger issue.

Case II:
Suppose we don't want the "/all Hello" to be first part of what we send to the server. We now cannot do anything about it as it is LOCKED in the stdin buffer (not visible to the client process) and backspace WILL NOT work anymore. The message interrupt breaks the backspace and we cannot delete what we wrote.

#### Our Requirement
We wanted the following as the solution when an incoming message arrives:
(1) Whatever the client types is removed from the screen.
(2) The incoming message is displayed (printed).
(3) The client's message is restored.

#### Our Solution:
We implemented low-level terminal control using `<termios.h>` (terminal control API of Linux):

(I) Disabled ICANON (Canonical Mode):

To ensure the restoration of the client's half typed message, we ensure that whatever the client types is stored live in the current_input[] buffer. Note: the contents of the current_input[] buffer are not yet passed to the server till an enter is received as input from the client.
Since we wanted to capture live characters, we disabled the default (canonical) mode and thus the terminal was using raw mode (reading character letter by letter).

Now, the client process could easily come to know what the client had typed before the incoming message arrived and store it in current_line[] buffer, which can be later restored.

The partial-written message is cleared using carriage return and deleting everything from there.
The incoming message is printed. 
The partial-written, client message is restored in the terminal for doing the modifications (continue from there, delete it, edit it, etc).
Once the client is done and presses enter, the current_line[] buffer's content is copied down to the main client buffer that contains the message to be sent in its entirety.

(II) Disabled ECHO Mode:

ICANON (Canonical mode) reads inputs byte-by-byte. When ICANON is off but ECHO is on, the terminal behaves differently with control characters.

If the ECHO mode is ON and the Backspace key is pressed, the OS might simply echo the literal control character representation (often displayed as ^? or ^H) instead of actually moving the cursor left and deleting the character on the screen.

By handling it manually, it is ensured that printf("\b \b") accurately (visually and really) deletes the character to match exactly what is done to the current_input buffer.

This ensures a smooth, uninterrupted typing experience.
---

### 2. Robust User Authentication & Session Management

#### Features

- **Unique Username Enforcement**
  - Case-sensitive
  - Alphanumeric + underscores allowed
  - Duplicate and invalid usernames rejected

- **Dynamic Renaming**
  - Command: `/rename`
  - Updates server registry in real-time
  - Broadcasts change to all users

- **Session Tracking**
  - Stores join timestamp (`time_t`)
  - Tracks total messages per user

---

### 3. Interactive Command-Line Interface (CLI)

#### Supported Commands

| Command | Description |
|--------|------------|
| `/help` | Displays help menu |
| `/users` | Lists active users + session duration |
| `/dm <username> <message>` | Sends private message |
| `/all <message>` | Broadcast message |
| `/whoami` | Shows current username |
| `/rename <new_username>`| Change current username |
| `/stats` | Displays total messages sent |
| `/uptime` | Shows server uptime (D:H:M:S) |
| `/time` | Returns server timestamp (`ctime_r`) |
| `/since` | Shows session duration |
| `/clear` | Clears terminal using ANSI codes |
| `/exit` | Graceful disconnect |

---

### 4. Advanced UI/UX Formatting

#### Features

- **Color-Coded Output (ANSI Escape Codes)**

  | Type | Color |
  |------|------|
  | Server Alerts | Red |
  | Broadcast Messages | Cyan |
  | Direct Messages | Magenta |
  | Menus | Blue |
  | Outgoing Messages | Green |

- **Audio Alerts**
  - Terminal bell (`\a`) on new messages

- **Real-Time Timestamps**
  - Format: `[HH:MM:SS]`
  - Uses `localtime_r` (thread-safe)

---

### 5. Resilient Error Handling

#### Stability Enhancements

- **SIGPIPE Immunity**
  - Server ignores `SIGPIPE`
  - Prevents crashes on broken sockets

- **Non-Blocking Safety**
  - Gracefully handles:
    - `EINTR`
    - `EAGAIN`
    - `EWOULDBLOCK`
  - Prevents I/O deadlocks and freezes

---

## 💡 Summary

This system is not just a basic chat application — it mimics an **advanced terminal messaging system** with:

- Smooth real-time interaction
- Robust fault tolerance
- Clean and responsive UI/UX

---

## 📸 Preview

### SS1: 
![](media/SS1.png)

### SS2:
![](media/SS2.png)

### SS3:
![](media/SS3.png)

### SS4:
![](media/SS4.png)

### SS5:
![](media/SS5.png)

---
