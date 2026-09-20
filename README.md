# Custom Unix Shell (C++)

A Unix-style command-line shell built from scratch in C++, without using any shell-writing libraries (no `readline`, no existing parser). It implements the core mechanics that real shells like `bash` and `zsh` are built on: process creation, program execution, I/O redirection, pipelines between processes, and signal handling.

This README is written to be read alongside `main.cpp` — it explains not just *what* each piece of code does, but *why* it has to work that way, so that someone unfamiliar with systems programming can follow the reasoning.

## Table of Contents

- [What This Shell Can Do](#what-this-shell-can-do)
- [Building and Running](#building-and-running)
- [The Big Picture: What a Shell Actually Is](#the-big-picture-what-a-shell-actually-is)
- [Code Walkthrough](#code-walkthrough)
  - [1. The Main Loop](#1-the-main-loop)
  - [2. Reading Input: `read_line`](#2-reading-input-read_line)
  - [3. Tokenizing: `tokenize`](#3-tokenizing-tokenize)
  - [4. Detecting End of Input (Ctrl+D)](#4-detecting-end-of-input-ctrld)
  - [5. Bridging to C: `to_argv`](#5-bridging-to-c-to_argv)
  - [6. Running a Program: `fork` and `execvp`](#6-running-a-program-fork-and-execvp)
  - [7. Builtins: `cd` and `exit`](#7-builtins-cd-and-exit)
  - [8. Output Redirection (`>`)](#8-output-redirection-)
  - [9. Input Redirection (`<`)](#9-input-redirection-)
  - [10. Pipelines (`|`)](#10-pipelines-)
  - [11. Signal Handling (Ctrl+C)](#11-signal-handling-ctrlc)
- [Known Limitations](#known-limitations)
- [Planned Next Steps](#planned-next-steps)

---

## What This Shell Can Do

```
$ ls -la
$ cd /tmp
$ ls -la > out.txt          # output redirection
$ cat < out.txt              # input redirection
$ cat main.cpp | grep int | wc -l   # multi-stage pipelines
$ sleep 100                  # Ctrl+C interrupts the command, not the shell
```

It supports: an interactive read-execute loop, the builtins `cd` and `exit`, output redirection (`>`), input redirection (`<`), pipelines of any length (`cmd1 | cmd2 | ... | cmdN`), and correct Ctrl+C behavior.

## Building and Running

```bash
clang++ -std=c++11 main.cpp -o shell
./shell
```

No external dependencies — only the C++ standard library and POSIX system headers (`<unistd.h>`, `<sys/wait.h>`, `<fcntl.h>`, `<csignal>`), which are available on macOS and Linux out of the box.

## The Big Picture: What a Shell Actually Is

A shell's entire job, at the lowest level, is a loop:

1. Print a prompt.
2. Read a line of text the user types.
3. Figure out what program(s) that line refers to, and with what arguments.
4. **Create a new process** and tell it to become that program.
5. Wait for it to finish.
6. Go back to step 1.

The one non-obvious part is step 4. A running program cannot simply "call" another program the way a function calls another function — the OS has to create an entirely new process. This shell uses three POSIX system calls to do that:

- **`fork()`** — clones the currently running process. After calling it, there are *two* identical processes running the same code, distinguished only by `fork()`'s return value (0 in the new "child" process, the child's process ID in the original "parent").
- **`execvp()`** — replaces the calling process's own code with a different program entirely. It never returns if it succeeds, because the process running it no longer exists as "this program" — it *became* the new one.
- **`waitpid()`** — makes a process pause until a specific other process (usually its child) finishes.

The pattern `fork()` → (in the child) `execvp()` → (in the parent) `waitpid()` shows up everywhere in this codebase. Once you recognize it, most of `main.cpp` is a variation on this same idea, with extra setup (redirection, pipes) inserted between the fork and the exec.

## Code Walkthrough

### 1. The Main Loop

```cpp
int main()
{
    signal(SIGINT, SIG_IGN);
    while (true)
    {
        print_prompt();
        vector<string> tokens = tokenize(read_line());
        ...
    }
}
```

Everything happens inside one infinite loop. Each iteration handles exactly one command the user types. The `signal(SIGINT, SIG_IGN)` line is explained in [section 11](#11-signal-handling-ctrlc) — for now, just know it makes the shell itself immune to Ctrl+C.

### 2. Reading Input: `read_line`

```cpp
string read_line()
{
    string full_command;
    getline(cin, full_command);
    return full_command;
}
```

`getline` reads an entire line of text, including spaces, into a string. This matters because `cin >> variable` (the more commonly taught way to read input) stops at the first whitespace — it couldn't read `ls -la` as one piece of text, only `ls`.

### 3. Tokenizing: `tokenize`

```cpp
vector<string> tokenize(string full_command)
{
    vector<string> tokens;
    stringstream ss(full_command);
    string token;
    while (getline(ss, token, ' '))
    {
        tokens.push_back(token);
    }
    return tokens;
}
```

A raw line like `"ls -la /tmp"` is just one long string — but `execvp` (used later) needs the command name and each argument as *separate* strings. This function splits the line on spaces using a `stringstream`, the same `getline` function as above but pointed at a string instead of the keyboard, with `' '` as the delimiter instead of the default newline. The result is a `vector<string>` like `{"ls", "-la", "/tmp"}`.

### 4. Detecting End of Input (Ctrl+D)

```cpp
if (cin.eof())
{
    cout << "No more commands" << endl;
    return -1;
}
```

Pressing Ctrl+D signals "end of input" to the terminal. `getline` doesn't throw an error for this or return a special string — it fails silently, leaving `cin` in a state you can check with `.eof()`. This is checked right after reading, so the shell exits cleanly instead of looping forever on empty reads.

Separately, an ordinary blank line (the user just pressing Enter) is checked right after:

```cpp
if (tokens.empty())
{
    continue;
}
```

This is a different case from Ctrl+D — the user is still there, they just typed nothing — so the shell simply reprints the prompt instead of exiting.

### 5. Bridging to C: `to_argv`

```cpp
vector<char*> to_argv(vector<string>& tokens)
{
    vector<char*> tokens_adapter;
    for (const string& token : tokens)
    {
        const char* c_string = token.c_str();
        tokens_adapter.push_back(const_cast<char*>(c_string));
    }
    tokens_adapter.push_back(nullptr);
    return tokens_adapter;
}
```

`execvp` is a C function — it has no idea what a C++ `std::string` is. It expects a plain array of C-style strings (`char*`), terminated by a `nullptr` so it knows where the list ends (C arrays don't track their own length).

This function loops over the tokens, calls `.c_str()` on each `std::string` to get a raw pointer to its characters, and appends that pointer to a new `vector<char*>`. The `const_cast` is necessary because `.c_str()` returns a `const char*` (a promise not to modify the string), but `execvp`'s signature technically expects non-const `char* const argv[]` — this is a well-known, safe pattern for this specific situation, since `execvp` doesn't actually modify the strings despite the signature.

The loop uses `const string& token` (a reference) rather than a copy — copying would create temporary strings that get destroyed at the end of each loop iteration, leaving the `char*` pointers dangling (pointing at memory that no longer holds valid data).

### 6. Running a Program: `fork` and `execvp`

The simplest case — a single command with no pipes or redirection — looks like this:

```cpp
pid_t pid = fork();
if (pid == 0)
{
    // CHILD: becomes the new program
    execvp(argvs[0], argvs.data());
    perror("execvp");   // only reached if execvp failed
    exit(1);
}
else
{
    // PARENT: waits for the child
    int status;
    waitpid(pid, &status, 0);
}
```

`fork()` is called once, but returns *twice* — once in each process. The child gets `0`; the parent gets the child's actual process ID (a positive number). This is the only way to tell, from inside the same compiled code, which of the two processes you're currently running as.

Only the child calls `execvp`. If `execvp` succeeds, the child's code is entirely replaced by the new program — the lines after it (`perror`, `exit(1)`) never run. They exist purely as a safety net for the case where `execvp` fails (e.g., the command doesn't exist).

The parent never touches `execvp` — it just calls `waitpid`, which blocks until the child finishes, before looping back to print the next prompt. Without this, the shell would print a new prompt immediately, racing with the child's output.

### 7. Builtins: `cd` and `exit`

`cd` and `exit` cannot be implemented by forking a child process, for a structural reason: `chdir()` (the function that changes a process's working directory) only affects the process that calls it. If a *child* called `chdir`, only that short-lived child's directory would change — it would then immediately exit, and the shell (the parent) would be completely unaffected. Same logic for `exit`: forking and exiting the child does nothing to the shell itself.

So both are checked *before* any `fork()` call, and handled directly by the shell's own process:

```cpp
if (tokens[0] == "cd")
{
    if (tokens.size() < 2)
    {
        cout << "cd: missing argument" << endl;
        continue;
    }
    if (chdir(argvs[1]) == 0)
    {
        char buffer[PATH_MAX];
        if (getcwd(buffer, sizeof(buffer)) != nullptr)
            cout << buffer << endl;
        continue;
    }
    else
    {
        perror("Failed to change directory");
        continue;
    }
}
else if (tokens[0] == "exit")
{
    exit(0);
}
```

Two safety checks matter here: `tokens.size() < 2` guards against `argvs[1]` being a `nullptr` (which would happen if the user typed just `cd` with nothing after it — accessing that would be undefined behavior), and `chdir`'s return value is checked so a bad path (e.g., a typo) produces a real error message via `perror` rather than silently failing or crashing.

### 8. Output Redirection (`>`)

`ls -la > out.txt` should send `ls`'s output into a file instead of the terminal. This works through **file descriptors** — every process has numbered "slots" for input/output, and slot `1` is conventionally "standard output" (stdout). Whatever a program writes to slot 1 is, by default, connected to the terminal — but that connection can be redirected.

First, `extract_redirection` scans the token list for `>`, remembers the filename that follows it, and removes both from the list (so `ls` never sees `>` or `out.txt` as if they were its own arguments):

```cpp
string extract_redirection(vector<string>& tokens)
{
    string file_name = "";
    for (auto it = tokens.begin(); it != tokens.end(); ++it)
    {
        if (*(it) == ">")
        {
            auto temp = it + 1;
            if (temp == tokens.end())
            {
                cout << "Error: No file specified for redirection" << endl;
                break;
            }
            file_name = *(temp);
            temp++;
            tokens.erase(it, temp);
            break;
        }
    }
    return file_name;
}
```

The bounds check (`temp == tokens.end()`) exists because if the user types `ls >` with nothing after it, there is no filename token to read — checking *before* dereferencing avoids undefined behavior.

Then, inside the child process, before `execvp`:

```cpp
int _open_result = open(_redirect_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
dup2(_open_result, STDOUT_FILENO);
close(_open_result);
```

- `open()` gets a file descriptor for the target file. Three flags are combined: `O_WRONLY` (open for writing — otherwise the file might open read-only), `O_CREAT` (create the file if it doesn't exist), and `O_TRUNC` (erase any existing contents first, so old data doesn't linger after shorter new output).
- `dup2(fd, STDOUT_FILENO)` makes file descriptor `1` (stdout) become a duplicate of the file's descriptor — from this point on, anything the process writes to "stdout" silently goes into the file instead.
- `close(_open_result)` closes the original descriptor, since `STDOUT_FILENO` is now an independent reference to the same file — the original isn't needed anymore.

### 9. Input Redirection (`<`)

The mirror image of output redirection: `cat < main.cpp` should make `cat` read from the file instead of the keyboard. `extract_input_redirection` works identically to `extract_redirection`, just searching for `<`. The child-side setup differs only in the `open` flags and the `dup2` target:

```cpp
int _open_result_input = open(_input_file.c_str(), O_RDONLY);
dup2(_open_result_input, STDIN_FILENO);
close(_open_result_input);
```

`O_RDONLY` (read-only — no need to create or truncate a file you're only reading), and `STDIN_FILENO` (file descriptor `0`) instead of stdout.

### 10. Pipelines (`|`)

This is the most involved feature. `cmd1 | cmd2 | cmd3` needs `cmd1`'s stdout to become `cmd2`'s stdin, and `cmd2`'s stdout to become `cmd3`'s stdin — with data flowing directly between processes, no file involved.

**Splitting the command.** `split_pipeline` walks the token list and cuts it into groups every time it sees `|`:

```cpp
vector<vector<string>> split_pipeline(vector<string>& tokens)
{
    vector<vector<string>> _result;
    vector<string> current_group;
    for (const string& token : tokens)
    {
        if (token != "|")
        {
            current_group.push_back(token);
        }
        else
        {
            _result.push_back(std::move(current_group));
            current_group.clear();
        }
    }
    _result.push_back(std::move(current_group));   // the final group has no trailing "|" to trigger it
    return _result;
}
```

For `"cat main.cpp | grep int | wc -l"`, this returns `{{"cat", "main.cpp"}, {"grep", "int"}, {"wc", "-l"}}` — three separate command groups. A command with no `|` at all simply produces a result with one group, which the main loop uses to decide whether to take the pipeline path or the plain single-command path.

**Creating the pipes.** For `N` commands in a chain, you need `N - 1` pipes — one for each "gap" between adjacent commands:

```cpp
int num_pipes = m_vector.size() - 1;
vector<array<int, 2>> pipes(num_pipes);
for (int i = 0; i < num_pipes; ++i)
{
    pipe(pipes[i].data());
}
```

`pipe()` fills a two-element array: index `0` is the **read end**, index `1` is the **write end** — data written into the write end can be read back out of the read end, entirely in kernel memory, no disk file involved. `pipes[i]` sits *between* command `i` and command `i + 1`.

**Forking each command.** One child is forked per command, and each one figures out — purely from its own index `i` — which pipe(s) it needs:

```cpp
if (i > 0)
{
    dup2(pipes[i - 1][0], STDIN_FILENO);   // read from the PREVIOUS pipe
}
if (i < N - 1)
{
    dup2(pipes[i][1], STDOUT_FILENO);      // write to the NEXT pipe
}
close_all_pipes(pipes);
```

- The **first** command (`i == 0`) skips the stdin redirect (nothing comes before it) but does redirect its stdout.
- The **last** command (`i == N - 1`) skips the stdout redirect (nothing comes after it) but does redirect its stdin.
- A **middle** command does both — it simultaneously reads from the pipe before it and writes to the pipe after it.

**Why every child closes every pipe, not just the ones it uses.** This is the single easiest thing to get wrong when implementing pipes, and the reason for the `close_all_pipes` helper:

```cpp
void close_all_pipes(const vector<array<int, 2>>& pipes)
{
    for (const auto& p : pipes)
    {
        close(p[0]);
        close(p[1]);
    }
}
```

`fork()` copies *every* open file descriptor into the child — including pipes that particular child has nothing to do with. A process reading from a pipe only sees "end of input" once *every* copy of that pipe's write end, across *every* process, has been closed. If even one unrelated process (say, the shell itself, or a command three stages away) keeps an unused copy of a pipe's write end open, a reader further down the chain will wait forever for data that will never come, because the kernel still thinks a writer *could* show up. So every child calls `close_all_pipes` on the *entire* `pipes` vector — including pipes it dup'd from (the dup'd copy on stdin/stdout is independent and stays open; only the original descriptor needs closing) and pipes it never touched at all.

The same logic applies to the parent: after forking every child, the parent closes every pipe's both ends too, and only then calls `waitpid` on each child in turn. If the parent forked all the children first and then waited on them one at a time *before* closing its own pipe copies, a middle command could still be blocked writing to a full pipe with no reader draining it — a deadlock. Forking all children first, *then* closing and waiting, lets every stage of the pipeline run concurrently.

### 11. Signal Handling (Ctrl+C)

Pressing Ctrl+C sends a signal called `SIGINT` to the process currently attached to the terminal. By default, receiving `SIGINT` kills a process immediately — which, without any handling, would kill the shell itself along with whatever command it was running.

The fix uses `signal()` from `<csignal>`, which lets a process override what happens when a given signal arrives:

```cpp
signal(SIGINT, SIG_IGN);   // once, at shell startup — the shell itself ignores Ctrl+C forever
```

But every child a shell creates *inherits* this "ignore" setting from `fork()` — meaning, without further action, a running command like `sleep 100` would also become immune to Ctrl+C, which is wrong; Ctrl+C should be able to kill the *running command*, just not the shell hosting it. So every child, right after `fork()` and before `execvp()`, explicitly restores the default behavior:

```cpp
signal(SIGINT, SIG_DFL);   // in every child, before execvp
```

This appears in two places in the code: the pipeline loop's child branch, and the single-command path's child branch — both `execvp` call sites needed it independently.

## Known Limitations

- Only a single space is treated as a token separator — there's no support for quoted arguments (`echo "hello world"` would be split into two tokens instead of one).
- Redirection (`>`/`<`) is only applied to single commands, not to individual stages inside a pipeline.
- No `>>` (append) mode yet — only truncating output redirection.
- No command history (up-arrow to recall previous commands).
- No background jobs (`&`) — every command runs in the foreground.
- No custom line editing — arrow keys and mid-line cursor movement aren't supported, since that requires putting the terminal into raw mode, which this project hasn't tackled yet.

## Planned Next Steps

- Background job support (`&`) with a job table and `SIGCHLD` handling to reap finished background processes without blocking the shell.
- Command history.
- `>>` append-mode redirection.
- Basic quoting/escaping in the tokenizer.
- Custom line editing via raw terminal mode (`termios`), allowing arrow-key navigation and mid-line edits.
