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

    vector <std::string> products;

    OrderStatus status;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
    std::mutex &orderInfoMutex;
public:
    explicit Order(unsigned int id,
                   vector <std::string> &products,
                   unsigned int timeout,
                   std::mutex &mutex) :
            id(id),
            timeout(timeout),
            products(products),
            status(NOT_DONE),
            orderInfoMutex(mutex) {}

    void markDone() {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
        doneTime = std::chrono::system_clock::now();
        status = READY;
    }

    [[nodiscard]] size_t getId() const {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
        return id;
    }

    [[nodiscard]] vector <std::string> getProducts() const {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
        return products;
    }

    [[nodiscard]] bool isReady() const {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
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

    bool checkIfPending() {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
        checkIfExpired();

        if (status == READY) {
            return true;
        }
        return false;
    }

    OrderStatus getStatus() {
        std::unique_lock<std::mutex> lock(orderInfoMutex);
        checkIfExpired();

        return status;
    }
};

class OrderSynchronizer;
class MachineWrapper;

template<typename T>
class BlockingQueue {
private:
    typedef std::future<std::unique_ptr<Product>> product_future_t;
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<T> d_queue;
public:
    void push(T const &value);

    std::pair<Order, std::vector<product_future_t>>
    manageOrder(OrderSynchronizer &sync, std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> &machines);

    T pop();

    void block() { d_mutex.lock(); }

    void unblock() { d_mutex.unlock(); }
};

class MachineWrapper {
    std::string machineName;
    std::shared_ptr<Machine> machine;
    BlockingQueue<std::promise<std::unique_ptr<Product>>>
            machineQueue;
    std::thread thread;
    atomic_bool &systemOpen;
    std::mutex returnProductMutex;
public:
    MachineWrapper(std::string machineName,
                   std::shared_ptr<Machine> &machine,
                   atomic_bool &systemOpen) :
            machineName(std::move(machineName)),
            machine(std::move(machine)),
            systemOpen(systemOpen) {
        thread = std::thread([this] { machineWorker(); });
    }

    void insertToQueue(std::promise<std::unique_ptr<Product>> &promise) {
        machineQueue.push(promise);
    }

    //todo może usunąć referncję
    void returnProduct(std::unique_ptr<Product> &product) {
        std::unique_lock<std::mutex> lock(returnProductMutex);
        machine->returnProduct(std::move(product));
    }

private:
    void machineWorker() {
        // todo: sytuacja, w której wątek czeka na kolejce, a system zamyka się
        while (!systemOpen) {
            std::promise<unique_ptr < Product>>
            promise = machineQueue.pop();
            try {
                auto product = machine->getProduct();
                promise.set_value(std::move(product));

            } catch (MachineFailure &e) {
                promise.set_exception(std::current_exception());
            }
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
    std::vector<product_future_t>
    insertOrderToMachines(Order &order,
                          std::unordered_map<std::string,
                                  std::shared_ptr<MachineWrapper>> &machines) {

        std::unique_lock<std::mutex> lock(mutex);

        std::vector<product_future_t> futures;
        for (const auto &productName: order.getProducts()) {

            product_promise_t newPromise;
            product_future_t newFuture = newPromise.get_future();

            machines.at(productName)->insertToQueue(newPromise);
            futures.push_back(std::move(newFuture));
        }
        return futures;
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

    // todo zmienić widoczność konstruktora i getOrder na prywatne
    Order getOrder() { return order; }

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
    typedef std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> machineWrappers_t;
    IdGenerator idGenerator;
    unsigned int clientTimeout;
    std::atomic_bool systemOpen;

    OrderSynchronizer orderSynchronizer;
    std::map<size_t, Order> orders;
    BlockingQueue<Order> orderQueue;
    std::map<size_t, CoasterPager> pagers;

    std::set<std::string> menu;
    std::mutex menuMutex;

    machineWrappers_t machines;

    std::vector<std::thread> orderWorkers;
    std::vector<struct WorkerReport> workerReports;

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

    bool productsInMenu(std::vector<string> &products);

private:
    void closeMachines();

    void informClosed();

    void orderWorker();

    void machineWorker();
};


#endif // SYSTEM_HPP