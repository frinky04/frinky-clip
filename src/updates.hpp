#pragma once
#include "common.hpp"

namespace clip {
// Called before normal startup, including command-line and recorder dispatch.
void update_startup();

class Updates {
public:
    struct Status {
        bool installed = false, working = false, available = false, ready = false;
        int progress = 0;
        std::string version, message, error;
    };
    Updates();
    void tick(bool automatic);
    Status status() const;
    void check();
    void download();
    void apply(bool resume_recording); // Only after the recorder and helpers exit.
private:
    struct Work;
    std::shared_ptr<Work> work_;
    std::int64_t started_ = 0, last_check_ = 0, next_manual_ = 0;
    std::string last_report_;
    void check(bool automatic);
};
}
