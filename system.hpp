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

class WorkerReportUpdater;

class MachineWrapper;

/* Klasa reprezentująca zamówienia. */
class Order {
public:
    /* Możliwy status zamówienia. */
    enum OrderStatus {
        NOT_DONE,
        BROKEN_MACHINE,
        READY,
        RECEIVED,
        EXPIRED,
    };
private:
    typedef std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> machine_wrappers_t;
    size_t id;
    unsigned int timeout;
    std::vector<std::string> products;
    OrderStatus status;
    std::chrono::time_point<std::chrono::system_clock> doneTime;
    std::shared_ptr<std::mutex> orderInfoMutex;
    std::shared_ptr<std::condition_variable> clientNotifier;
    std::shared_ptr<std::condition_variable> workerNotifier;
    std::vector<std::pair<std::string, std::unique_ptr<Product>>> readyProducts;
public:
    Order(size_t id,
          std::vector<std::string> &products,
          unsigned int timeout) :
            id(id),
            timeout(timeout),
            products(products),
            status(NOT_DONE),
            orderInfoMutex(std::make_shared<std::mutex>()),
            clientNotifier(std::make_shared<std::condition_variable>()),
            workerNotifier(std::make_shared<std::condition_variable>()) {}

    /* Funkcja wywoływana po stronie klienta.
     * Blokuje do czasu zmienienia statusu zamówienia. */
    void waitForOrder();

    /* Analogiczna funkcja jak wyżej, blokująca maksymalnie na x czasu. */
    void waitForOrderTimed(unsigned int time);

    /* Funkcja wywoływana po stronie pracownika.
     * Blokuje pracownika na maksymalnie timeout, w tym czasie klient
     * powinien odebrać swoje zamówienie. */
    void waitForClient(WorkerReportUpdater &infoUpdater, machine_wrappers_t
    &machines);

    /* Funkcja po stronie pracownika, oznaczenie zamówienia jako gotowe. */
    void markOrderDone(std::vector<std::pair<std::string,
            std::unique_ptr<Product>>> &namedProducts);

    /* Funkcja po stronie klienta do odebrania gotowego zamówienia. */
    std::vector<std::unique_ptr<Product>> collectReadyProducts();

    /* Powiadomienie pracownika. */
    void notifyWorker() { workerNotifier->notify_one(); }

    /* Powiadomienie klienta. */
    void notifyClient() { clientNotifier->notify_one(); }

    /* Funkcja po stronie klienta do odebrania zamówienia, z możliwością
     * rzucenia wyjątków. */
    std::vector<std::unique_ptr<Product>> tryToCollectOrder();

    /* Funkcja po stronie pracownika, zwraca stworzone produkty do
     * maszyn w przypadku awarii zamówienia.  */
    void returnProducts(machine_wrappers_t &machines);

    /* Zwrócenie id. */
    [[nodiscard]] size_t getId() const { return id; }

    /* Sprawdzenie, czy zamówienie jest gotowe. */
    [[nodiscard]] bool isReady();

    /* Funkcja zwracająca wszystkie nazwy produktów w zamówieniu. */
    [[nodiscard]] std::vector<std::string> getProductNames();

    /* Funkcja sprawdzająca, czy upłynął czas na odebranie zamówienia przez
     * klienta. Jeśli tak, aktualizuje status. */
    void updateIfExpired();

    /* Funkcja sprawdzająca, czy zamówienie powinno zostać zwrócone w
     * przypadku wywołania getPendingOrders(). */
    bool checkIfPending();

    /* Zwrócenie statusu. */
    OrderStatus getStatus();

    /* Ustawienie statusu.*/
    void setStatus(OrderStatus newStatus) { status = newStatus; }

    /* Ustawienie statusu pod mutexem ochrony zmiennych. */
    void setStatusLocked(OrderStatus newStatus);

    /* Zwrócenie mutexu ochrony zmiennych. */
    std::shared_ptr<std::mutex> getInfoMutex() { return orderInfoMutex; }
};


struct WorkerReport {
    std::vector<std::vector<std::string>> collectedOrders;
    std::vector<std::vector<std::string>> abandonedOrders;
    std::vector<std::vector<std::string>> failedOrders;
    std::vector<std::string> failedProducts;
};

/* Klasa pomocnicza do aktualizowania raportów pracownika. */
class WorkerReportUpdater {
private:
    struct WorkerReport report;
public:
    enum ACTION {
        FAIL,
        COLLECT,
        ABANDON
    };

    /* Funkcje aktualizujące raport. */
    void updateReport(Order &order, ACTION a);

    void updateReport(std::string &product) {
        report.failedProducts.push_back(product);
    }

    /* Umieszczenie raportu do kontenera wszystkich raportów. */
    void addReport(std::vector<WorkerReport> &workerReports) {
        workerReports.push_back(report);
    }
};

/* Kolejka blokująca na zamówienia. W przypadku pustej kolejki, wątki
 * reprezentujące pracowników śpią na zmiennej warunkowej. Wrzucenie nowego
 * zamówienia do kolejki powoduje obudzenie lub brak blokowania wątku
 * pracownika. */
class OrderQueue {
private:
    std::mutex queMutex;
    std::condition_variable queCondition;
    std::queue<std::shared_ptr<Order>> que;
public:
    /* Umieszczenie zamówienia w kolejce. */
    void pushOrder(std::shared_ptr<Order> value);

    /* Możliwe wyciągnięcie zamówienia z kolejki. W przypadku zamknięcia
     * systemu wątki zostają obudzone i opuszczają funkcję bez zamówienia. */
    std::optional<std::shared_ptr<Order>>
    popOrder(std::unique_lock<std::mutex> &lock,
             std::atomic_bool &systemOpen);

    /*Sprawdzenie, czy kolejka jest pusta.*/
    bool isEmpty();

    /* Powiadomienie pracowników o zamknięciu systemu. */
    void notifyShutdown() { queCondition.notify_all(); }

    /* Zwrócenie mutexu na kolejkę. */
    std::mutex &getMutex() { return queMutex; }
};

/* Kolejka blokująca na zlecenia wyprodukowania produktu w maszynie.
 * Analogiczna jak wyżej.
 * Kolejka przechowuje wskaźniki na promisy, przez które maszyna przekazuje
 * gotowy produkt.  */
class MachineQueue {
private:
    std::mutex queMutex;
    std::condition_variable queCondition;
    std::queue<promise_ptr> que;
public:
    /* Umieszczenie wskaźnika na promise w kolejce. */
    void pushPromise(promise_ptr value);

    /* Wyciągnięcie promise'u z kolejki. */
    std::optional<promise_ptr> popPromise(std::atomic_bool &ended);

    /* Powiadomienie wszystkich wątków czekających na kolejce. */
    void notify() { queCondition.notify_all(); }
};

/* Klasa pomocnicza dla maszyny. Każda maszyna posiada wątek pomocniczy, który czeka
 * na kolejce blokującej, odbiera z niej zlecenia i wytwarza produkt. */
class MachineWrapper {
public:
    /* Informacja o stanie maszyny. */
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
    /* Konstruktor rozpoczyna pracę wątku obsługującego maszynę. */
    MachineWrapper(std::string machineName,
                   std::shared_ptr<Machine> &machine,
                   std::atomic_bool &orderWorkersEnded) :
            machineName(std::move(machineName)),
            machine(std::move(machine)),
            status(OFF),
            orderWorkersEnded(orderWorkersEnded) {
        worker = std::thread([this] { machineWorker(); });
    }

    /* Umieszczenie promise'u do kolejki. */
    void insertToQueue(promise_ptr promise) {
        machineQueue.pushPromise(std::move(promise));
    }

    /* Zwrócenie produktu z nieudanego zamówienia z powrotem do maszyny.
     * W tym samym czasie może zostać zwracany tylko jeden produkt. */
    void returnProduct(std::unique_ptr<Product> &product) {
        std::unique_lock<std::mutex> lock(returnProductMutex);
        machine->returnProduct(std::move(product));
    }

    /* Powiadomienie pracowników maszyn o zamknięciu systemu. */
    void notifyShutdown() { machineQueue.notify(); }

    /* Czekanie na koniec pracy wątków maszyn.  */
    void joinMachineWorker() {
        if (worker.joinable()) {
            worker.join();
        }
    }

private:
    /* Włączenie maszyny. */
    void startMachine() {
        machine->start();
        status = ON;
    }

    /* Wyłączenie maszyny. */
    void stopMachine() {
        machine->stop();
        status = OFF;
    }

    /* Funkcja obsługująca pracę wątku pomocniczego. */
    void machineWorker();
};

/* Klasa reprezentująca pager. */
class CoasterPager {
    friend class System;

private:
    /* Pager posiada wskaźnik na swoje zamówienie. */
    std::shared_ptr<Order> order;

    explicit CoasterPager(std::shared_ptr<Order> order) : order(
            std::move(order)) {}

public:
    void wait() const { order->waitForOrder(); }

    void wait(unsigned int timeout) const { order->waitForOrderTimed(timeout); }

    [[nodiscard]] unsigned int getId() const { return order->getId(); };

    [[nodiscard]] bool isReady() const { return order->isReady(); };
};

/* Główna klasa reprezentująca system restauracji. */
class System {
    typedef std::unordered_map<std::string,
            std::shared_ptr<MachineWrapper>> machine_wrappers_t;
private:
    /* Generator id zamówienia. */
    class IdGenerator {
        size_t id{1};
    public:
        size_t newId() { return id++; }
    };
    IdGenerator idGenerator;
    unsigned int clientTimeout;
    std::atomic_bool systemOpen;

    machine_wrappers_t machines;

    std::set<std::string> menu;
    mutable std::mutex menuMutex;

    std::map<size_t, std::shared_ptr<Order>> orders;
    mutable std::mutex ordersMapMutex;
    mutable std::mutex newOrderMutex;
    OrderQueue orderQueue;

    std::vector<std::thread> orderWorkers;
    mutable std::mutex workerReportsMutex;
    std::vector<WorkerReport> workerReports;
    std::atomic_bool orderWorkersEnded;

    /* Sprawdza, czy dany produkt znajduje się w menu. */
    bool productsInMenu(std::vector<std::string> &products);

    /* Usuwa pozycję z menu. */
    void removeFromMenu(std::string &product);

    /* Funkcja pracownika kompletującego zamówienia. */
    void orderWorker();

public:
    typedef std::unordered_map<std::string, std::shared_ptr<Machine>> machines_t;
    /* Konstruktor systemu. Opakowuje maszyny we wrappery, uruchamia wątki
     * pracowników, obsługujących zamówienia. */
    System(machines_t machines, unsigned int numberOfWorkers,
           unsigned int clientTimeout);

    /* Zamknięcie systemu. Zakładamy, że ta funkcja musi zostać wywołana
     * na koniec działania programu. */
    std::vector<WorkerReport> shutdown();

    /* Zwraca menu. */
    std::vector<std::string> getMenu() const;

    /* Zwraca zamówienia w realizacji. */
    std::vector<unsigned int> getPendingOrders() const;

    /* Składa nowe zamówienie. */
    std::unique_ptr<CoasterPager> order(std::vector<std::string> products);

    /* Odbiera gotowe zamówienie, w przypadku awarii zwraca wyjątek. */
    std::vector<std::unique_ptr<Product>>
    collectOrder(std::unique_ptr<CoasterPager> CoasterPager);

    /* Zwraca maksymalny czas, w jakim można odebrać gotowe zamówienie. */
    unsigned int getClientTimeout() const { return clientTimeout; }

};


#endif // SYSTEM_HPP