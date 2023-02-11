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
        NOT_FOUND
    };
private:
    size_t id;
    vector <std::string> products;
    unsigned int timeout;
    OrderStatus status;
    std::mutex &mutex;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
public:
    explicit Order(unsigned int id,
                   vector <std::string> &products,
                   unsigned int timeout,
                   std::mutex &mutex) :
            id(id),
            products(products),
            timeout(timeout),
            status(NOT_DONE),
            mutex(mutex) {}

    void markDone() {
        std::unique_lock<std::mutex> lock(mutex);
        doneTime = std::chrono::system_clock::now();
        status = READY;
    }

    [[nodiscard]] size_t getId() const {
        std::unique_lock<std::mutex> lock(mutex);
        return id;
    }

    [[nodiscard]] vector <std::string> getProducts() const {
        std::unique_lock<std::mutex> lock(mutex);
        return products;
    }

    [[nodiscard]] bool isReady() const {
        std::unique_lock<std::mutex> lock(mutex);
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
        std::unique_lock<std::mutex> lock(mutex);
        checkIfExpired();

        if (status == READY) {
            return true;
        }
        return false;
    }

    OrderStatus getStatus() {
        std::unique_lock<std::mutex> lock(mutex);
        checkIfExpired();

        return status;
    }
};

class MachinesSynchronizer;


template<typename T>
class BlockingQueue {
private:
    std::mutex d_mutex;
    std::condition_variable d_condition;
    std::deque<T> d_queue;
public:
    void push(T const &value) {
        {
            std::unique_lock<std::mutex> lock(d_mutex);
            d_queue.push_front(value);
        }
        d_condition.notify_one();
    }

    T popOrder(MachinesSynchronizer &sync) {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_condition.wait(lock, [=] { return !d_queue.empty(); });
        T order(std::move(d_queue.back()));
        d_queue.pop_back();

        sync.insertOrder(order);

        return order;
    }

    T popId() {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_condition.wait(lock, [=] { return !d_queue.empty(); });
        T id(std::move(d_queue.back()));
        d_queue.pop_back();

        return id;
    }
};

class MachinesSynchronizer {
private:
    std::unordered_map<std::string, std::deque<size_t>> ques; //tu zamienić
    // na blocking queue
    std::condition_variable condition;
    std::mutex mutex;
public:
    void initialize(const std::unordered_map<std::string,
            std::shared_ptr<Machine>> &machines) {
        std::unique_lock<std::mutex> lock(mutex);
        for (auto &machine: machines) {
            ques.emplace(machine.first, std::deque<size_t>());
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

    std::unique_ptr<Product> waitAndGetProduct(size_t orderId,
                           const std::string &productStr,
                           const std::shared_ptr<Machine> &machine) {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock,
                       [=] { return ques.at(productStr).back() == orderId; });
        try {
            lock.unlock();
            unique_ptr <Product> product = machine->getProduct();
            lock.lock();

            ques.at(productStr).pop_back();
            condition.notify_one();

            return product;

        } catch (const MachineFailure &e) {
            throw e;
        }
    }

    void removeOrder(size_t orderId) {
        for (auto &que: ques) {
            while (!que.second.empty()) {
                for(auto it = que.second.begin(); it != que.second.end(); ++it)
                    if (*it == orderId)
                        que.second.erase(it);
            }
        }
    }

    void returnProducts(size_t orderId,
                        const std::unordered_map<std::string,
                        std::shared_ptr<Machine>> &machines,
                        std::vector<std::pair<std::string,
                        std::unique_ptr<Product>>>  &doneProducts) {

        std::unique_lock<std::mutex> lock(mutex);
        for (auto &pair: doneProducts) {
            std::string productStr = pair.first;
            auto product = std::move(pair.second);
            //todo może trzeba maszynę opakować mutexem
            machines.at(productStr)->returnProduct(std::move(product));
        }

        removeOrder(orderId);
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
    unsigned int numberOfWorkers;
    unsigned int clientTimeout;

    IdGenerator idGenerator;
    std::map<size_t, Order> orders;
    BlockingQueue<Order> orderQueue;
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
    System(machines_t machines, unsigned int numberOfWorkers,
           unsigned int clientTimeout);

    void informClosed();

    std::vector<WorkerReport> shutdown();

    std::vector<std::string> getMenu() const {
        return closed ? vector<string>() : vector<string>(menu.begin(),
                                                          menu.end());
    }

    std::vector<unsigned int> getPendingOrders() const;

    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    std::vector<std::unique_ptr<Product>>
    collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    unsigned int getClientTimeout() const { return clientTimeout; }

    bool productsInMenu(std::vector<string> &products);

private:
    void initializeMachines(machines_t &machines);

    void closeMachines();

    void orderWorker();

    void machineWorker();
};


#endif // SYSTEM_HPP