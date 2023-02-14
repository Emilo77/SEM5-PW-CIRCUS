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
#include <queue>

#include "machine.hpp"

typedef std::promise<std::unique_ptr<Product>> product_promise_t;
typedef std::future<std::unique_ptr<Product>> product_future_t;
typedef std::shared_ptr<product_promise_t> promise_ptr;

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

struct WorkerReport;

class WorkerReportUpdater;

class OrderSynchronizer;

class MachineWrapper;

class CoasterPager;

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
    typedef std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> machine_wrappers_t;

    size_t id;
    unsigned int timeout;

    std::vector<std::string> products;
    std::vector<std::pair<std::string, std::unique_ptr<Product>>> readyProducts;

    OrderStatus status;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
    std::shared_ptr<std::mutex> orderInfoMutex;
    std::shared_ptr<std::mutex> orderStatusMutex;
    std::shared_ptr<std::condition_variable> clientNotifier;
    std::shared_ptr<std::condition_variable> workerNotifier;
public:
    Order(size_t id,
          std::vector<std::string> &products,
          unsigned int timeout) :
            id(id),
            timeout(timeout),
            products(products),
            status(NOT_DONE),
            orderInfoMutex(std::make_shared<std::mutex>()),
            orderStatusMutex(std::make_shared<std::mutex>()),
            clientNotifier(std::make_shared<std::condition_variable>()),
            workerNotifier(std::make_shared<std::condition_variable>()) {}

    void waitForOrder();

    void waitForOrderTimed(unsigned int time);

    void waitForClient(WorkerReportUpdater &infoUpdater, machine_wrappers_t
    &machines);

    void
    markOrderDone(std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                  &namedProducts);


    std::vector<std::unique_ptr<Product>> collectReadyProducts();

    void notifyWorker() { workerNotifier->notify_one(); }

    void notifyClient() { clientNotifier->notify_one(); }

    std::vector<std::unique_ptr<Product>> tryToCollectOrder();

    void returnProducts(machine_wrappers_t &machines);

    [[nodiscard]] size_t getId() const { return id; }

    [[nodiscard]] bool isReady();

    [[nodiscard]] std::vector<std::string> getProductNames();

    void updateIfExpired();

    bool checkIfPending();

    OrderStatus getStatus();

    void setStatus(OrderStatus newStatus) { status = newStatus; }

    void setStatusLocked(OrderStatus newStatus);

    std::shared_ptr<std::mutex> getInfoMutex() { return orderInfoMutex; }
    std::shared_ptr<std::mutex> getStatusMutex() { return orderStatusMutex; }
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
    bool actionMade{false};
public:
    enum ACTION {
        FAIL,
        COLLECT,
        ABANDON
    };

    void updateOrder(Order &order, ACTION a);

    void updateFailedProduct(std::string &product) {
        report.failedProducts.push_back(product);
        actionMade = true;
    }

    void addReport(std::vector<WorkerReport> &workerReports) {
        if (actionMade) {
            workerReports.push_back(report);
        }
    }
};


class OrderQueue {
private:
    std::mutex queMutex;
    std::condition_variable queCondition;
    std::queue<std::shared_ptr<Order>> que;
public:

    void pushOrder(std::shared_ptr<Order> value);

    std::optional<std::shared_ptr<Order>>
    popOrder(std::unique_lock<std::mutex> &lock,
                                    std::atomic_bool &systemOpen);

    bool isEmpty();

    void notifyShutdown() { queCondition.notify_all(); }

    std::mutex &getMutex() { return queMutex; }
};


class MachineQueue {
private:
    typedef std::promise<std::unique_ptr<Product>> product_promise_t;
    std::mutex queMutex;
    std::condition_variable queCondition;
    std::queue<promise_ptr> que;
public:
    void pushPromise(promise_ptr value);

    std::optional<promise_ptr> popPromise(std::atomic_bool &ended);

    void notify() { queCondition.notify_all(); }
};


class MachineWrapper {
public:
    enum MachineStatus {
        OFF,
        ON,
    };
private:
    std::string machineName;
    std::shared_ptr<Machine> machine;
    MachineStatus status;
    MachineQueue machineQueue;
    std::thread worker;
    std::atomic_bool &orderWorkersEnded;
    std::mutex returnProductMutex;
public:
    MachineWrapper(std::string machineName,
                   std::shared_ptr<Machine> &machine,
                   std::atomic_bool &orderWorkersEnded) :
            machineName(std::move(machineName)),
            machine(std::move(machine)),
            status(OFF),
            orderWorkersEnded(orderWorkersEnded) {
        worker = std::thread([this] { machineWorker(); });
    }

    void insertToQueue(promise_ptr promise) {
        machineQueue.pushPromise(std::move(promise));
    }

    void returnProduct(std::unique_ptr<Product> &product) {
        /* Sekcja krytyczna dla metody returnProduct w maszynie. */
        std::unique_lock<std::mutex> lock(returnProductMutex);
        machine->returnProduct(std::move(product));
    }

    void notifyShutdown() { machineQueue.notify(); }

    void joinMachineWorker() {
        if (worker.joinable()) {
            worker.join();
        }
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

    void machineWorker();
};

class CoasterPager {
    friend class System;

private:
    std::shared_ptr<Order> order;

    explicit CoasterPager(std::shared_ptr<Order> order) : order(
            std::move(order)) {}

public:

    void wait() const { order->waitForOrder(); }

    void wait(unsigned int timeout) const { order->waitForOrderTimed(timeout); }

    [[nodiscard]] unsigned int getId() const { return order->getId(); };

    [[nodiscard]] bool isReady() const { return order->isReady(); };
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
            std::shared_ptr<MachineWrapper>> machine_wrappers_t;

    IdGenerator idGenerator;
    unsigned int clientTimeout;
    std::atomic_bool systemOpen;

    machine_wrappers_t machines;

    std::set<std::string> menu;
    mutable std::mutex menuMutex;

    std::map<size_t, std::shared_ptr<Order>> orders;
    mutable std::mutex ordersMapMutex;
    mutable std::mutex newOrderMutex;
    mutable std::mutex collectOrderMutex;
    OrderQueue orderQueue;

    std::vector<std::thread> orderWorkers;
    mutable std::mutex workerReportsMutex;
    std::vector<WorkerReport> workerReports;
    std::atomic_bool orderWorkersEnded;

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


    void orderWorker();
};


#endif // SYSTEM_HPP