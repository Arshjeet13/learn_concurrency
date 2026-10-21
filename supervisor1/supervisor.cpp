#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <cerrno>
#include <vector>

void worker(int slot){
    //worker <slot> pid=<pid> started
    pid_t pid = getpid();
    std::cout << "worker " << slot << " pid=" << pid << " started\n" << std::flush;
    while(true){
        std::cout << "worker " << slot << " pid=" << pid << "\n" << std::flush;
        sleep(1);
    }
}

void spawn_worker(int slot, std::vector<pid_t>& workers){
    pid_t p = fork();

    if(p < 0){
        std::cerr << "Fork failed\n" << std::flush;
        exit(1);
    }
    if(p == 0){
        worker(slot);
        _exit(0);
    }
    if(p > 0){
        workers[slot] = p;
    }
}

int main(int argc, char** argv){
    if(argc != 2){
        std::cerr << "Usage: "<< argv[0] << " <arg1>\n";
        exit(1);
    }

    int N = std::stoi(argv[1]);

    std::vector<pid_t> workers(N);

    for(int i=0; i<N; ++i){
        spawn_worker(i, workers);
    }

    while(true){
        int wstatus;
        pid_t p = waitpid(-1, &wstatus, 0);
        if(p < 0){
            std::cerr << "Parent: Failure while waiting. " << strerror(errno) << "\n" << std::flush;
            exit(1);
        }

        int idx = -1;
        for(int i = 0; i < N; ++i){
            if(workers[i] == p){
                idx = i;
                break;
            }
        }
        if(idx == -1){
            std::cerr << "Workers array doesn't contain worker with pid " << p << "\n";
            exit(1);
        }

        if(WIFEXITED(wstatus)){
            // slot <k> pid=<pid> died: exited code=<c>
            std::cout << "slot " << idx << " pid=" << p << " died: exited code=" << WEXITSTATUS(wstatus) << "\n" << std::flush;
        }
        if(WIFSIGNALED(wstatus)){
            // slot <k> pid=<pid> died: killed by signal <s> (<name>)
            const char* abbrev = sigabbrev_np(WTERMSIG(wstatus));
            std::string signal_name = (abbrev != nullptr) ? abbrev : "unknown";
            std::cout << "slot " << idx << " pid=" << p << " died: killed by signal " << WTERMSIG(wstatus) << " (" << signal_name << ")" <<"\n" << std::flush;
        }

        spawn_worker(idx, workers);
    }
}