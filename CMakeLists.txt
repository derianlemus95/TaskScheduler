cmake_minimum_required(VERSION 3.14)
project(worker)

set(CMAKE_CXX_STANDARD 14)

# Specify the vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE "C:/Users/deven/OneDrive/Desktop/grpc_stuff/vcpkg/scripts/buildsystems/vcpkg.cmake")

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_executable(server server.cpp task.pb.cc task.grpc.pb.cc)
target_link_libraries(server gRPC::grpc++ protobuf::libprotobuf)

add_executable(client client.cpp task.pb.cc task.grpc.pb.cc)
target_link_libraries(client gRPC::grpc++ protobuf::libprotobuf)