#ifndef SYSTEM_HPP
#define SYSTEM_HPP

#include <exception>
#include <utility>
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


class Order {
public:
    enum OrderStatus {
        NOT_DONE,
        BROKEN_MACHINE,
        READY,
        RECEIVED,
        EXPIRED,
        OTHER
    };
private:
    size_t id;
    unsigned int timeout;

    std::vector<std::string> products;
    std::vector<std::unique_ptr<Product>> readyProducts;

    OrderStatus status;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
    std::unique_ptr<std::mutex> orderInfoMutex;
public:
    Order(size_t id,
          std::vector <std::string> &products,
          unsigned int timeout) :
            id(id),
            timeout(timeout),
            products(products),
            status(NOT_DONE),
            orderInfoMutex(std::make_unique<std::mutex>()) {}

    void markDone(std::vector<std::unique_ptr<Product>> &achievedProducts) {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        readyProducts = std::move(achievedProducts);
        doneTime = std::chrono::system_clock::now();
        status = READY;
    }

    [[nodiscard]] size_t getId() const {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        return id;
    }

    [[nodiscard]] std::vector <std::string> getProductNames() {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        return products;
    }

    [[nodiscard]] bool isReady() {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        checkIfExpired();
        return status == READY;
    }

    void checkIfExpired() {
        if (status == READY) {
            std::chrono::duration<double> elapsedTime =
                    std::chrono::system_clock::now() - doneTime;
            if (elapsedTime > std::chrono::milliseconds(timeout)) {
                status = EXPIRED;
            }
        }
    }

    [[nodiscard]] bool checkIfPending() const {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        if (status == READY || status == NOT_DONE) {
            return true;
        }
        return false;
    }

    OrderStatus getStatus() {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        checkIfExpired();
        return status;
    }

    void setStatus(OrderStatus newStatus) {
        std::unique_lock<std::mutex> lock(*orderInfoMutex);
        status = newStatus;
    }
};

struct WorkerReport {
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

class WorkerReportUpdater {
private:
    struct WorkerReport report;
public:
    void updateFailedOrder(Order &order) {
        report.failedOrders.push_back(order.getProductNames());
    }

    void updateFailedProduct(std::string &product) {
        report.failedProducts.push_back(product);
    }

    void updateCollectedOrder(Order &order) {
        report.collectedOrders.push_back(order.getProductNames());
    }

    void updateAbandonedOrder(Order &order) {
        report.abandonedOrders.push_back(order.getProductNames());
    }

    WorkerReport getReport() {
        return report;
    }
};

class OrderSynchronizer;
class MachineWrapper;

class OrderQueue {
private:
    typedef std::future<std::unique_ptr<Product>> product_future_t;
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<Order> d_queue;
public:
    void pushOrder(Order &value);

    Order popOrder(std::unique_lock<std::mutex> &lock);

    std::mutex &getMutex() { return d_mutex; }
};


class MachineQueue {
private:
    typedef std::promise<std::unique_ptr<Product>> product_promise_t;
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<product_promise_t> d_queue;
public:
    void pushPromise(product_promise_t &value);

    product_promise_t popPromise();
};

class MachineWrapper {
public:
    enum MachineStatus {
        OFF,
        ON,
        BROKEN
    };
private:
    std::string machineName;
    std::shared_ptr<Machine> machine;
    MachineStatus status;
    MachineQueue machineQueue;
    std::thread thread;
    std::atomic_bool &systemOpen;
    std::mutex returnProductMutex;
public:
    MachineWrapper(std::string machineName,
                   std::shared_ptr<Machine> &machine,
                   std::atomic_bool &systemOpen) :
            machineName(std::move(machineName)),
            machine(std::move(machine)),
            status(OFF),
            systemOpen(systemOpen) {
        thread = std::thread([this] { machineWorker(); });
    }

    void insertToQueue(std::promise<std::unique_ptr<Product>> &promise) {
        machineQueue.pushPromise(promise);
    }

    //todo może usunąć referncję
    void returnProduct(std::unique_ptr<Product> &product) {
        std::unique_lock<std::mutex> lock(returnProductMutex);
        machine->returnProduct(std::move(product));
    }

private:

    void startMachine() {
        machine->start();
        status = ON;
    }

    void stopMachine() {
        machine->stop();
        status = OFF;
    }

    void machineWorker() {
        // todo: sytuacja, w której wątek czeka na kolejce, a system zamyka się
        while (!systemOpen) {
            std::promise<std::unique_ptr<Product>>
                    promise = machineQueue.popPromise();

            if (status == OFF) {
                startMachine();
            }

            try {
                auto product = machine->getProduct();
                promise.set_value(std::move(product));

            } catch (MachineFailure &e) {
                promise.set_exception(std::current_exception());
            }
        }

        if(status == ON) {
            stopMachine();
        }
    }

    ~MachineWrapper() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

class OrderSynchronizer {
private:
    typedef std::promise<std::unique_ptr<Product>> product_promise_t;
    typedef std::future<std::unique_ptr<Product>> product_future_t;

    std::condition_variable condition;
    std::mutex mutex;

public:
    std::vector<std::pair<std::string, product_future_t>>
    insertOrderToMachines(Order &order,
                          std::unordered_map<std::string,
                                  std::shared_ptr<MachineWrapper>> &machines) {

        std::unique_lock<std::mutex> lock(mutex);

        std::vector<std::pair<std::string, product_future_t>> futureProducts;
        for (const auto &productName: order.getProductNames()) {

            product_promise_t newPromise;
            product_future_t newFuture = newPromise.get_future();

            machines.at(productName)->insertToQueue(newPromise);
            futureProducts.emplace_back(productName, std::move(newFuture));
        }
        return futureProducts;
    }
};

class CoasterPager {
    friend class System;

private:
    Order &order;

    explicit CoasterPager(Order &order) :
            order(order) {}

    Order &getOrder() { return order; }

public:

    void wait() const;

    void wait(unsigned int timeout) const;

    [[nodiscard]] unsigned int getId() const { return order.getId(); };

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
    typedef std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> machineWrappers_t;
    IdGenerator idGenerator;
    unsigned int clientTimeout;
    std::atomic_bool systemOpen;

    OrderSynchronizer orderSynchronizer;
    std::map<size_t, Order> orders;
    OrderQueue orderQueue;
    std::map<size_t, CoasterPager> pagers;

    std::set<std::string> menu;
    std::mutex menuMutex;

    machineWrappers_t machines;

    std::vector<std::thread> orderWorkers;
    std::vector<WorkerReport> workerReports;

public:
    System(machines_t machines, unsigned int numberOfWorkers,
           unsigned int clientTimeout);

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const;

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>>
    collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const { return clientTimeout; }
private:
    bool productsInMenu(std::vector<std::string> &products);
    void removeFromMenu(std::string &product);

    void returnProducts(
            std::vector<std::pair<std::string, std::unique_ptr<Product>>>
            &readyProducts);

    void orderWorker();
};


#endif // SYSTEM_HPP