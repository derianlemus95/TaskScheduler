#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <unordered_map>
#include <chrono>
#include <random>
#include "task.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::ServerReaderWriter;
using worker::Task;
using worker::TaskStatus;
using worker::Scheduler;
using worker::HeartBeatRequest;
using worker::HeartBeatResponse;

struct LocalTask {
    int task_id;
    std::string command;
    std::string status;

    LocalTask(int id, const std::string& cmd)
        : task_id(id), command(cmd), status("in-progress") {}
};

class Worker {
private:
    std::string worker_id;
    int type;
    int total_capacity;
    int current_capacity;
    std::list<LocalTask> tasks;
    std::unordered_map<int, std::string> previous_statuses;
    std::mutex tasks_mutex;
    std::condition_variable task_cv;
    bool stop_flag = false;
    std::vector<std::thread> thread_pool;
    std::default_random_engine generator;
    std::uniform_int_distribution<int> distribution;
    std::uniform_int_distribution<int> status_distribution;

    void processTask() {
        while (true) {
            LocalTask task(0, "");
            {
                std::unique_lock<std::mutex> lock(tasks_mutex);
                task_cv.wait(lock, [this] { return !tasks.empty() || stop_flag; });

                if (stop_flag && tasks.empty()) break;

                auto it = std::find_if(tasks.begin(), tasks.end(), [](const LocalTask& t) { return t.status == "in-progress"; });
                if (it != tasks.end()) {
                    task = *it;
                    it->status = "processing";
                }
                else {
                    continue;
                }
            }

            int processing_time = distribution(generator);
            std::this_thread::sleep_for(std::chrono::milliseconds(processing_time));
         
            int result = status_distribution(generator);

            {
                std::lock_guard<std::mutex> guard(tasks_mutex);
                auto it = std::find_if(tasks.begin(), tasks.end(), [&](const LocalTask& t) { return t.task_id == task.task_id; });
                if (it != tasks.end()) {
                    if (result == 0) {
                        it->status = "completed";
                    }
                    else {
                        it->status = "failed";
                    }
                }
                current_capacity++;
            }
        }
    }

public:
    Worker(const std::string& id, int tp, int cap)
        : worker_id(id), type(tp), total_capacity(cap), current_capacity(cap), distribution(1000, 2000), status_distribution(0, 1) { 
        for (int i = 0; i < total_capacity; ++i) {
            thread_pool.emplace_back(&Worker::processTask, this);
        }
    }

    ~Worker() {
        {
            std::lock_guard<std::mutex> guard(tasks_mutex);
            stop_flag = true;
        }
        task_cv.notify_all();
        for (std::thread& t : thread_pool) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void receiveTask(int task_id, const std::string& command) {
        {
            std::lock_guard<std::mutex> guard(tasks_mutex);
            if (current_capacity > 0) {
                tasks.emplace_back(task_id, command);
                current_capacity--;
                task_cv.notify_one();
                previous_statuses[task_id] = "in-progress";
            }
            else {
                std::cerr << "No capacity to handle new task." << std::endl;
            }
        }
    }

    void sendHeartbeat(ServerReaderWriter<HeartBeatResponse, HeartBeatRequest>* stream) {
        while (true) {
            {
                std::lock_guard<std::mutex> guard(tasks_mutex);

                HeartBeatResponse response;
                response.set_workerid(std::stoi(worker_id));
                response.set_current_capacity(current_capacity);

                std::vector<int> tasks_to_remove;

                for (const auto& task : tasks) {
                    auto prev_status_it = previous_statuses.find(task.task_id);
                    if ((task.status == "completed" || task.status == "failed") &&
                        (prev_status_it == previous_statuses.end() || prev_status_it->second != task.status)) {
                        TaskStatus* task_status = response.add_tasks();
                        task_status->set_taskid(task.task_id);
                        task_status->set_status(task.status);
                        previous_statuses[task.task_id] = task.status;
                        tasks_to_remove.push_back(task.task_id);
                    }
                }
                stream->Write(response);

                std::cout << "Sending Heartbeat:" << std::endl;
                std::cout << "  Worker ID: " << response.workerid() << std::endl;
                std::cout << "  Current Capacity: " << response.current_capacity() << std::endl;
                for (const auto& task_status : response.tasks()) {
                    std::cout << "  Task ID: " << task_status.taskid() << ", Status: " << task_status.status() << std::endl;
                }

                for (int task_id : tasks_to_remove) {
                    auto it = std::find_if(tasks.begin(), tasks.end(), [&](const LocalTask& t) { return t.task_id == task_id; });
                    if (it != tasks.end()) {
                        tasks.erase(it);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void run() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
};

class WorkerService final : public Scheduler::Service {
public:
    WorkerService(Worker* worker) : worker_(worker) {}

    Status SubmitTask(ServerContext* context, const Task* task, TaskStatus* status) override {
        worker_->receiveTask(task->taskid(), task->commands());
        status->set_status("Received");
        status->set_taskid(task->taskid());
        return Status::OK;
    }

    Status Heartbeat(ServerContext* context, ServerReaderWriter<HeartBeatResponse, HeartBeatRequest>* stream) override {
        worker_->sendHeartbeat(stream);
        return Status::OK;
    }

private:
    Worker* worker_;
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    Worker worker("123", 1, 20);
    WorkerService service(&worker);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    std::thread worker_thread(&Worker::run, &worker);
    server->Wait();
    worker_thread.join();
}

int main(int argc, char** argv) {
    RunServer();
    return 0;
}
