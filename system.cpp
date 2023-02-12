#include <algorithm>
#include "system.hpp"

template<typename T>
void BlockingQueue<T>::push(T const &value) {
    {
        std::unique_lock<std::mutex> lock(d_mutex);
        d_queue.push_front(value);
    }
    d_condition.notify_one();
}

template<typename T>
T BlockingQueue<T>::popOrder(MachinesSynchronizer &sync) {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    T order(std::move(d_queue.back()));
    d_queue.pop_back();

    sync.insertOrder(order);

    return order;
}

template<typename T>
T BlockingQueue<T>::pop() {
    std::unique_lock<std::mutex> lock(d_mutex);
    d_condition.wait(lock, [=] { return !d_queue.empty(); });
    T id(std::move(d_queue.back()));
    d_queue.pop_back();

    return id;
}


void System::closeMachines() {
    for (auto &machine: machines) {
        machine.second->stop();
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        clientTimeout(clientTimeout),
        systemOpen(true) {

    //map all elements of machines to wrappers and initialize them
    for (auto &machine: machines) {
        this->machines.emplace(machine.first,
                               std::make_shared<MachineWrapper>(
                                       machine.first,
                                       machine.second,
                                       systemOpen));
    }

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        orderWorkers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    systemOpen = true;

    for (auto &w: orderWorkers) {
        if (w.joinable()) {
            w.join();
        }
    }

    closeMachines();
    std::vector<WorkerReport> dailyReport;

    for (auto &r: workerReports) {
        dailyReport.push_back(r);
    }

    return dailyReport;
}

std::vector<std::string> System::getMenu() const {
    return systemOpen ? std::vector<std::string>() :
           std::vector<std::string>(menu.begin(), menu.end());
}


std::vector<unsigned int> System::getPendingOrders() const {

    std::chrono::system_clock::time_point now
            = std::chrono::system_clock::now();
    std::vector<unsigned int> pendingOrders;

    for (const auto &element: orders) {
        auto order = element.second;
        if (order.checkIfPending()) {
            pendingOrders.push_back(order.getId());
        }
    }
    return pendingOrders;
}


std::unique_ptr<CoasterPager> System::order(std::vector<std::string> products) {
    if (systemOpen) {
        throw RestaurantClosedException();
    }

    if (!productsInMenu(products) || products.empty()) {
        throw BadOrderException();
    }

    size_t newId = idGenerator.newId();
    Order newOrder(newId, products, clientTimeout);
    orders.emplace(newId, newOrder);

    CoasterPager newPager(newId, newOrder);
    pagers.emplace(newId, newPager);

    orderQueue.push(newOrder);

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
        case Order::RECEIVED: {
            throw BadPagerException();
        }
        case Order::READY: {
            // Przypadki brzegowe itp.
            // zwrócenie produktu
            return {};
        }
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

void System::orderWorker() {
    while (!systemOpen) {

    }
}

void System::machineWorker() {

}

