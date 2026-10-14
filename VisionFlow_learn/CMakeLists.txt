cmake_minimum_required(VERSION 3.16)
project(VisionFlowLearn LANGUAGES CXX)

# 与原项目保持一致：使用 C++17，且不启用编译器专有语言扩展。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 创建名为 vision_cli 的可执行目标
add_executable(vision_cli
    apps/vision_cli.cpp
    src/line_utils.cpp
    src/processor.cpp
)

# core 提供基础类型，imgcodecs 提供图片读写，imgproc 提供图像处理。
find_package(OpenCV 4 REQUIRED COMPONENTS core imgcodecs imgproc)
# JSON 结果输出使用已有的 nlohmann/json 头文件库。
find_package(nlohmann_json 3.2 REQUIRED CONFIG)
target_include_directories(vision_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(vision_cli PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
)

add_executable(protocol_demo
    apps/protocol_demo.cpp
    src/protocol.cpp
)
target_include_directories(protocol_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 模块 7：学生写好新的入口后，再由学生重新配置生成此目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/vision_server.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_io.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/tcp_io.hpp")
    add_executable(vision_server
        apps/vision_server.cpp
        src/tcp_io.cpp
        src/protocol.cpp
        src/processor.cpp
        src/line_utils.cpp
    )
    target_include_directories(vision_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(vision_server PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
    )
endif()

# 模块 8：学生写好新的入口后，再由学生重新配置生成此目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/vision_epoll_server.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_io.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/tcp_io.hpp")
    add_executable(vision_epoll_server
        apps/vision_epoll_server.cpp
        src/tcp_io.cpp
        src/protocol.cpp
        src/processor.cpp
        src/line_utils.cpp
    )
    target_include_directories(vision_epoll_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(vision_epoll_server PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
    )
endif()

# 模块 9.1：学生写齐队列与观察程序后，自行重新配置生成目标。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/image_task_queue.hpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/image_task_queue.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/task_queue_demo.cpp")
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    add_executable(task_queue_demo
        apps/task_queue_demo.cpp
        src/image_task_queue.cpp
    )
    target_include_directories(task_queue_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(task_queue_demo PRIVATE Threads::Threads)
endif()

# 模块 9.2：学生写齐线程池和新入口后，自行重新配置。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/image_worker_pool.hpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/image_worker_pool.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/vision_pool_server.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/image_task_queue.hpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/image_task_queue.cpp")
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    add_executable(vision_pool_server
        apps/vision_pool_server.cpp
        src/image_worker_pool.cpp
        src/image_task_queue.cpp
        src/tcp_io.cpp
        src/protocol.cpp
        src/processor.cpp
        src/line_utils.cpp
    )
    target_include_directories(vision_pool_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(vision_pool_server PRIVATE
        Threads::Threads
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
    )
endif()

# 模块 10.2：学生写齐进程观察入口后，自行重新配置。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/process_child.cpp")
    add_executable(process_child apps/process_child.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/process_launcher.cpp")
    add_executable(process_launcher
        apps/process_launcher.cpp
        src/tcp_io.cpp
    )
    target_include_directories(process_launcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()


# 模块 10.3：学生写齐监管器和停止观察入口后，自行重新配置。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/shutdown_child.cpp")
    add_executable(shutdown_child apps/shutdown_child.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/process_supervisor.cpp")
    add_executable(process_supervisor
        apps/process_supervisor.cpp
        src/tcp_io.cpp
    )
    target_include_directories(process_supervisor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

