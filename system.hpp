#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <iostream>
#include <set>

#include "machine.hpp"

enum MachineStatus {
    OFF,
    ON,
    BROKEN
};

class FulfillmentFailure : public std::exception {
};

class OrderNotReadyException : public std::exception {
};

class BadOrderException : public std::exception {
};

class BadPagerException : public std::exception {
};

class OrderExpiredException : public std::exception {
};

class RestaurantClosedException : public std::exception {
};

struct WorkerReport {
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

class Worker {
private:
    WorkerReport dailyReport;
    unsigned int id;
    std::thread thread;
public:
    Worker(unsigned int id)
            : id(id) {
        thread = std::thread{[this] { this->startWorking(); }};
    }

    void startWorking() {
        std::cout << "worker " << id << " started.\n";
    }

    WorkerReport getReport() { return dailyReport; }

    ~Worker() {
        if (thread.joinable()) {
            thread.join();
        }
    }

};

class CoasterPager {
private:
    unsigned int currentId;
public:
    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const { return currentId; };

    [[nodiscard]] bool isReady() const;
};

class System {
public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
private:
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;
    std::atomic_bool closed;
    machines_t machines;
    std::set<std::string> menu;
    std::unordered_map<unsigned int, Worker> workers;
    std::unordered_map<std::string, MachineStatus> machineStatus;

    std::mutex shutdown_mutex;

public:
    System(machines_t machines, unsigned int numberOfWorkers, unsigned int clientTimeout);

    void informClosed();

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
        return closed ? vector<string>() : vector<string>(menu.begin(), menu.end());
    }

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>> collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const { return clientTimeout; }

    bool productsInMenu(std::vector<string> &products);


    void initializeMachines(machines_t &machines);

    void closeMachines();
};


#endif // SYSTEM_HPP