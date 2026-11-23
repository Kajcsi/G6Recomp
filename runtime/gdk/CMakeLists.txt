# g6gdk: the Xbox Game Runtime APIs, written against the public GDK's surface (docs/gdk.md and
# docs/hle-status.md): XTaskQueue, XAsync, XError, XThread, and XUser, and the xgameruntime.dll
# entry points through which the guest reaches them, which the default import policy binds.
add_library(g6gdk STATIC
    src/async.cpp
    src/error.cpp
    src/runtime.cpp
    src/task_queue.cpp
    src/thread.cpp
    src/user.cpp)
target_include_directories(g6gdk PUBLIC include)
target_link_libraries(g6gdk PUBLIC g6common)
g6_target_defaults(g6gdk)
