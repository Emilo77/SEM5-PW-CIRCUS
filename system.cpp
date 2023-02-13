#include <algorithm>
#include "system.hpp"

typedef std::promise<std::unique_ptr<Product>> product_promise_t;
typedef std::future<std::unique_ptr<Product>> product_future_t;

void OrderQueue::pushOrder(Order &order) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(std::move(order));
    }
    d_condition.notify_one();
}

Order OrderQueue::popOrder(std::unique_lock<std::mutex> &lock) {

    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    Order order(std::move(d_queue.back()));
    d_queue.pop_back();

    return order;
}

bool OrderQueue::isEmpty() {
    std::unique_lock<std::mutex> lock(d_mutex);
    return d_queue.empty();
}


void MachineQueue::pushPromise(product_promise_t &value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(std::move(value));
    }
    d_condition.notify_one();
}


product_promise_t MachineQueue::popPromise(std::atomic_bool
                                           &orderWorkersEnded) {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [this, &orderWorkersEnded] {
        return !d_queue.empty() || orderWorkersEnded;
    });
    if (orderWorkersEnded) {
        return {};
    }
    product_promise_t promise(std::move(d_queue.back()));
    d_queue.pop_back();

    return promise;
}

void System::removeFromMenu(std::string &product) {
    std::unique_lock<std::mutex> lock(menuMutex);
    auto it = std::find(menu.begin(), menu.end(), product);
    if (it != menu.end()) {
        menu.erase(it);
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        clientTimeout(clientTimeout),
        systemOpen(true),
        orderWorkersEnded(false) {

    //map all elements of machines to wrappers and initialize them
    for (auto &machine: machines) {
        this->machines.emplace(machine.first,
                               std::make_shared<MachineWrapper>(
                                       machine.first,
                                       machine.second,
                                       orderWorkersEnded));

    }

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        orderWorkers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    systemOpen = false;

    for (auto &w: orderWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }

    orderWorkersEnded = true;

    return workerReports;
}

std::vector<std::string> System::getMenu() const {
    return systemOpen ? std::vector<std::string>() :
           std::vector<std::string>(menu.begin(), menu.end());
}


std::vector<unsigned int> System::getPendingOrders() const {

    std::chrono::system_clock::time_point now
            = std::chrono::system_clock::now();
    std::vector<unsigned int> pendingOrders;

    for (auto &element: orders) {
        const Order &order = element.second;
        if (order.checkIfPending()) {
            pendingOrders.push_back(order.getId());
        }
    }
    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    if (!systemOpen) {
        throw RestaurantClosedException();
    }

    if (products.empty() || !productsInMenu(products)) {
        throw BadOrderException();
    }

    size_t newId = idGenerator.newId();
    Order newOrder(newId, products, clientTimeout);


    CoasterPager newPager(newOrder);
    pagers.emplace(newId, newPager);

    orders.emplace(newId, std::move(newOrder));
    orderQueue.pushOrder(newOrder);

    return std::make_unique<CoasterPager>(newPager);
}


std::vector<std::unique_ptr<Product>>
System::collectOrder(std::unique_ptr<CoasterPager> CoasterPager) {
    auto orderStatus = CoasterPager->getOrder().getStatus();
    switch (orderStatus) {
        case Order::BROKEN_MACHINE: {
            throw FulfillmentFailure();
        }
        case Order::NOT_DONE: {
            throw OrderNotReadyException();
        }
        case Order::EXPIRED: {
            throw OrderExpiredException();
        }
        case Order::READY: {
            // Przypadki brzegowe itp.
            // zwrócenie produktu
            return {};
        }
        case Order::RECEIVED: {
            throw BadPagerException();
        }
        case Order::OTHER:
            throw BadPagerException();
    }
}


bool System::productsInMenu(std::vector<std::string> &products) {
    std::unique_lock<std::mutex> lock(menuMutex);
    return std::ranges::all_of(products.begin(),
                               products.end(),
                               [this](std::string &product) {
                                   return menu.contains(product);
                               });
}


void CoasterPager::wait() const {

}


void CoasterPager::wait(unsigned int timeout) const {

}

void System::returnProducts(
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
        &readyProducts) {
    for (auto &product: readyProducts) {
        machines.at(product.first)->returnProduct(product.second);
    }
}

void System::orderWorker() {
    WorkerReportUpdater infoUpdater;

    /* Pracownik wykonuje pracę, dopóki system nie zamknie się i kolejka nie
     * będzie pusta. */
    while (systemOpen || !orderQueue.isEmpty()) {

        /* Zablokowanie kolejki zamówień. */
        std::unique_lock<std::mutex> lock(orderQueue.getMutex());

        /* Odebranie zamówienia z kolejki blokującej. */
        Order order = orderQueue.popOrder(lock);

        /* Zlecenie maszynom wyprodukowanie produktów. */
        auto futureProducts = orderSynchronizer.insertOrderToMachines(order,

                                                                      machines);
        /* Odblokowanie kolejki zamówień. */
        lock.unlock();

        /* Wyprodukowane produkty przez maszyny. */
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                readyProducts;

        for (auto &futureProduct: futureProducts) {
            std::string productName = futureProduct.first;
            try {
                auto product = futureProduct.second.get();

                /* Jeżeli wyprodukowanie powiodło się, dodanie do kontenera.*/
                readyProducts.emplace_back(productName, std::move(product));

            } catch (BadProductException &e) {
                removeFromMenu(productName);
                /* W przypadku awarii maszyny, zaznaczenie informacji. */
                order.setStatus(Order::BROKEN_MACHINE);
                infoUpdater.updateFailedProduct(productName);
            }
        }

        if (order.getStatus() == Order::BROKEN_MACHINE) {
            infoUpdater.updateFailedOrder(order);
            /* Zwrócenie niewykorzystanych produktów do odpowiednich maszyn. */
            returnProducts(readyProducts);
        } else {
            order.setStatus(Order::READY);
            //budzenie osoby czekającej na CoasterPager

            //czekanie na klienta przez timeout milisekund
            //jeśli się pojawi, uznanie zamówienia za skończone
            //send order
            order.setStatus(Order::RECEIVED);
            infoUpdater.updateCollectedOrder(order);

            //jeśli nie, uznanie zamówienia za abandoned
            order.setStatus(Order::EXPIRED);
            infoUpdater.updateAbandonedOrder(order);

        }

        // ...

    }

    workerReports.push_back(infoUpdater.getReport());
}


