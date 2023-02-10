#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <exception>
#include <vector>
#include <unordered_map>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <deque>
#include <optional>

#include "machine.hpp"

enum MachineStatus {
    OFF,
    ON,
    BROKEN
};

// todo sprawdzic , czy działa
template<typename T>
class BlockingQueue {
private:
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<T> d_queue;
public:
    void push(T const &value) {
        {
            std::unique_lock<std::mutex> lock(this->d_mutex);
            d_queue.push_front(value);
        }
        this->d_condition.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(this->d_mutex);
        this->d_condition.wait(lock, [=] { return !this->d_queue.empty(); });
        T rc(std::move(this->d_queue.back()));
        this->d_queue.pop_back();
        return rc;
    }
};

class Order {
    enum OrderStatus {
        NOT_DONE,
        READY,
        INVALID
    };
private:
    unsigned int id;
    std::chrono::time_point<std::chrono::system_clock> time;
    OrderStatus status;
    unsigned int timeout;
public:
    explicit Order(unsigned int id, unsigned int timeout) :
            id(id),
            status(NOT_DONE),
            timeout(timeout) {}

    void markDone() {
        time = std::chrono::system_clock::now();
        status = READY;
    }

    unsigned int getId() const {
        return id;
    }

    std::optional<unsigned int> pendingId(std::chrono::time_point<std::chrono::system_clock>
            currentTime) {
        std::chrono::duration<double> elapsedTime = currentTime - time;
        if (status == READY && elapsedTime <= std::chrono::milliseconds(timeout)) {
            return id;
        }
        return {};
    }
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
    std::set<Order> orders;
    std::unordered_map<std::string, MachineStatus> machineStatus;

    std::vector<std::thread> workers;
    std::vector<struct WorkerReport> reports;

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

    void startWorking();
};


#endif // SYSTEM_HPP