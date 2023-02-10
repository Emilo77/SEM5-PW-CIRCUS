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
#include <map>

#include "machine.hpp"

enum MachineStatus {
    OFF,
    ON,
    BROKEN
};


template<typename T>
class OrderQueue {
private:
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<T> d_queue;
public:
    void pushOrder(T const &value) {
        {
            std::unique_lock<std::mutex> lock(this->d_mutex);
            d_queue.push_front(value);
        }
        this->d_condition.notify_one();
    }

    T popOrder() {
        std::unique_lock<std::mutex> lock(this->d_mutex);
        this->d_condition.wait(lock, [=] { return !this->d_queue.empty(); });
        T order(std::move(this->d_queue.back()));
        this->d_queue.pop_back();

        // uzupełnienie wszystkich kolejek, do których trzeba wrzucić zamówienie

        return order;
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

class Order {
    enum OrderStatus {
        NOT_DONE,
        READY,
        INVALID
    };
private:
    size_t id;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
    OrderStatus status;
    unsigned int timeout;
    vector <std::string> products;
public:
    explicit Order(unsigned int id, unsigned int timeout) :
            id(id),
            status(NOT_DONE),
            timeout(timeout) {}

    void markDone() {
        doneTime = std::chrono::system_clock::now();
        status = READY;
    }

    [[nodiscard]] size_t getId() const {
        return id;
    }

    [[nodiscard]] vector <std::string> getProducts() const {
        return products;
    }

    [[nodiscard]] bool isReady() const {
        return status == READY;
    }

    std::optional<unsigned int> pendingId(std::chrono::time_point<std::chrono::system_clock>
                                          currentTime) {
        std::chrono::duration<double> elapsedTime = currentTime - doneTime;
        if (status == READY && elapsedTime <= std::chrono::milliseconds(timeout)) {
            return id;
        }
        return {};
    }
};

class MachinesSynchronizer {
private:
    std::unordered_map<std::string, std::deque<unsigned int>> ques;
    std::mutex mutex;
public:
    void initialize(const std::unordered_map<std::string,
            std::shared_ptr<Machine>> &machines) {
        std::unique_lock<std::mutex> lock(mutex);
        for (auto &machine: machines) {
            ques.emplace(machine.first, std::deque<unsigned int>());
        }
    }

    void insertOrder(Order &order) {
        std::unique_lock<std::mutex> lock(mutex);
        for (const auto &product: order.getProducts()) {
            if (ques.count(product))
                ques.at(product).push_front(order.getId());
            else {
                cout << "insertOrder: product "
                     << product << " not found in any machine\n";
                exit(1);
            }
        }
    }

    void popProduct(const std::string &product) {
        std::unique_lock<std::mutex> lock(mutex);
        if (ques.count(product))
            ques.at(product).pop_back();
        else {
            cout << "popProduct: product "
                 << product << " not found in any machine\n";
            exit(1);
        }
    }
};


class CoasterPager {
private:
    size_t currentId;
    Order order;
public:
    CoasterPager(size_t id, Order &order) :
            currentId(id),
            order(order) {}

    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const { return currentId; };

    [[nodiscard]] bool isReady() const { return order.isReady(); };
};

class System {
private:
    class IdGenerator {
        size_t id{1};
    public:
        size_t newId() { return id++; }
    };

public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
private:
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    IdGenerator idGenerator;
    std::set<Order> orders;
    std::map<size_t, CoasterPager> pagers;

    std::atomic_bool closed;

    std::set<std::string> menu;
    std::mutex menuMutex;

    machines_t machines;
    std::unordered_map<std::string, MachineStatus> machineStatus;
    MachinesSynchronizer mSynchronizer;

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