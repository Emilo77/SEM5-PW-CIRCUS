#include <algorithm>
#include "system.hpp"

void System::initializeMachines(machines_t &m) {
    for (auto &machine: m) {
        machineStatus.emplace(machine.first, MachineStatus::OFF);
    }
    mSynchronizer.initialize(m);
}


void System::closeMachines() {
    for (auto &machine: machines) {
        machine.second->stop();
    }
}


System::System(machines_t machines,
               unsigned int numberOfWorkers,
               unsigned int clientTimeout) :
        numberOfWorkers(numberOfWorkers),
        clientTimeout(clientTimeout),
        closed(false),
        machines(std::move(machines)) {

    initializeMachines(machines);

    for (auto &machine: machines) {
        menu.emplace(machine.first);
    }

    for (unsigned int i = 0; i < numberOfWorkers; i++) {
        workers.emplace_back([this]() { orderWorker(); });
    }
}


std::vector<WorkerReport> System::shutdown() {
    closed = true;

    for (auto &w: workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    closeMachines();
    std::vector<WorkerReport> dailyReport;

    for (auto &r: reports) {
        dailyReport.push_back(r);
    }

    return dailyReport;
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
    if (closed) {
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
    while (!closed) {
        Order order = orderQueue.popOrder(mSynchronizer);
        std::vector<std::pair<std::string, std::unique_ptr<Product>>>
                doneProducts;

//        for (auto &product: order.getProducts()) {
//            auto machine = machines.find(product);
//            if (machine != machines.end()) {
//                try {
//                    std::unique_ptr<Product> newProduct = mSynchronizer
//                            .waitAndGetProduct(order.getId(),
//                                                    machine->first,
//                                                    machine->second);
//                    doneProducts.emplace_back(machine->first, std::move(newProduct));
//                } catch (MachineFailure &e) {
//                    // zwrócenie produktów
//
//                }
//
//            } else {
//                mSynchronizer.returnProducts(order.getId(), machines,
//                                             doneProducts);
//                order.setStatus(Order::NOT_FOUND);
//            }
//        }

    }
}

void System::machineWorker() {

}

