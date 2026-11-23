# g6gdk: the Xbox Game Runtime APIs, written against the public GDK's surface (docs/gdk.md and
# docs/hle-status.md). XTaskQueue and XAsync so far; the guest reaches them through the Game
# Runtime's interface once the loader binds it.
add_library(g6gdk STATIC
    src/async.cpp
    src/task_queue.cpp)
target_include_directories(g6gdk PUBLIC include)
target_link_libraries(g6gdk PUBLIC g6common)
g6_target_defaults(g6gdk)
