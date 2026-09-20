#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <fstream>
#include <fcntl.h>
#include <csignal>

using std::cout;
using std::string;
using std::cin;
using std::endl;
using std::vector;
using std::stringstream;
using std::ofstream;
using std::cerr;
using std::pair;
using std::array;

void close_all_pipes(const vector<array<int, 2>>& pipes)
{
    for (const auto& p : pipes)
    {
        close(p[0]);
        close(p[1]);
    }
}


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
    _result.push_back(std::move(current_group));
    return _result;
}

string extract_input_redirection(vector<string>& tokens)
{
    string file_name = "";

    for (auto it = tokens.begin(); it != tokens.end(); ++it)
    {
        if (*(it) == "<")
        {
            auto temp = it + 1;
            if (temp == tokens.end())
            {
                cout << "Error: No file specified for input" << endl;
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


string read_line()
{
    string full_command;
    getline(cin, full_command);
    return full_command;
}


void print_prompt()
{
    cout << "$ ";
}


int main()
{
    signal(SIGINT, SIG_IGN);
    while (true)
    {
        print_prompt();
        vector<string> tokens = tokenize(read_line());
        if (cin.eof())
        {
            cout << "No more commands" << endl;
            return -1;
        }
        if (tokens.empty())
        {
            continue;
        }

        vector<vector<string>> m_vector = split_pipeline(tokens);

        if (m_vector.size() > 1)
        {
            int num_pipes = m_vector.size() - 1;
            vector<array<int, 2>> pipes(num_pipes);
            for (int i = 0; i < num_pipes; ++i)
            {
                if (pipe(pipes[i].data()) < 0)
                {
                    perror("pipe failed");
                    exit(1);
                }
            }

            vector<pid_t> child_pids;
            int N = m_vector.size();

            for (int i = 0; i < N; ++i)
            {
                pid_t pid = fork();
                if (pid < 0)
                {
                    perror("fork failed");
                    exit(1);
                }
                else if (pid == 0)
                {
                    // --- CHILD PROCESS ---
                    signal(SIGINT, SIG_DFL);
                    if (i > 0)
                    {
                        if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0)
                        {
                            perror("dup2 stdin failed");
                            exit(1);
                        }
                    }

                    if (i < N - 1)
                    {
                        if (dup2(pipes[i][1], STDOUT_FILENO) < 0)
                        {
                            perror("dup2 stdout failed");
                            exit(1);
                        }
                    }

                    close_all_pipes(pipes);

                    vector<char*> argvs = to_argv(m_vector[i]);
                    execvp(argvs[0], argvs.data());

                    perror("execvp failed");
                    exit(1);
                }
                else
                {
                    // --- PARENT PROCESS ---
                    child_pids.push_back(pid);
                }
            }

            // --- POST-LOOP (PARENT ONLY) ---
            close_all_pipes(pipes);

            for (pid_t wpid : child_pids)
            {
                int status;
                waitpid(wpid, &status, 0);
            }

            continue; // pipeline handled, skip the single-command path below
        }

        // --- SINGLE-COMMAND PATH (unchanged) ---

        string _redirect_file = extract_redirection(tokens);
        string _input_file = extract_input_redirection(tokens);

        vector<char*> argvs = to_argv(tokens);
        if (tokens[0] == "cd")
        {
            if (tokens.size() < 2)
            {
                cout << "cd: missing argument" << endl;
                continue;
            }
            else
            {
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
        }
        else
        {
            if (tokens[0] == "exit")
            {
                exit(0);
            }
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            cout << "Error: no child was created" << endl;
        }
        else
        {
            if (pid == 0)
            {
                signal(SIGINT, SIG_DFL);
                if (!_redirect_file.empty())
                {
                    int _open_result = open(_redirect_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                            0666);
                    if (_open_result < 0)
                    {
                        cerr << "Error: failed to open the file: " << _redirect_file << endl;
                        return 1;
                    }
                    if (dup2(_open_result, STDOUT_FILENO) < 0)
                    {
                        perror("dup2 failed");
                        exit(1);
                    }
                    close(_open_result);
                }
                if (!_input_file.empty())
                {
                    int _open_result_input = open(_input_file.c_str(), O_RDONLY);
                    if (_open_result_input < 0)
                    {
                        cerr << "Error: failed to open the file: " << _input_file << endl;
                        return 1;
                    }
                    if (dup2(_open_result_input, STDIN_FILENO) < 0)
                    {
                        perror("dup2 failed");
                        exit(1);
                    }
                    close(_open_result_input);
                }

                execvp(argvs[0], argvs.data());
                perror("execvp");
                exit(1);
            }
            else
            {
                int status;
                waitpid(pid, &status, 0);
            }
        }
    }
}
