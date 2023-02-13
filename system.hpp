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

struct WorkerReport;

class WorkerReportUpdater;

class OrderSynchronizer;

class MachineWrapper;

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
            orderInfoMutex(std::make_shared<std::mutex>()) {}

    void waitForOrder();
    void waitForOrderTimed(unsigned int time);
    void waitForClient(WorkerReportUpdater &infoUpdater, machine_wrappers_t
    &machines);

    void markDone(std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                  &namedProducts);

    std::vector<std::unique_ptr<Product>> collectReadyProducts();

    void notifyWorker() { workerNotifier->notify_one(); }
    void notifyClient() { clientNotifier->notify_one(); }

    void returnProducts(machine_wrappers_t &machines);

    [[nodiscard]] size_t getId() const;
    [[nodiscard]] bool isReady();
    [[nodiscard]] std::vector<std::string> getProductNames();

    void updateIfExpired();
    bool checkIfPending();

    OrderStatus getStatus();
    void setStatus(OrderStatus newStatus);

    std::shared_ptr<std::mutex> getMutex() { return orderInfoMutex; }
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
    enum ACTION {
        FAIL,
        COLLECT,
        ABANDON
    };

    void updateOrder(Order &order, ACTION a);

    void updateFailedProduct(std::string &product) {
        report.failedProducts.push_back(product);
    }

    WorkerReport getReport() { return report; }
};


class OrderQueue {
private:
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<std::shared_ptr<Order>> d_queue;
public:

    void pushOrder(std::shared_ptr<Order> &value);
    std::shared_ptr<Order> popOrder(std::unique_lock<std::mutex> &lock);

    bool isEmpty();

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
    product_promise_t popPromise(std::atomic_bool &ended);
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

    void insertToQueue(std::promise<std::unique_ptr<Product>> &promise) {
        machineQueue.pushPromise(promise);
    }

    void returnProduct(std::unique_ptr<Product> &product) {
        std::unique_lock<std::mutex> lock(returnProductMutex);
        machine->returnProduct(std::move(product));
    }

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

    void machineWorker() {
        /* Obsługa maszyny trwa do momentu, aż wszystkie wątki kompletujące
         * zamówienia zakończą pracę. */
        while (!orderWorkersEnded) {
            /* Czekamy na kolejce, wyciągamy polecenie wyprodukowania
             * produktu. */
            std::promise<std::unique_ptr<Product>>
                    promise = machineQueue.popPromise(orderWorkersEnded);

            /* Sytuacja, w której wyszliśmy z czekania z kolejki, bo wszyscy
             * pracownicy od zamówień skończyli pracę i nie ma już żadnej
             * pracy do wykonania.  */
            if (orderWorkersEnded) {
                break;
            }

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

        if (status == ON) {
            stopMachine();
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
    /* Zakładamy, że wszystkie nazwy produktów w zamówieniu są poprawne. */
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
    std::shared_ptr<Order> order;

    explicit CoasterPager(std::shared_ptr<Order> &order) :
            order(order) {}

    std::shared_ptr<Order> getOrder() { return order; }

public:

    void wait() const { order->waitForOrder(); }

    void wait(unsigned int timeout) const {
        order->waitForOrderTimed(timeout);
    }

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
    std::mutex menuMutex;

    OrderSynchronizer orderSynchronizer;
    std::map<size_t, std::shared_ptr<Order>> orders;
    OrderQueue orderQueue;

    std::vector<std::thread> orderWorkers;
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